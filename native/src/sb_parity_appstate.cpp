// Implements sb_parity_dump.h's sb_get_app_state() — reads TApplication::mAppState from
// the real gpApplication object. Sits in sms-native so it has the sms include path.
//
// The DEFINITION lives here (rather than an inline in the header) on purpose: sms-render
// calls sb_get_app_state() from sb_parity_emit(), and that hard link reference forces the
// linker to pull this TU in. An inline+static-init approach was tried first — the TU got
// stripped by --gc-sections because nothing referenced any symbol in it, so the reader
// registered nullptr and every parity line emitted appState=0xFFFF (session 16 discovery).

#include <System/Application.hpp>

extern "C" unsigned sb_get_app_state() {
	return (unsigned)gpApplication.mAppState;
}
