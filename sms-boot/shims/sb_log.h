// C-compatible facade over Sunbright's Lucent logger. Product diagnostics use one named channel
// and one call per line. SB_LOG selects debug channels; SB_LUCENT_LOG_FILE selects the sink.
//
// Usage (game TUs — shims/ is on the include path — and sms-boot/runtime):
//   #include <sb_log.h>
//   SB_LOGC("fludd", "load flag=0x%08x", v);        // every hit (if enabled)
//   SB_LOG_ONCE("jmath", "cos(0)=%f", c);           // first hit only
//   SB_LOG_EVERY("nrmmtx", 1000, "bail #%ld", n);   // hits 1, 1001, 2001, ...
//
// The macros cache the enable check per callsite (one strcmp walk on first
// hit, a static int read afterwards) so disabled channels cost ~nothing.
// Keep channel names short, lowercase, stable — they are the user interface.
// PRUNE channels when the diagnostic dies (no tombstones).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 1 if `chan` is enabled by SB_LOG (exact match or "all"), else 0.
// Also registers the channel name for SB_LOG=list discovery.
void sb_log_configure(const char* channels);
int sb_log_enabled(const char* chan);

// Formatted Lucent output. Debug calls are channel-gated by the macros below; info, warning, and
// error output is always emitted through the configured sink.
#if defined(__GNUC__) || defined(__clang__)
void sb_logf(const char* chan, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_infof(const char* chan, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_warnf(const char* chan, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_errorf(const char* chan, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void sb_logf(const char* chan, const char* fmt, ...);
void sb_infof(const char* chan, const char* fmt, ...);
void sb_warnf(const char* chan, const char* fmt, ...);
void sb_errorf(const char* chan, const char* fmt, ...);
#endif

#ifdef __cplusplus
}
#endif

// Per-callsite cached channel log. `chan` must be a string literal (the cache
// assumes the channel never changes at a given callsite).
#define SB_LOGC(chan, ...)                                                                         \
    do {                                                                                           \
        static int _sb_en = -1;                                                                    \
        if (_sb_en < 0)                                                                            \
            _sb_en = sb_log_enabled(chan);                                                         \
        if (_sb_en)                                                                                \
            sb_logf(chan, __VA_ARGS__);                                                            \
    } while (0)

// Cached per-callsite enable test for diagnostics that must do work before formatting.
#ifdef __cplusplus
#define SB_LOG_ON(chan)                                                                            \
    ([]() -> int {                                                                                 \
        static const int _sb_on = sb_log_enabled(chan);                                            \
        return _sb_on;                                                                             \
    }())
#endif

// Log only the first time this callsite is reached (if the channel is enabled).
#define SB_LOG_ONCE(chan, ...)                                                                     \
    do {                                                                                           \
        static int _sb_once = 0;                                                                   \
        if (!_sb_once) {                                                                           \
            _sb_once = 1;                                                                          \
            SB_LOGC(chan, __VA_ARGS__);                                                            \
        }                                                                                          \
    } while (0)

// Log hit 1 and then every `n`th hit at this callsite.
#define SB_LOG_EVERY(chan, n, ...)                                                                 \
    do {                                                                                           \
        static long _sb_n = 0;                                                                     \
        if ((++_sb_n % (n)) == 1)                                                                  \
            SB_LOGC(chan, __VA_ARGS__);                                                            \
    } while (0)
