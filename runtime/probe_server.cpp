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
extern u32 mem_r32(u32 ea);
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

// REPL request handler. Returns the response body for any /repl path; empty string = not a REPL path.
std::string handle_repl(const char* path) {
    char buf[8192]; int n = 0;
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
    if (strncmp(path, "/help", 5) == 0 || strcmp(path, "/") == 0) {
        return "sunbright REPL (curl http://127.0.0.1:17654<path>):\n"
               "  /metrics            perf counters (JSON)\n"
               "  /r?a=HEX&n=N        read N words at guest addr (default 8)\n"
               "  /fn?a=HEX           resolve addr -> nearest function name\n"
               "  /stack?sp=HEX       walk guest back-chain LRs from sp, named\n"
               "  /cur                current OSThread + saved srr0/lr/sp/prio\n";
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
    return std::chrono::duration<double>(clock_t_::now() - g_start).count();
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
    (void)!write(fd, hdr, (size_t)hn);
    (void)!write(fd, body.data(), body.size());
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
    g_start = clock_t_::now();
    int port = 17654;
    if (const char* p = getenv("SUNBRIGHT_PROBE_PORT")) { int v = atoi(p); if (v > 0) port = v; }
    std::thread(server_loop, port).detach();
}
