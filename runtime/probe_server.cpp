#include "probe_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>

#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#ifdef HAVE_DOLPHIN_CORE
#  include "Core/System.h"
#  include "Core/Core.h"
#  include "Core/CoreTiming.h"
#  include "Core/HW/SystemTimers.h"
#  include "VideoCommon/PerformanceMetrics.h"
#endif

ProbeCounters g_probe;
bool g_probe_enabled = false;

#ifdef HAVE_DOLPHIN_CORE
#  include <vector>
#  include <algorithm>
#  include <fstream>
#  include <atomic>
#ifdef HAVE_DOLPHIN_CORE
#include "Core/System.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/DSP.h"
#include "VideoCommon/CommandProcessor.h"
#endif
extern u32 mem_r32(u32 ea);
extern u16 mem_r16(u32 ea);
extern void sunbright_repl_inject(const char* line);   // main_sdl.cpp — /pad scripted input
extern unsigned long g_nintr_counts[32];               // dolphin_hook.cpp — /nintr counters
extern void mem_w32(u32, u32);                         // memory_bridge — /w diagnostic poke
extern void mem_w8(u32, u8);
extern unsigned long g_ds_token_dispatches, g_ds_callbacks, g_ds_sleeps, g_ds_wakes;
unsigned long long watchdog_vi_fields();

// ── REPL-readable trace ring ──────────────────────────────────────────────────
// A thin observer (e.g. a SUNBRIGHT_OVERRIDE) calls sb_trace(tag,a,b,c,d) to record an event;
// the REPL `/tracelog` endpoint dumps it. Keeps execution-trace data in the REPL instead of
// env-gated stderr logs. Lock-free-ish: a single producer (the guest thread holding the CPU
// token) writes; the probe thread reads a snapshot. Good enough for diagnosis.
struct TraceRec { char tag[16]; uint32_t a, b, c, d; uint64_t seq; };
constexpr int SB_TRACE_N = 8192;   // ~40 B/entry; big enough to hold the seconds AROUND a fault
                                   // at audio-frame event rates (the 512 ring aged out the
                                   // dead-audio death window before a poller could react)
TraceRec           g_trace[SB_TRACE_N];
std::atomic<uint64_t> g_trace_seq{0};

extern "C" void sb_trace(const char* tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint64_t s = g_trace_seq.fetch_add(1, std::memory_order_relaxed);
    TraceRec& r = g_trace[s % SB_TRACE_N];
    int i = 0; for (; tag[i] && i < 15; i++) r.tag[i] = tag[i]; r.tag[i] = 0;
    r.a = a; r.b = b; r.c = c; r.d = d; r.seq = s;
}
#endif

namespace {

#ifdef HAVE_DOLPHIN_CORE
// ── REPL: interactive guest-state inspection (curl the endpoints; no rebuild/env-log cycle) ──
// Symbol map (reference/sms_gmse01_funcs.txt: "ADDR name" per line), loaded once for addr→name.
std::vector<std::pair<u32, std::string>>& symtab() {
    static std::vector<std::pair<u32, std::string>> t;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const char* path = getenv("SUNBRIGHT_SYMBOLS");
        std::ifstream f(path ? path : "reference/sms_gmse01_funcs.txt");
        std::string line;
        while (std::getline(f, line)) {
            char* end = nullptr;
            unsigned long a = strtoul(line.c_str(), &end, 16);
            if (end == line.c_str() || !end) continue;
            while (*end == ' ' || *end == '\t') end++;
            if (a) t.emplace_back((u32)a, std::string(end));
        }
        std::sort(t.begin(), t.end());
        fprintf(stderr, "[probe/repl] loaded %zu symbols\n", t.size());
    }
    return t;
}
// Nearest function entry at or below `a` → "name+0xNN" (or raw hex if no map).
std::string sym(u32 a) {
    auto& t = symtab();
    if (t.empty() || a < t.front().first) { char b[16]; snprintf(b, sizeof b, "%08x", a); return b; }
    auto it = std::upper_bound(t.begin(), t.end(), a,
        [](u32 v, const std::pair<u32,std::string>& p){ return v < p.first; });
    --it;
    char b[256]; snprintf(b, sizeof b, "%s+0x%x", it->second.c_str(), a - it->first);
    return b;
}
u32 qarg(const char* path, const char* key, u32 def) {
    std::string pat = std::string(key) + "=";
    const char* p = strstr(path, pat.c_str());
    if (!p) return def;
    return (u32)strtoul(p + pat.size(), nullptr, 16);
}
// Decimal variant — for durations (ms=...); hex-parsing 90000 as 0x90000 ms once wedged the
// single-threaded server for 10 minutes and looked like "HTTP is dead".
u32 qarg_dec(const char* path, const char* key, u32 def) {
    std::string pat = std::string(key) + "=";
    const char* p = strstr(path, pat.c_str());
    if (!p) return def;
    return (u32)strtoul(p + pat.size(), nullptr, 10);
}

// REPL request handler. Returns the response body for any /repl path; empty string = not a REPL path.
std::string handle_repl(const char* path) {
    // 64 KB: /tracelog dumps up to 512 ring entries (~80 bytes each) — the old 8 KB cut the
    // tail off exactly where a deadlock's last events live.
    static thread_local char buf[65536]; int n = 0;
    auto app = [&](const char* fmt, auto... a){ if (n < (int)sizeof buf) n += snprintf(buf+n, sizeof buf-n, fmt, a...); };

    if (strncmp(path, "/r?", 3) == 0 || strncmp(path, "/r ", 3) == 0) {
        u32 a = qarg(path, "a", 0), cnt = qarg(path, "n", 8);
        if (cnt > 256) cnt = 256;
        for (u32 i = 0; i < cnt; i++) {
            if ((i & 3) == 0) app("%08x:", a + i*4);
            app(" %08x", mem_r32(a + i*4));
            if ((i & 3) == 3) app("\n");
        }
        if (cnt & 3) app("\n");
        return std::string(buf, n);
    }
    if (strncmp(path, "/r16?", 5) == 0) {   // 16-bit reads (CP/PE/VI MMIO regs have no 32-bit mapping)
        u32 a = qarg(path, "a", 0), cnt = qarg(path, "n", 8);
        if (cnt > 256) cnt = 256;
        for (u32 i = 0; i < cnt; i++) {
            if ((i & 7) == 0) app("%08x:", a + i*2);
            app(" %04x", (unsigned)mem_r16(a + i*2));
            if ((i & 7) == 7) app("\n");
        }
        if (cnt & 7) app("\n");
        return std::string(buf, n);
    }
    if (strncmp(path, "/gx", 3) == 0) {     // CP/Fifo internals (dual-core pacing diagnostics)
#ifdef HAVE_DOLPHIN_CORE
        auto& sys = Core::System::GetInstance();
        auto& cp  = sys.GetCommandProcessor();
        auto& ff  = cp.GetFifo();
        app("CPBase=%08x CPEnd=%08x CPHiWM=%08x CPLoWM=%08x\n",
            ff.CPBase.load(), ff.CPEnd.load(), ff.CPHiWatermark, ff.CPLoWatermark);
        app("wp=%08x rp=%08x dist=%08x bp=%08x\n",
            ff.CPWritePointer.load(), ff.CPReadPointer.load(),
            ff.CPReadWriteDistance.load(), ff.CPBreakpoint.load());
        app("bpEnable=%d bpInt=%d bpHit=%d hiWM=%d hiWMInt=%d loWM=%d loWMInt=%d gpRead=%d\n",
            (int)ff.bFF_BPEnable.load(), (int)ff.bFF_BPInt.load(), (int)ff.bFF_Breakpoint.load(),
            (int)ff.bFF_HiWatermark.load(), (int)ff.bFF_HiWatermarkInt.load(),
            (int)ff.bFF_LoWatermark.load(), (int)ff.bFF_LoWatermarkInt.load(),
            (int)ff.bFF_GPReadEnable.load());
        app("interrupt_waiting=%d pi_cause=%08x pi_mask=%08x\n",
            (int)cp.IsInterruptWaiting(),
            sys.GetProcessorInterface().GetCause(), sys.GetProcessorInterface().GetMask());
#endif
        return std::string(buf, n);
    }
    if (strncmp(path, "/w?", 3) == 0) {
        // Diagnostic guest-memory poke: /w?a=HEX&v=HEX[&b=1 for byte] — hypothesis testing
        // (e.g. forcing a state byte to confirm a gate theory) without rebuild cycles.
        u32 a = qarg(path, "a", 0), v = qarg(path, "v", 0), byte = qarg(path, "b", 0);
        if (a >= 0x80000000u && a < 0x81800000u) {
            if (byte) mem_w8(a, (u8)v); else mem_w32(a, v);
            app("wrote %08x to %08x (%s)\n", v, a, byte ? "byte" : "word");
        } else app("refused: %08x not in RAM\n", a);
        return std::string(buf, n);
    }
    if (strncmp(path, "/aram", 5) == 0) {
        // ARAM content checker (instrument wave banks): /aram?a=<offset>&n=<bytes, hex> →
        // FNV-1a hash + nonzero count of the region. Compare oracle vs recomp uploads.
        u32 a = qarg(path, "a", 0), len = qarg(path, "n", 0x10000);
        if (len > 0x400000) len = 0x400000;
        const u8* p = Core::System::GetInstance().GetDSP().GetARAMPtr();
        if (!p) { app("no ARAM ptr\n"); return std::string(buf, n); }
        u32 h = 2166136261u; unsigned long nz = 0;
        for (u32 i = 0; i < len; i++) { const u8 b = p[a + i]; h = (h ^ b) * 16777619u; nz += b != 0; }
        app("aram a=%08x n=%x fnv=%08x nonzero=%lu\n", a, len, h, nz);
        return std::string(buf, n);
    }
    if (strncmp(path, "/vpb", 4) == 0) {
        // JAS DSP voice parameter blocks, read straight from guest RAM (CH_BUF global
        // 0x8040E5B8 → 64 × 0x180-byte DSPBuffer; layout = Dolphin Zelda VPB, BE u16s).
        // The ear-free voice probe: enabled/done flags + per-channel target/current volumes.
        const u32 base = mem_r32(0x8040E5B8u);
        app("CH_BUF=%08x\n", base);
        if (base >= 0x80000000u && base < 0x81800000u) {
            for (int v = 0; v < 64; v++) {
                const u32 b = base + (u32)v * 0x180u;
                const u16 en = mem_r16(b), done = mem_r16(b + 2);
                if (!en && !done) continue;
                app("v%02d en=%u done=%u", v, en, done);
                for (int c = 0; c < 6; c++) {
                    const u32 ch = b + 0x10u + (u32)c * 8u;   // channels[6]{id,tgt,cur,unk} u16s
                    const u16 id = mem_r16(ch);
                    if (id) app(" ch%04x=%d/%d", id, (s16)mem_r16(ch + 2), (s16)mem_r16(ch + 4));
                }
                if (mem_r16(b + 0x58u))                       // use_dolby_volume (u16 idx 0x2C)
                    app(" dolby=%d/%d", (s16)mem_r16(b + 0x54u), (s16)mem_r16(b + 0x56u));
                app(" pos=%u:%u\n", mem_r16(b + 0x68u), mem_r16(b + 0x6Au));
            }
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/nintr", 6) == 0) {     // native interrupt dispatch counters per source
        for (int i = 0; i < 32; i++)
            if (g_nintr_counts[i]) app("intr%d=%lu\n", i, g_nintr_counts[i]);
        return std::string(buf, n);
    }
    if (strncmp(path, "/drawsync", 9) == 0) {  // pollution/drawsync pipeline counters
        app("token_dispatches=%lu callbacks=%lu sleeps=%lu wakes=%lu vi_fields=%llu\n",
            g_ds_token_dispatches, g_ds_callbacks, g_ds_sleeps, g_ds_wakes, watchdog_vi_fields());
        return std::string(buf, n);
    }
    if (strncmp(path, "/fn?", 4) == 0) {
        u32 a = qarg(path, "a", 0);
        app("%08x  %s\n", a, sym(a).c_str());
        return std::string(buf, n);
    }
    if (strncmp(path, "/stack?", 7) == 0) {
        u32 fp = qarg(path, "sp", 0);
        app("guest stack from sp=%08x (back-chain + saved LR):\n", fp);
        for (int i = 0; i < 24 && fp >= 0x80000000u && fp < 0x81800000u; i++) {
            u32 lr = mem_r32(fp + 4);
            app("  [%2d] lr=%08x  %s\n", i, lr, sym(lr).c_str());
            u32 nx = mem_r32(fp);
            if (nx <= fp || nx < 0x80000000u || nx >= 0x81800000u) break;
            fp = nx;
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/cur", 4) == 0) {
        u32 cur = mem_r32(0x800000E4u);
        app("OS_CURRENT_THREAD = %08x\n", cur);
        if (cur >= 0x80000000u && cur < 0x81800000u) {
            // OSThread/OSContext: srr0 @ +0x198, lr @ +0x84, gpr1(sp) @ +0x4, state @ +0x2c8, prio @ +0x2d0
            u32 srr0 = mem_r32(cur + 0x198), lr = mem_r32(cur + 0x84), sp = mem_r32(cur + 0x4);
            app("  srr0=%08x  %s\n", srr0, sym(srr0).c_str());
            app("  lr  =%08x  %s\n", lr, sym(lr).c_str());
            app("  sp  =%08x  state=%u prio=%d\n", sp, mem_r32(cur + 0x2c8) & 0xffff, (int)mem_r32(cur + 0x2d0));
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/trace?", 7) == 0) {
        // Sample one guest word as fast as possible for `ms` (default 3000), report VALUE TRANSITIONS
        // (index, t_ms, old->new). Run in native AND pure-Dolphin (SUNBRIGHT_DISABLE_RECOMP=1), both
        // with SUNBRIGHT_PROBE=1, and diff the two transition sequences to see where they diverge.
        u32 a = qarg(path, "a", 0), ms = qarg_dec(path, "ms", 3000);
        if (ms > 15000) ms = 15000;   // single-threaded server: a long trace blocks every other probe
        auto t0 = std::chrono::steady_clock::now();
        u32 last = mem_r32(a); long samples = 0; int trans = 0;
        app("trace %08x for %u ms:\n", a, ms);
        app("  [%6ld] t=%5dms  start=%08x\n", 0L, 0, last);
        for (;;) {
            u32 v = mem_r32(a); samples++;
            if (v != last) {
                int t = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (trans < 200) app("  [%6ld] t=%5dms  %08x -> %08x\n", samples, t, last, v);
                last = v; trans++;
            }
            if ((samples & 0x3fff) == 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= (long)ms)
                break;
        }
        app("  done: %ld samples, %d transitions, final=%08x\n", samples, trans, last);
        return std::string(buf, n);
    }
    if (strncmp(path, "/pad?", 5) == 0) {
        // Inject a scripted pad action: /pad?do=<combo>&ms=<hold-ms> — same grammar as the
        // SUNBRIGHT_REPL fifo (combo = a|b|x|y|z|start|l|r|up|down|left|right joined by '+',
        // 'wait' to idle). Queued; the main loop holds the bits for ms. e.g. /pad?do=up&ms=2000
        char combo[64] = {0}; u32 ms = qarg_dec(path, "ms", 150);
        if (const char* p = strstr(path, "do=")) {
            size_t i = 0; p += 3;
            while (*p && *p != '&' && i + 1 < sizeof combo) combo[i++] = *p++;
        }
        if (!combo[0]) return std::string("usage: /pad?do=<combo>&ms=<ms>\n");
        char line[96]; snprintf(line, sizeof line, "%s %u", combo, ms);
        sunbright_repl_inject(line);
        app("queued: %s\n", line);
        return std::string(buf, n);
    }
    if (strncmp(path, "/poll?", 6) == 0) {
        // One-shot snapshot of up to 6 addresses (a,b,c,d,e,f as hex), each named — a compact
        // "compare these key cells" line for A/B between native and Dolphin runs.
        const char* keys = "abcdef";
        for (int i = 0; keys[i]; i++) {
            char k[2] = {keys[i], 0};
            u32 a = qarg(path, k, 0);
            if (!a) continue;
            u32 v = mem_r32(a);
            app("%c: [%08x]=%08x  (%s)\n", keys[i], a, v, sym(v).c_str());
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/tracelog", 9) == 0) {
        // Dump the trace ring in chronological order, each event's tag + 4 named hex args.
        // /tracelog?s=<startseq>&n=<count> (decimal) windows the dump — the full 8192-entry ring
        // exceeds the response buffer, so page through it.
        uint64_t end = g_trace_seq.load(std::memory_order_relaxed);
        uint64_t start = end > SB_TRACE_N ? end - SB_TRACE_N : 0;
        if (uint64_t s_arg = qarg_dec(path, "s", 0); s_arg > start && s_arg < end) start = s_arg;
        if (uint64_t n_arg = qarg_dec(path, "n", 700); end - start > n_arg) end = start + n_arg;
        app("tracelog: %llu events (showing %llu..%llu)\n",
            (unsigned long long)end, (unsigned long long)start, (unsigned long long)end);
        for (uint64_t s = start; s < end; s++) {
            const TraceRec& r = g_trace[s % SB_TRACE_N];
            if (r.seq != s) continue;   // overwritten mid-read
            app("  #%-6llu %-12s a=%08x b=%08x c=%08x d=%08x\n",
                (unsigned long long)r.seq, r.tag, r.a, r.b, r.c, r.d);
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/help", 5) == 0 || strcmp(path, "/") == 0) {
        return "sunbright REPL (curl http://127.0.0.1:17654<path>):\n"
               "  /metrics            perf counters (JSON)\n"
               "  /r?a=HEX&n=N        read N words at guest addr (default 8)\n"
               "  /fn?a=HEX           resolve addr -> nearest function name\n"
               "  /stack?sp=HEX       walk guest back-chain LRs from sp, named\n"
               "  /cur                current OSThread + saved srr0/lr/sp/prio\n"
               "  /trace?a=HEX&ms=N   sample a word for N ms, list value transitions (A/B Dolphin vs native)\n"
               "  /poll?a=HEX&b=..    snapshot up to 6 cells (a..f), each named\n"
               "  /tracelog           dump the trace ring (events from sb_trace observers)\n";
    }
    return std::string();
}
#endif

using clock_t_ = std::chrono::steady_clock;

clock_t_::time_point g_start;

// Snapshot of counters + wall time at the previous /metrics request, so each response can
// report per-second RATES (the diagnostic that matters) without the client doing math.
struct Snap {
    uint64_t recomp, interp, native_os, tail, steps, poll;
    double   t;   // seconds since start
};
std::mutex g_snap_mtx;
Snap g_last{};
bool g_have_last = false;

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

// Build the /metrics JSON body.
std::string build_metrics() {
    const double t = now_s();

    const uint64_t recomp    = g_probe.call_recomp.load(std::memory_order_relaxed);
    const uint64_t interp    = g_probe.call_interp.load(std::memory_order_relaxed);
    const uint64_t native_os = g_probe.call_native_os.load(std::memory_order_relaxed);
    const uint64_t tail      = g_probe.tail.load(std::memory_order_relaxed);
    const uint64_t steps     = g_probe.interp_steps.load(std::memory_order_relaxed);
    const uint64_t poll      = g_probe.poll_yield.load(std::memory_order_relaxed);
    const uint64_t interp_ns = g_probe.interp_ns.load(std::memory_order_relaxed);
    const double   interp_frac = t > 1e-6 ? (double(interp_ns) / 1e9) / t : 0.0;  // share of wall in interpreter

    // Rates since the previous probe.
    double dt = 0, r_recomp = 0, r_interp = 0, r_native = 0, r_tail = 0, r_steps = 0, r_poll = 0;
    {
        std::lock_guard<std::mutex> lk(g_snap_mtx);
        if (g_have_last) {
            dt = t - g_last.t;
            if (dt > 1e-6) {
                r_recomp = (recomp    - g_last.recomp)    / dt;
                r_interp = (interp    - g_last.interp)    / dt;
                r_native = (native_os - g_last.native_os) / dt;
                r_tail   = (tail      - g_last.tail)      / dt;
                r_steps  = (steps     - g_last.steps)     / dt;
                r_poll   = (poll      - g_last.poll)      / dt;
            }
        }
        g_last = {recomp, interp, native_os, tail, steps, poll, t};
        g_have_last = true;
    }

    double fps = 0, vps = 0, speed = 0, max_speed = 0, emu_secs = 0;
    bool core_running = false;
#ifdef HAVE_DOLPHIN_CORE
    auto& sys = Core::System::GetInstance();
    core_running = (Core::GetState(sys) == Core::State::Running);
    // Perf metrics are safe to read from any thread (atomics inside).
    auto& pm = sys.GetPerfMetrics();
    fps = pm.GetFPS();
    vps = pm.GetVPS();
    speed = pm.GetSpeed();
    max_speed = pm.GetMaxSpeed();
    if (core_running) {
        const u64 ticks = sys.GetCoreTiming().GetTicks();
        const u32 tps   = sys.GetSystemTimers().GetTicksPerSecond();
        if (tps) emu_secs = double(ticks) / double(tps);
    }
#endif

    char buf[2048];
    int n = snprintf(buf, sizeof buf,
        "{\n"
        "  \"uptime_s\": %.3f,\n"
        "  \"window_s\": %.3f,\n"
        "  \"core_running\": %s,\n"
        "  \"emu_secs\": %.3f,\n"
        "  \"dolphin\": { \"fps\": %.2f, \"vps\": %.2f, \"speed\": %.4f, \"max_speed\": %.4f },\n"
        "  \"calls_total\": { \"recomp\": %llu, \"interp\": %llu, \"native_os\": %llu, \"tail\": %llu, \"interp_steps\": %llu, \"poll_yield\": %llu },\n"
        "  \"calls_per_s\": { \"recomp\": %.0f, \"interp\": %.0f, \"native_os\": %.0f, \"tail\": %.0f, \"interp_steps\": %.0f, \"poll_yield\": %.1f },\n"
        "  \"interp_wall_frac\": %.4f\n"
        "}\n",
        t, dt, core_running ? "true" : "false", emu_secs,
        fps, vps, speed, max_speed,
        (unsigned long long)recomp, (unsigned long long)interp, (unsigned long long)native_os,
        (unsigned long long)tail, (unsigned long long)steps, (unsigned long long)poll,
        r_recomp, r_interp, r_native, r_tail, r_steps, r_poll,
        interp_frac);
    return std::string(buf, n > 0 ? (size_t)n : 0);
}

void serve_conn(int fd) {
    // The server is single-threaded: a client that connects but never sends (e.g. a curl
    // killed between connect and write) must not park the whole probe in recv() forever.
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[1024] = {0};
    (void)recv(fd, req, sizeof req - 1, 0);

    // Parse "GET <path> HTTP/1.1" → route. /metrics (default) + the REPL inspection endpoints.
    std::string body;
    char path[512] = "/metrics";
    if (sscanf(req, "%*s %511s", path) == 1) {}
#ifdef HAVE_DOLPHIN_CORE
    if (strncmp(path, "/metrics", 8) != 0) {
        body = handle_repl(path);
        if (body.empty()) body = "unknown path; try /help\n";
    } else
#endif
        body = build_metrics();
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %zu\r\n\r\n",
        body.size());
    // MSG_NOSIGNAL: a client that timed out and closed (curl -m) must not SIGPIPE-kill the
    // whole emulator — long endpoints (/trace) regularly outlive the client.
    (void)send(fd, hdr, (size_t)hn, MSG_NOSIGNAL);
    (void)send(fd, body.data(), body.size(), MSG_NOSIGNAL);
    close(fd);
}

void server_loop(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("[probe] socket"); return; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only — never expose externally
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) < 0) {
        fprintf(stderr, "[probe] bind :%d failed: %s\n", port, strerror(errno));
        close(srv);
        return;
    }
    if (listen(srv, 8) < 0) { perror("[probe] listen"); close(srv); return; }
    fprintf(stderr, "[probe] HTTP probe on http://127.0.0.1:%d/metrics\n", port);

    for (;;) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        serve_conn(fd);
    }
    close(srv);
}

}  // namespace

void probe_server_start() {
    static bool started = false;
    if (started) return;
    if (!getenv("SUNBRIGHT_PROBE")) return;
    started = true;
    g_probe_enabled = true;
    g_start = std::chrono::steady_clock::now();
    int port = 17654;
    if (const char* p = getenv("SUNBRIGHT_PROBE_PORT")) { int v = atoi(p); if (v > 0) port = v; }
    std::thread(server_loop, port).detach();
}
