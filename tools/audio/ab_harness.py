#!/usr/bin/env python3
"""Live side-by-side audio A/B harness (docs/audio_ab_harness.md).

Runs the native build and the oracle (DISABLE_RECOMP) simultaneously, drives both
through the same input script at the same GAME-PROGRESS points (event triggers, not
wall clock), aligns their SUNBRIGHT_AB_EVENTS voice streams per wave hash, and stops
at the first real divergence with a cause report.

Usage:
  tools/audio/ab_harness.py [--secs 240] [--script tools/audio/ab_script_delfino.json]
                            [--no-stop] [--window 4.0]
"""
import argparse, json, math, os, signal, subprocess, sys, time, urllib.request
from collections import defaultdict, deque

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGS = os.path.join(REPO, 'scratch', 'logs')

PITCH_CENTS = 50.0
GAIN_DB = 8.0
LIFE_RATIO = 3.0
MIN_MATCHES_FOR_GAIN = 8   # need calibration before judging gains


def cents(a, b):
    return 1200.0 * math.log2(a / b) if a > 0 and b > 0 else float('nan')


class Side:
    def __init__(self, name, port, env):
        self.name, self.port = name, port
        self.events_path = os.path.join(LOGS, f'ab_{name}.events.jsonl')
        self.log_path = os.path.join(LOGS, f'ab_{name}.log')
        for p in (self.events_path,):
            if os.path.exists(p):
                os.remove(p)
        e = dict(os.environ)
        e.update(SUNBRIGHT_HEADLESS='1', SUNBRIGHT_AUTOSTART='1', SUNBRIGHT_PROBE='1',
                 SUNBRIGHT_PROBE_PORT=str(port), SUNBRIGHT_AB_EVENTS=self.events_path)
        e.update(env)
        self.proc = subprocess.Popen([os.path.join(REPO, 'build', 'sunbright')],
                                     env=e, cwd=REPO,
                                     stdout=open(self.log_path, 'w'),
                                     stderr=subprocess.STDOUT)
        self.fh = None
        self.script_idx = 0
        self.t_last = 0.0

    def events(self):
        """Yield newly appended events."""
        if self.fh is None:
            if not os.path.exists(self.events_path):
                return
            self.fh = open(self.events_path)
        for line in self.fh:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            self.t_last = ev.get('t', self.t_last)
            yield ev

    def pad(self, combo, ms):
        try:
            urllib.request.urlopen(
                f'http://127.0.0.1:{self.port}/pad?do={combo}&ms={ms}', timeout=2).read()
        except Exception:
            pass

    def probe(self, path):
        try:
            return urllib.request.urlopen(
                f'http://127.0.0.1:{self.port}{path}', timeout=3).read().decode()
        except Exception as e:
            return f'<probe error: {e}>'

    def freeze(self):
        try:
            self.proc.send_signal(signal.SIGSTOP)
        except Exception:
            pass

    def kill(self):
        for sig in (signal.SIGCONT, signal.SIGKILL):
            try:
                self.proc.send_signal(sig)
            except Exception:
                pass


def trigger_hit(ev, trig):
    if ev.get('ev') != trig.get('ev'):
        return False
    return all(str(ev.get(k)) == str(v) for k, v in trig.items() if k != 'ev')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--secs', type=float, default=240)
    ap.add_argument('--script', default=os.path.join(REPO, 'tools/audio/ab_script_delfino.json'))
    ap.add_argument('--window', type=float, default=4.0)
    ap.add_argument('--no-stop', action='store_true')
    args = ap.parse_args()

    os.makedirs(LOGS, exist_ok=True)
    script = json.load(open(args.script))
    residuals = []
    rp = os.path.join(REPO, 'tools/audio/ab_residuals.json')
    if os.path.exists(rp):
        residuals = json.load(open(rp))

    native = Side('native', 17654, {})
    oracle = Side('oracle', 17655, {'SUNBRIGHT_DISABLE_RECOMP': '1',
                                    'SUNBRIGHT_BACKEND': 'OGL',
                                    'SUNBRIGHT_AB_ORACLE': '1'})
    sides = {'native': native, 'oracle': oracle}
    print(f'[ab] native pid={native.proc.pid}  oracle pid={oracle.proc.pid}')

    # per-hash queues of unmatched events, per side
    pend = {'native': defaultdict(deque), 'oracle': defaultdict(deque)}
    offset = None          # oracle.t - native.t, EMA over matches
    matched = deque(maxlen=12)
    gain_ratios = []       # oracle_peak / native_peak over matched voffs
    divergences = []

    def is_residual(klass, h):
        return any(not r.get('_disabled') and r.get('class') == klass
                   and r.get('hash') in (h, '*') for r in residuals)

    def report(klass, detail, nat_ev=None):
        if is_residual(klass, (nat_ev or {}).get('hash', detail.get('hash', ''))):
            return False
        d = dict(klass=klass, **detail)
        divergences.append(d)
        print(f'\n=== DIVERGENCE: {klass} ===')
        print(json.dumps(d, indent=2))
        print('--- last matched events (in-sync context) ---')
        for m in matched:
            print('   ', json.dumps(m))
        if not args.no_stop:
            native.freeze(); oracle.freeze()
            print('--- native /njas ---'); print(native.probe('/njas'))
            print('--- oracle /vpb (head) ---')
            print('\n'.join(oracle.probe('/vpb').splitlines()[:20]))
            print(f'[ab] both instances SIGSTOPped (pids {native.proc.pid}/{oracle.proc.pid})'
                  ' — inspect, then kill manually.')
            return True
        return False

    t0 = time.time()
    stop = False
    try:
        while not stop and time.time() - t0 < args.secs:
            time.sleep(0.2)
            for sname, side in sides.items():
                if side.proc.poll() is not None:
                    print(f'[ab] {sname} exited rc={side.proc.returncode}')
                    stop = True
                for ev in side.events():
                    # progress-based input driving (each side at its own pace); the
                    # oracle has no native anchors — steps may carry an oracle_trigger
                    # (e.g. the von hash of the scene BGM's first note)
                    if side.script_idx < len(script):
                        step = script[side.script_idx]
                        trig = step['trigger'] if sname == 'native' else \
                               step.get('oracle_trigger', step['trigger'])
                        if trigger_hit(ev, trig):
                            side.script_idx += 1
                            for act in step.get('actions', []):
                                side.pad(act['do'], act.get('ms', 500))
                            print(f'[ab] {sname}: step {side.script_idx} '
                                  f'({step.get("name","?")}) fired')
                    if ev['ev'] not in ('von', 'voff'):
                        continue
                    # phase-scoped: events are only comparable within the same script
                    # phase (scene); cross-scene comparison floods false MISSING/EXTRA
                    pend[sname][(side.script_idx, ev['ev'], ev['hash'])].append(ev)

            # matching pass: pair oracle/native per (kind, hash) FIFO
            for key in list(set(pend['native']) | set(pend['oracle'])):
                qn, qo = pend['native'][key], pend['oracle'][key]
                while qn and qo:
                    n, o = qn.popleft(), qo.popleft()
                    if offset is None:
                        offset = o['t'] - n['t']
                    offset = 0.95 * offset + 0.05 * (o['t'] - n['t'])
                    matched.append({'hash': n['hash'], 'kind': key[1],
                                    'nat_t': n['t'], 'ora_t': o['t']})
                    if key[1] == 'von':
                        dc = cents(n.get('ratio', 0), o.get('ratio', 0))
                        if dc == dc and abs(dc) > PITCH_CENTS:
                            stop |= report('PITCH', {'hash': n['hash'], 'cents': round(dc, 1),
                                                     'native': n, 'oracle': o}, n)
                    else:
                        if o.get('peak', 0) > 0 and n.get('peak', 0) > 0:
                            gain_ratios.append(o['peak'] / n['peak'])
                        if len(gain_ratios) >= MIN_MATCHES_FOR_GAIN:
                            med = sorted(gain_ratios)[len(gain_ratios) // 2]
                            if o.get('peak', 0) > 0 and n.get('peak', 0) > 0:
                                db = 20 * math.log10((n['peak'] * med) / o['peak'])
                                if abs(db) > GAIN_DB:
                                    stop |= report('GAIN', {'hash': n['hash'],
                                                            'db': round(db, 1),
                                                            'native': n, 'oracle': o}, n)
                        dn, do = n.get('dur', 0), o.get('dur', 0)
                        # native dur in 80-sample subframes, oracle in HLE frames (also
                        # 80-sample renders) — same unit, compare directly
                        if min(dn, do) > 20 and max(dn, do) / max(1, min(dn, do)) > LIFE_RATIO:
                            stop |= report('LIFE', {'hash': n['hash'], 'nat_dur': dn,
                                                    'ora_dur': do, 'native': n, 'oracle': o}, n)
                    if stop:
                        break
                if stop:
                    break

            # expiry — phase-aware:
            #  - phases BOTH sides have fully passed: leftovers are final divergences
            #  - the shared current phase: window-based expiry
            #  - one side ahead: no expiry (the laggard just hasn't gotten there yet)
            if not stop:
                done_phase = min(native.script_idx, oracle.script_idx)
                same_phase = native.script_idx == oracle.script_idx
                for sname, other in (('oracle', 'native'), ('native', 'oracle')):
                    klass = 'MISSING_NATIVE' if sname == 'oracle' else 'EXTRA_NATIVE'
                    horizon = None
                    if same_phase and offset is not None:
                        horizon = sides[other].t_last + \
                                  (offset if other == 'native' else -offset)
                    for key, q in pend[sname].items():
                        final = key[0] < done_phase
                        while q and (final or (same_phase and key[0] == done_phase
                                               and horizon is not None
                                               and q[0]['t'] < horizon - args.window * 1000)):
                            ev = q.popleft()
                            stop |= report(klass, {'hash': ev['hash'], 'phase': key[0],
                                                   'event': ev}, ev)
                            if stop:
                                break
                        if stop:
                            break
                    if stop:
                        break
    finally:
        if not stop or args.no_stop:
            native.kill(); oracle.kill()
    print(f'\n[ab] done: {len(divergences)} divergence(s), '
          f'{len(matched)} recent matches, offset={offset and round(offset)} ms')
    return 1 if divergences else 0


if __name__ == '__main__':
    sys.exit(main())
