// probe_server.cpp — see probe_server.h for what this is and the threading contract.
//
// The socket handling is ported from the retired runtime/probe_server.cpp (git 9283f44^) including
// its hard-won details, each of which cost a session when it was missing:
//   * SO_RCVTIMEO — a client that connects and never sends (a killed curl) must not park the
//     single-threaded server in recv() forever.
//   * SO_SNDTIMEO — a client that stops reading mid-transfer must not park it in send() either:
//     once the peer's window closes a large body blocks indefinitely and wedges every later probe.
//   * MSG_NOSIGNAL — a client that timed out and closed must not SIGPIPE-kill the whole game.
//   * INADDR_LOOPBACK — this is a debug control surface with memory-write endpoints. It binds to
//     127.0.0.1 and must never be reachable from off-host.

#include "probe_server.h"

#include "intrinsics.h"

#include <lucent/log.h>

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct Endpoint {
    std::string help;
    std::function<std::string(const ProbeArgs&)> fn;
};

std::map<std::string, Endpoint>& endpoints() {
    // Function-local so registration from other translation units' static initializers cannot
    // race the container's own construction.
    static std::map<std::string, Endpoint> m;
    return m;
}

// One in-flight request, handed from the socket thread to the game thread and back.
struct Request {
    std::string path;
    ProbeArgs args;
    std::string reply;
    bool done = false;
};

std::mutex g_lock;
std::condition_variable g_cv;
std::deque<Request*> g_queue;
bool g_running = false;

std::string dispatch_on_game_thread(const std::string& path, ProbeArgs args) {
    Request req;
    req.path = path;
    req.args = std::move(args);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_queue.push_back(&req);
    }
    std::unique_lock<std::mutex> lk(g_lock);
    // Bounded: if the game thread is wedged the probe must answer rather than hang the client
    // (and, through the connection, itself).
    if (!g_cv.wait_for(lk, std::chrono::seconds(3), [&] { return req.done; })) {
        // The request is still queued and the game thread may still reach it, so it cannot be
        // abandoned by value — drop it from the queue first.
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
            if (*it == &req) { g_queue.erase(it); break; }
        return "{\"error\":\"the game thread did not reach the frame seam within 3s\"}\n";
    }
    return req.reply;
}

void serve_conn(int fd) {
    timeval rtv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
    timeval stv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);

    char req[1024] = {0};
    (void)recv(fd, req, sizeof req - 1, 0);

    char target[512] = "/help";
    (void)sscanf(req, "%*s %511s", target);

    std::string path(target), query;
    if (const auto q = path.find('?'); q != std::string::npos) {
        query = path.substr(q + 1);
        path = path.substr(0, q);
    }

    ProbeArgs args{std::move(query)};
    std::string body;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (endpoints().count(path) == 0) {
            body = "unknown path '" + path + "'; try /help\n";
        }
    }
    if (body.empty()) body = dispatch_on_game_thread(path, std::move(args));

    char hdr[256];
    const int hn = std::snprintf(hdr, sizeof hdr,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/plain\r\n"
                                 "Connection: close\r\n"
                                 "Access-Control-Allow-Origin: *\r\n"
                                 "Content-Length: %zu\r\n\r\n",
                                 body.size());
    (void)send(fd, hdr, (size_t)hn, MSG_NOSIGNAL);
    (void)send(fd, body.data(), body.size(), MSG_NOSIGNAL);
    close(fd);
}

void server_loop(int port) {
    const int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        lucent::error("probe", "socket: {}", std::strerror(errno));
        return;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never off-host: /w writes guest memory
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) < 0) {
        lucent::error("probe", "bind 127.0.0.1:{} failed: {}", port, std::strerror(errno));
        close(srv);
        return;
    }
    if (listen(srv, 8) < 0) {
        lucent::error("probe", "listen: {}", std::strerror(errno));
        close(srv);
        return;
    }
    lucent::info("probe", "http://127.0.0.1:{}/help", port);

    for (;;) {
        const int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        serve_conn(fd);
    }
    close(srv);
}

f32 guest_f32(u32 ea) {
    const u32 bits = sb_r32(ea);
    f32 f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

} // namespace

// ── ProbeArgs ────────────────────────────────────────────────────────────────────────────
bool ProbeArgs::has(const char* key) const {
    const size_t klen = std::strlen(key);
    for (size_t i = 0; i < query.size();) {
        const size_t amp = std::min(query.find('&', i), query.size());
        if (query.compare(i, klen, key) == 0 && i + klen < query.size() && query[i + klen] == '=')
            return true;
        i = amp + 1;
    }
    return false;
}

std::string ProbeArgs::str(const char* key, const char* fallback) const {
    const size_t klen = std::strlen(key);
    for (size_t i = 0; i < query.size();) {
        const size_t amp = std::min(query.find('&', i), query.size());
        if (query.compare(i, klen, key) == 0 && i + klen < amp && query[i + klen] == '=')
            return query.substr(i + klen + 1, amp - (i + klen + 1));
        i = amp + 1;
    }
    return fallback;
}

double ProbeArgs::num(const char* key, double fallback) const {
    const std::string v = str(key);
    if (v.empty()) return fallback;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return (end && *end == '\0') ? d : fallback;
}

long ProbeArgs::integer(const char* key, long fallback) const {
    const std::string v = str(key);
    if (v.empty()) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 0);   // 0 = accept 0x... and decimal
    return (end && *end == '\0') ? n : fallback;
}

// ── Registration / pump ──────────────────────────────────────────────────────────────────
void sb_probe_register(const char* path, const char* help,
                       std::function<std::string(const ProbeArgs&)> fn) {
    std::lock_guard<std::mutex> lk(g_lock);
    endpoints()[path] = Endpoint{help, std::move(fn)};
}

void sb_probe_pump() {
    if (!g_running) return;
    for (;;) {
        Request* req = nullptr;
        std::function<std::string(const ProbeArgs&)> fn;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            if (g_queue.empty()) return;
            req = g_queue.front();
            g_queue.pop_front();
            auto it = endpoints().find(req->path);
            if (it != endpoints().end()) fn = it->second.fn;
        }
        std::string reply = fn ? fn(req->args) : std::string("unknown path\n");
        {
            std::lock_guard<std::mutex> lk(g_lock);
            req->reply = std::move(reply);
            req->done = true;
        }
        g_cv.notify_all();
    }
}

void sb_probe_start() {
    static bool started = false;
    if (started) return;
    const char* e = std::getenv("SBR_PROBE");
    if (e == nullptr || e[0] == '\0' || e[0] == '0') return;
    started = true;
    g_running = true;

    sb_probe_register("/help", "list the registered endpoints", [](const ProbeArgs&) {
        std::string out;
        std::lock_guard<std::mutex> lk(g_lock);
        for (const auto& [path, ep] : endpoints()) out += path + "  — " + ep.help + "\n";
        return out;
    });

    // Guest memory read. The single most useful RE endpoint: it answers "what does the game
    // think right now" without a rebuild, and it is safe because it runs at the frame seam.
    //   /r?a=0x8040E10C&n=16[&f=1]     n = bytes (default 4), f=1 also decodes as floats
    sb_probe_register("/r", "read guest memory: a=<addr> n=<bytes> f=1 for floats", [](const ProbeArgs& a) {
        const u32 addr = (u32)a.integer("a", 0);
        const u32 n = (u32)std::min<long>(a.integer("n", 4), 256);
        if (addr == 0) return std::string("usage: /r?a=0x80000000&n=16\n");
        std::string out;
        char buf[128];
        for (u32 i = 0; i < n; i += 4) {
            if (!sb_ram_fast(addr + i)) {
                out += "  <unmapped>\n";
                break;
            }
            const u32 v = sb_r32(addr + i);
            if (a.integer("f", 0))
                std::snprintf(buf, sizeof buf, "  %08x: %08x  %g\n", addr + i, v, (double)guest_f32(addr + i));
            else
                std::snprintf(buf, sizeof buf, "  %08x: %08x\n", addr + i, v);
            out += buf;
        }
        return out;
    });

    //   /w?a=0x...&v=0x...            diagnostic poke, 32-bit
    sb_probe_register("/w", "write guest memory: a=<addr> v=<u32>", [](const ProbeArgs& a) {
        const u32 addr = (u32)a.integer("a", 0);
        if (addr == 0 || !a.has("v")) return std::string("usage: /w?a=0x80000000&v=0\n");
        if (!sb_ram_fast(addr)) return std::string("unmapped\n");
        const u32 v = (u32)a.integer("v", 0);
        const u32 old = sb_r32(addr);
        sb_w32(addr, v);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%08x: %08x -> %08x\n", addr, old, v);
        return std::string(buf);
    });

    int port = 17654;
    if (const char* p = std::getenv("SBR_PROBE_PORT")) {
        const int v = std::atoi(p);
        if (v > 0) port = v;
    }
    std::thread(server_loop, port).detach();
}
