// sb_log.h — the ONE tracked diagnostic-logging registry for the native port.
//
// Replaces ad-hoc `getenv("SB_DBG_*") + fprintf` sprawl. Every diagnostic print
// goes through a NAMED CHANNEL, enabled at runtime via a single env var:
//
//   SB_LOG=fludd,nrmmtx      enable specific channels (comma-separated)
//   SB_LOG=all               enable everything
//   SB_LOG=list              print each channel name the first time it is
//                            checked (discover what channels a run exposes)
//
// Output goes to stderr as "[<chan>] <message>\n".
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
int sb_log_enabled(const char* chan);

// vfprintf-style print to stderr, prefixed "[<chan>] " and newline-terminated.
// Does NOT check enablement — the macros below do that (cached).
#if defined(__GNUC__) || defined(__clang__)
void sb_logf(const char* chan, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void sb_logf(const char* chan, const char* fmt, ...);
#endif

#ifdef __cplusplus
}
#endif

// Per-callsite cached channel log. `chan` must be a string literal (the cache
// assumes the channel never changes at a given callsite).
#define SB_LOGC(chan, ...)                                                     \
	do {                                                                       \
		static int _sb_en = -1;                                                \
		if (_sb_en < 0)                                                        \
			_sb_en = sb_log_enabled(chan);                                     \
		if (_sb_en)                                                            \
			sb_logf(chan, __VA_ARGS__);                                        \
	} while (0)

// Cached per-callsite ENABLE TEST, for the case where the diagnostic needs to do real work before
// it can print (walk a list, resolve a name, take a backtrace) and so cannot be expressed as
// SB_LOGC arguments — those are evaluated before the macro's gate can help.
//
// Use this in place of `getenv("SB_..._DBG")` in a condition. The distinction matters: an uncached
// getenv is a linear scan of environ on every evaluation, and on a per-draw or per-joint path that
// is millions of scans per run for a diagnostic that is switched off. Measured on Delfino with an
// LD_PRELOAD getenv counter, the ad-hoc gates in this codebase cost 7.0M getenv calls in a 30 s run.
#ifdef __cplusplus
#define SB_LOG_ON(chan)                                                        \
	([]() -> int {                                                             \
		static const int _sb_on = sb_log_enabled(chan);                        \
		return _sb_on;                                                         \
	}())
#endif

// Log only the first time this callsite is reached (if the channel is enabled).
#define SB_LOG_ONCE(chan, ...)                                                 \
	do {                                                                       \
		static int _sb_once = 0;                                               \
		if (!_sb_once) {                                                       \
			_sb_once = 1;                                                      \
			SB_LOGC(chan, __VA_ARGS__);                                        \
		}                                                                      \
	} while (0)

// Log hit 1 and then every `n`th hit at this callsite.
#define SB_LOG_EVERY(chan, n, ...)                                             \
	do {                                                                       \
		static long _sb_n = 0;                                                 \
		if ((++_sb_n % (n)) == 1)                                              \
			SB_LOGC(chan, __VA_ARGS__);                                        \
	} while (0)
