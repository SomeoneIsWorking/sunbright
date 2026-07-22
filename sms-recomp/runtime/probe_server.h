#pragma once
// probe_server — a tiny HTTP control/inspection surface for a LIVE run.
//
// Resurrected from the retired Dolphin-era runtime/probe_server.cpp (git 9283f44^), minus the
// Dolphin-coupled endpoints. It exists because reaching a scene costs real time: booting to Delfino
// Plaza takes ~90 seconds, so A/B-ing one blend factor through a rebuild-and-relaunch cycle is
// minutes per data point. Over an afternoon of tuning that is the difference between measuring and
// guessing.
//
//   SBR_PROBE=1              enable (off by default; binds 127.0.0.1 only, never externally)
//   SBR_PROBE_PORT=<n>       port (default 17654)
//
//   curl -s 127.0.0.1:17654/help
//   curl -s '127.0.0.1:17654/r?a=0x8040E10C&n=4'
//
// THREADING CONTRACT — the part that matters. The socket loop runs on its own host thread, but the
// guest is single-threaded and its memory is only coherent at a frame boundary. So a handler NEVER
// runs on the socket thread: requests are queued and executed from sb_probe_pump() at the frame
// seam, and the socket thread blocks until the reply is filled in. Handlers may therefore read and
// write guest memory freely, and may take as long as one frame to answer.
//
// Modules register their own endpoints, so this file never grows to know about them — that is what
// turned the previous incarnation into 924 lines.

#include <functional>
#include <string>

// Query parameters of one request: probe_arg("alpha") -> "0.5", or the fallback when absent.
struct ProbeArgs {
    std::string query;   // everything after '?', e.g. "alpha=0.5&mode=3"

    // Named parameter as a string / number. Missing or unparseable -> fallback.
    std::string str(const char* key, const char* fallback = "") const;
    double num(const char* key, double fallback) const;
    long integer(const char* key, long fallback) const;   // accepts 0x... hex
    bool has(const char* key) const;
};

// Register an endpoint. `path` includes the leading slash ("/interp60"). The handler runs at the
// frame seam (see the threading contract above) and returns the response body.
void sb_probe_register(const char* path, const char* help, std::function<std::string(const ProbeArgs&)> fn);

// Start the listener if SBR_PROBE is set. Safe to call repeatedly.
void sb_probe_start();

// Execute any queued requests. Call once per frame from the frame seam, on the game thread.
void sb_probe_pump();
