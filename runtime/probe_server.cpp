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

namespace {

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
    // Drain the request headers (we only ever answer /metrics, so we don't parse the verb/path
    // beyond reading to the blank line so the client's write completes).
    char req[1024];
    (void)recv(fd, req, sizeof req, 0);

    const std::string body = build_metrics();
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
