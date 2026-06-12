#!/usr/bin/env python3
"""BMS (JAudio sequence) disassembler — semantics ported from the SMS decomp
JASSeqParser::mainProc / Cmd_Process / Arglist (reference/sms/src/JSystem/JAudio).

Usage: bms_dis.py <file> [start_offset_hex] [--max N]
Traces reachable code via BFS from the start offset, following jmp/call/opentrack.
Indirect (register/table) jump targets are reported but not followed unless the
jump-table form (flag&0xC0) is detected, in which case table entries are followed.
"""
import sys
from collections import deque

CMD_NAMES = {
    0xC1: "opentrack", 0xC2: "opentrackbros", 0xC4: "call", 0xC6: "ret",
    0xC8: "jmp", 0xC9: "loop_s", 0xCA: "loop_e", 0xCB: "readport",
    0xCC: "writeport", 0xCD: "checkportimport", 0xCE: "checkportexport",
    0xCF: "wait_r", 0xD0: "connectname", 0xD1: "parentwriteport",
    0xD2: "childwriteport", 0xD4: "setlastnote", 0xD5: "timerelate",
    0xD6: "simpleosc", 0xD7: "simpleenv", 0xD8: "simpleadsr", 0xD9: "transpose",
    0xDA: "closetrack", 0xDB: "outswitch", 0xDC: "updatesync", 0xDD: "busconnect",
    0xDE: "pausestatus", 0xDF: "setinterrupt", 0xE0: "disinterrupt", 0xE1: "clri",
    0xE2: "seti", 0xE3: "reti", 0xE4: "inttimer", 0xE6: "connectopen",
    # 0xE7 = syncCPU (binary-verified: one u16 arg = cmdSyncCPU's shape; every track body
    # starts with e7 00 00 = the per-track init callback). The old "connectclose @E7 /
    # synccpu @E9" naming was the sCmdPList −1-shift trap; E9 takes 0 args, name uncertain.
    0xE7: "synccpu", 0xE9: "unk_e9", 0xEA: "flushall", 0xEB: "flushrelease",
    0xEC: "wait3", 0xED: "panpowset", 0xEE: "iirset", 0xEF: "firset",
    0xF0: "extset", 0xF1: "panswset", 0xF2: "oscroute", 0xF3: "iircutoff",
    0xF4: "oscfull", 0xF5: "volumemode", 0xFA: "checkwave", 0xFB: "printf",
    0xFC: "nop", 0xFD: "tempo", 0xFE: "timebase", 0xFF: "finish",
}
# (argc, argmask) from Arglist, index = opcode-0xC0
ARGLIST = [
    (0,0x0000),(2,0x0008),(2,0x0008),(1,0x0002),(0,0x0000),(0,0x0000),
    (1,0x0000),(1,0x0002),(0,0x0000),(1,0x0001),(0,0x0000),(2,0x0000),
    (2,0x000C),(1,0x0000),(1,0x0000),(1,0x0003),(2,0x0005),(2,0x000C),
    (2,0x000C),(0,0x0000),(1,0x0000),(1,0x0000),(1,0x0000),(2,0x0008),
    (5,0x0155),(1,0x0000),(1,0x0000),(1,0x0000),(1,0x0001),(2,0x0004),
    (1,0x0000),(2,0x0008),(1,0x0000),(0,0x0000),(0,0x0000),(0,0x0000),
    (2,0x0004),(0,0x0000),(0,0x0000),(1,0x0001),(0,0x0000),(0,0x0000),
    (1,0x0002),(5,0x0000),(4,0x0055),(1,0x0002),(1,0x0002),(3,0x0000),
    (1,0x0000),(1,0x0000),(3,0x0028),(1,0x0000),(0,0x0000),(0,0x0000),
    (0,0x0000),(0,0x0000),(0,0x0000),(0,0x0000),(1,0x0001),(0,0x0000),
    (0,0x0000),(1,0x0001),(1,0x0001),(0,0x0000),
]

class Dis:
    def __init__(self, data):
        self.d = data
        self.lines = {}     # offset -> text
        self.work = deque()
        self.seen_starts = set()
        self.jumptables = set()

    def u8(self): v = self.d[self.p]; self.p += 1; return v
    def u16(self): v = int.from_bytes(self.d[self.p:self.p+2],'big'); self.p += 2; return v
    def u24(self): v = int.from_bytes(self.d[self.p:self.p+3],'big'); self.p += 3; return v

    def enqueue(self, off, why):
        if off < len(self.d) and off not in self.seen_starts:
            self.seen_starts.add(off)
            self.work.append((off, why))

    def run(self, start, maxops=200000):
        self.enqueue(start, "start")
        n = 0
        while self.work and n < maxops:
            off, why = self.work.popleft()
            self.lines.setdefault(off, f"; ---- block ({why}) ----")
            self.p = off
            while n < maxops:
                n += 1
                ip = self.p
                if ip in self.lines and ip != off:
                    break
                try:
                    stop = self.insn(ip)
                except IndexError:
                    self.lines[ip] = "<eof>"
                    break
                if stop:
                    break

    def emit(self, ip, text):
        self.lines[ip] = text

    def insn(self, ip):
        op = self.u8()
        if op < 0x80:   # note on (cmdNoteOn order: flag, velocity, then gate% + duration)
            flag = self.u8()
            vel = self.u8()
            if vel >= 0x80: velt = f"r{vel-0x80}"
            else: velt = str(vel)
            extra = ""
            if (flag & 7) == 0:
                gate = self.u8()
                dur_bytes = (flag >> 3) & 3
                durv = 0
                for _ in range(dur_bytes): durv = durv << 8 | self.u8()
                extra = f" gate={gate}% dur={durv}({dur_bytes}B)"
                chan = 0
            else:
                chan = flag & 7
            self.emit(ip, f"noteon key={op} flag={flag:02x} ch={chan} vel={velt}{extra}")
            return False
        if (op & 0xF0) == 0x80 and (op & 7) == 0:  # wait
            nb = 1 if op == 0x80 else 2
            v = 0
            for _ in range(nb): v = v << 8 | self.u8()
            self.emit(ip, f"wait {v}")
            return False
        if (op & 0xF0) == 0x80 or op == 0xF9:      # note off
            if op == 0xF9:
                b = self.u8()
                self.emit(ip, f"noteoff_r {b:02x}")
                return False
            note = op & 0xF
            rel = ""
            if op & 8:
                note -= 8
                rel = f" rel={self.u8()}"
            self.emit(ip, f"noteoff ch={note}{rel}")
            return False
        if (op & 0xF0) == 0x90:                    # timed param
            sub = op & 0xF
            tgt = self.u8()
            srcs = {0:"reg",4:"u8",8:"u8<<8",0xC:"u16"}[sub & 0xC]
            if (sub & 0xC) == 0: val = f"r{self.u8()}"
            elif (sub & 0xC) == 4: val = str(self.u8())
            elif (sub & 0xC) == 8: val = f"{self.u8()}<<8"
            else: val = str(self.u16())
            t = {0:"now",1:"t=reg",2:"t=u8",3:"t=u16"}[sub & 3]
            tv = ""
            if (sub & 3) == 1: tv = f" time=r{self.u8()}"
            elif (sub & 3) == 2: tv = f" time={self.u8()}"
            elif (sub & 3) == 3: tv = f" time={self.u16()}"
            self.emit(ip, f"timedparam[{tgt}] = {val} ({srcs},{t}){tv}")
            return False
        if (op & 0xF0) == 0xA0:                    # reg param
            self.emit(ip, self.regparam(op))
            return False
        if 0xB0 <= op <= 0xB7:                     # reg cmd
            r5 = self.u8()
            reg = (op & 8) != 0
            mask = 0
            if not reg or (op & 7):
                b = self.u8()
                # build mask like RegCmd_Process
                r4 = 3
                for i in range((op & 7) + 1):
                    if b & 0x80: mask |= r4
                    b = (b << 1) & 0xFF
                    r4 <<= 2
            self.emit(ip, f"regcmd op={r5:02x} mask={mask:04x}")
            return self.cmd(ip, r5, mask, prefix="regcmd: ")
        return self.cmd(ip, op, 0)

    def regparam(self, op):
        sub = op & 0xF
        if sub == 0xB:
            tgt = self.u8(); src = self.u8()
            return f"regparam: r{tgt} = r{src} (mode B)"
        text = f"regparam {sub:x}: "
        if sub == 0xA:
            p2 = self.u8(); tgt = self.u8(); tbl = self.u8()
            extra = {0:"reg",4:"u8",8:"u8<<8",0xC:"u16"}[p2 & 0xC]
            if (p2 & 0xC) == 0: idx = f"r{self.u8()}"
            elif (p2 & 0xC) == 4: idx = str(self.u8())
            elif (p2 & 0xC) == 8: idx = f"{self.u8()}<<8"
            else: idx = str(self.u16())
            return text + f"r{tgt} = tbl[r{tbl}][{idx}] elem={(p2>>4)+4} ({extra})"
        if sub == 9:
            ext = self.u8(); tgt = self.u8()
            return text + f"ext={ext:02x} r{tgt} (mode 9)"
        tgt = self.u8()
        mode = sub & 0xC
        if mode == 0: val = f"r{self.u8()}"
        elif mode == 4: val = str(self.u8())
        elif mode == 8: val = f"{self.u8()}<<8|<<1"
        else: val = str(self.u16())
        oper = {0:"=",1:"+=",2:"mul",3:"cmp"}[sub & 3]
        return text + f"r{tgt} {oper} {val}"

    def cmd(self, ip, op, mask, prefix=""):
        name = CMD_NAMES.get(op, f"cmd_{op:02x}")
        if op == 0xC4 or op == 0xC8:   # call / jmp: custom arg reading
            flag = self.u8()
            cond = flag & 0xF
            kind = "call" if op == 0xC4 else "jmp"
            if flag & 0x80:
                r = self.u8()
                if flag & 0x40:
                    if flag & 0x20:
                        r2 = self.u8()
                        self.emit(ip, f"{prefix}{kind}.tbl[r{r2}+r{r}*3] cond={cond}")
                    else:
                        tbl = self.u24()
                        self.emit(ip, f"{prefix}{kind}.tbl@{tbl:06x}[r{r}*3] cond={cond}")
                        self.jumptables.add((tbl, ip))
                else:
                    self.emit(ip, f"{prefix}{kind}.reg r{r} cond={cond}")
            else:
                off = self.u24()
                self.emit(ip, f"{prefix}{kind} {off:06x} cond={cond}")
                self.enqueue(off, f"{kind} from {ip:06x}")
                if op == 0xC8 and cond == 0:
                    return True
            return False
        if op == 0xFB:  # printf: read string + regs
            s = bytearray()
            cnt = 0
            while True:
                c = self.u8()
                if c == 0: break
                if c == ord('\\'):
                    c2 = self.u8()
                    if c2 == 0: break
                    s += b'\\' + bytes([c2]); continue
                if c == ord('%'):
                    c2 = self.u8()
                    s += b'%' + bytes([c2])
                    if c2 in b'dxsrRt': cnt += 1
                    continue
                s.append(c)
            regs = [self.u8() for _ in range(cnt)]
            self.emit(ip, f'printf "{s.decode("latin1")}" regs={regs}')
            return False
        if op < 0xC0 or op > 0xFF:
            self.emit(ip, f"<bad op {op:02x}>")
            return True
        argc, argmask = ARGLIST[op - 0xC0]
        argmask |= mask
        args = []
        m = argmask
        for _ in range(argc):
            k = m & 3
            if k == 0: args.append(str(self.u8()))
            elif k == 1: args.append(str(self.u16()))
            elif k == 2: args.append(f"{self.u24():06x}")
            else: args.append(f"r{self.u8()}")
            m >>= 2
        self.emit(ip, f"{prefix}{name} {' '.join(args)}")
        if op in (0xC1, 0xC2) and len(args) == 2 and not args[1].startswith('r'):
            try: self.enqueue(int(args[1], 16), f"opentrack from {ip:06x}")
            except ValueError: pass
        if op == 0xDF:  # setinterrupt: arg1 is offset relative to base
            try: self.enqueue(int(args[1]), f"interrupt from {ip:06x}")
            except ValueError: pass
        # terminators
        if op == 0xFF or (op == 0xC6 and ARGLIST[6]):  # finish; ret cond read as arg
            if op == 0xFF: return True
            # ret: cond in args[0]
            if args and args[0] == '0': return True
        if op == 0xE3: return True  # reti
        return False

def main():
    data = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    d = Dis(data)
    d.run(start)
    for off in sorted(d.lines):
        print(f"{off:06x}: {d.lines[off]}")
    if d.jumptables:
        print("\n; jump tables referenced:")
        for tbl, ip in sorted(d.jumptables):
            print(f";  table @{tbl:06x} (used at {ip:06x})")

if __name__ == '__main__':
    main()
