// 60fps interpolation VISUAL verification tool (/verify REPL endpoint).
//
// Question it answers: is the in-between field a genuine visual MIDPOINT between the two real
// frames, or is it a duplicate of one of them (no interpolation reaching the screen)?
//
// How: the fork (Present.cpp ViSwap) reads every UNIQUE present's XFB back to CPU and calls
// sb_hook_frame_captured() tagged with the XFB address. Under interp60 the present stream is
//   real N        (addr = orig)
//   in-between N+½ (addr = orig ^ 0x400000  — the alt buffer)
//   real N+1      (addr = orig)
//   in-between N+1½ ...
// so the capture ring is an ordered N, N+½, N+1, N+1½, … sequence. We downsample each frame to a
// small luma grid and compute consecutive-frame mean-squared differences. For each in-between B
// between reals A and C:
//   loMSE = MSE(A,B)   hiMSE = MSE(B,C)   fullMSE = MSE(A,C)
//   • true midpoint  → loMSE ≈ hiMSE ≈ fullMSE/4  (a half-magnitude step has ¼ the MSE of the full)
//   • B == A (no interp) → loMSE ≈ 0, hiMSE ≈ fullMSE
//   • B == C            → loMSE ≈ fullMSE, hiMSE ≈ 0
// At alpha=0.5 the metric is symmetric, so it doesn't depend on which address we call "real".
//
// Drive it with camera rotation, e.g.:  curl '/pad?do=cright&ms=2000' &  then  curl '/verify?n=24'
// (the endpoint arms the capture, waits for it to fill, and prints the analysis in one call).

#include "../overrides.h"
#include "../intrinsics.h"
#include "Common/SunbrightHooks.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <mutex>
#include <ctime>
#include <sys/stat.h>

namespace {

constexpr int GW = 64, GH = 36;        // luma grid (downsampled frame signature)
constexpr int RING = 48;               // captured frames kept

struct Cap {
    unsigned addr = 0;
    float luma[GW * GH] = {0};
};

std::mutex g_mtx;
Cap   g_ring[RING];
int   g_count = 0;                     // total captured since arm (clamped index = g_count-1 % RING used)
unsigned g_orig_addr = 0;              // first-seen address treated as the "real" buffer base
long  g_session = 0;                   // arm timestamp; frames go to scratch/verify/s<unixtime>/ (unique
                                       // across processes, so a re-run never collides — and no rm)

float mse(const float* a, const float* b) {
    double s = 0;
    for (int i = 0; i < GW * GH; i++) { const double d = (double)a[i] - b[i]; s += d * d; }
    return (float)(s / (GW * GH));
}

}  // namespace

// Fork capture hook: downsample the RGBA8 frame to a GWxGH luma grid and push into the ring.
// rgba is row-major, `stride` bytes per row, w×h pixels. Runs on the video thread.
extern "C" void sb_hook_frame_captured(unsigned xfb_addr, const unsigned char* rgba, int w, int h,
                                       int stride) {
    if (!rgba || w <= 0 || h <= 0) return;
    Cap c;
    c.addr = xfb_addr;
    // Box-average each grid cell over its source pixels (cheap, robust to internal res).
    for (int gy = 0; gy < GH; gy++) {
        const int y0 = (int)((long long)gy * h / GH), y1 = (int)((long long)(gy + 1) * h / GH);
        for (int gx = 0; gx < GW; gx++) {
            const int x0 = (int)((long long)gx * w / GW), x1 = (int)((long long)(gx + 1) * w / GW);
            double acc = 0; long n = 0;
            for (int y = y0; y < y1; y++) {
                const unsigned char* row = rgba + (size_t)y * stride;
                for (int x = x0; x < x1; x++) {
                    const unsigned char* p = row + (size_t)x * 4;
                    acc += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];   // RGBA8 luma
                    n++;
                }
            }
            c.luma[gy * GW + gx] = n ? (float)(acc / n) : 0.0f;
        }
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_count == 0) g_orig_addr = xfb_addr & ~0x400000u;   // first frame defines the "real" base
    // Also write the FULL-resolution frame to scratch/verify/ as a PPM so the pan shots and pixel
    // diffs can be rendered (tools/interp/verify_shots.py). Tagged real/btwn by the alt-buffer bit.
    {
        const bool between = (xfb_addr & 0x400000u) != (g_orig_addr & 0x400000u);
        mkdir("scratch", 0755); mkdir("scratch/verify", 0755);
        char dir[96]; snprintf(dir, sizeof dir, "scratch/verify/s%ld", g_session); mkdir(dir, 0755);
        char path[160];
        snprintf(path, sizeof path, "%s/f%03d_%s_%08x.ppm", dir, g_count,
                 between ? "btwn" : "real", xfb_addr);
        if (FILE* fp = fopen(path, "wb")) {
            fprintf(fp, "P6\n%d %d\n255\n", w, h);
            std::vector<unsigned char> rgb((size_t)w * 3);
            for (int y = 0; y < h; y++) {
                const unsigned char* row = rgba + (size_t)y * stride;
                for (int x = 0; x < w; x++) {
                    rgb[x * 3 + 0] = row[x * 4 + 0];
                    rgb[x * 3 + 1] = row[x * 4 + 1];
                    rgb[x * 3 + 2] = row[x * 4 + 2];
                }
                fwrite(rgb.data(), 1, rgb.size(), fp);
            }
            fclose(fp);
        }
    }
    g_ring[g_count % RING] = c;
    g_count++;
}

static const bool s_installed = (sb_slot_frame_captured = &sb_hook_frame_captured, true);

// Arm a capture of n unique presents (clears the ring). The fork decrements sb_capture_frames.
extern "C" void interp_verify_arm(int n) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_count = 0; g_orig_addr = 0; g_session = (long)time(nullptr);
    // The luma ring keeps only the last RING frames for the in-process report, but EVERY armed
    // frame is dumped full-res to scratch/verify/s<NNN>/ — so a long capture (e.g. 600 = a 10s
    // walk) is analyzed offline from the PPMs (tools/interp/verify_walk.py), not the ring.
    if (n < 2) n = 2; if (n > 2000) n = 2000;
    sb_capture_frames = n;
}

// Produce the analysis report into out (cap bytes). Returns bytes written.
extern "C" int interp_verify_report(char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_mtx);
    int n = 0;
    auto app = [&](const char* fmt, ...) {
        if (n >= cap) return;
        va_list ap; va_start(ap, fmt);
        n += vsnprintf(out + n, cap - n, fmt, ap);
        va_end(ap);
        if (n > cap) n = cap;
    };
    const int have = g_count < RING ? g_count : RING;
    app("captured=%d (armed remaining=%d)  real-base=%08x  grid=%dx%d\n",
        have, sb_capture_frames, g_orig_addr, GW, GH);
    if (have < 3) { app("not enough frames yet — re-run /verify after the capture fills\n"); return n; }

    app("seq  addr      tag  MSE(prev)\n");
    int doublings = 0;   // consecutive same-tag presents = a cadence hiccup (a frame shown twice)
    for (int i = 0; i < have; i++) {
        const Cap& c = g_ring[i];
        const bool between = (c.addr & 0x400000u) != (g_orig_addr & 0x400000u);
        const bool pbtwn = i ? ((g_ring[i-1].addr & 0x400000u) != (g_orig_addr & 0x400000u)) : !between;
        const bool dbl = i && (between == pbtwn);
        if (dbl) doublings++;
        float dprev = i ? mse(g_ring[i - 1].luma, c.luma) : 0.f;
        app("%3d  %08x  %s  %s%.1f%s\n", i, c.addr, between ? "BTWN" : "real",
            i ? "" : "(--) ", dprev, dbl ? "   <-- cadence doubling" : "");
    }

    // Triplet midpoint analysis: every BTWN frame flanked by two real frames. The pixel-MSE of a
    // half-step is ~¼ of the full step for TRANSLATION but ~½ for ROTATION (rotation is non-linear
    // in pixel space), so the decisive midpoint signal is BALANCE (lo≈hi = temporally centered),
    // not absolute magnitude. DUP = one side ≈ 0 (the in-between equals a real frame = no interp).
    double sum_lo_ratio = 0, sum_hi_ratio = 0; int trips = 0; int dup = 0, mid = 0, skew = 0;
    app("\ntriplet  loMSE   hiMSE   fullMSE  lo/full hi/full  verdict\n");
    for (int i = 1; i + 1 < have; i++) {
        const Cap& A = g_ring[i - 1]; const Cap& B = g_ring[i]; const Cap& C = g_ring[i + 1];
        const bool b_between = (B.addr & 0x400000u) != (g_orig_addr & 0x400000u);
        const bool a_real    = (A.addr & 0x400000u) == (g_orig_addr & 0x400000u);
        const bool c_real    = (C.addr & 0x400000u) == (g_orig_addr & 0x400000u);
        if (!(b_between && a_real && c_real)) continue;
        const float lo = mse(A.luma, B.luma), hi = mse(B.luma, C.luma), full = mse(A.luma, C.luma);
        if (full < 1.0f) continue;   // the two reals barely differ (no motion this pair) — skip
        const float lr = lo / full, hr = hi / full;
        const float imbalance = (lr + hr) > 0 ? (lr - hr) / (lr + hr) : 0;   // -1..1, 0 = centered
        const char* verdict = (lr < 0.08f || hr < 0.08f) ? "DUP(no-interp)"
                            : (imbalance > -0.25f && imbalance < 0.25f) ? "MIDPOINT(balanced)"
                            : "skewed";
        if (!strcmp(verdict, "DUP(no-interp)")) dup++;
        else if (!strncmp(verdict, "MIDPOINT", 8)) mid++; else skew++;
        sum_lo_ratio += lr; sum_hi_ratio += hr; trips++;
        app("%4d-%d   %6.1f  %6.1f  %7.1f   %.2f    %.2f   %s\n", i - 1, i + 1, lo, hi, full, lr, hr, verdict);
    }
    if (trips) {
        app("\nMIDPOINT QUALITY: %d triplets w/ motion  MIDPOINT=%d skewed=%d DUP=%d  avg lo/full=%.2f hi/full=%.2f\n",
            trips, mid, skew, dup, sum_lo_ratio / trips, sum_hi_ratio / trips);
        app("  balanced lo≈hi & DUP=0  => the in-between is a genuine intermediate frame (interp works).\n");
        app("  DUP>0  => in-between equals a real frame (no interpolation reaching the screen).\n");
    } else {
        app("\nno triplets with motion — rotate the camera/move during capture (e.g. /pad?do=cright&ms=2000)\n");
    }
    // Cadence here is PERTURBED by this tool's per-present GPU readback (it stalls the present
    // pipeline). For the TRUE cadence read the cheap always-on counter in /interp60 ("CADENCE
    // (lifetime, no readback)"). This figure is only a rough in-capture sanity check.
    app("CADENCE (perturbed by readback — see /interp60 for true rate): %d/%d doublings (%.0f%%)\n",
        doublings, have - 1, have > 1 ? 100.0 * doublings / (have - 1) : 0.0);
    return n;
}
