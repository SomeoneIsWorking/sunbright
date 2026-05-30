// Stub implementations of Dolphin host/UI/Discord functions.
// The recompiler is a headless offline tool — no renderer, no window, no GUI.
#include "Core/Host.h"
#include "UICommon/UICommon.h"
#include "UICommon/DiscordPresence.h"
#include "Common/HookableEvent.h"
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ── Host interface ────────────────────────────────────────────────────────────
std::vector<std::string> Host_GetPreferredLocales()   { return {}; }
bool Host_UIBlocksControllerState()                    { return false; }
bool Host_RendererHasFocus()                           { return false; }
bool Host_RendererHasFullFocus()                       { return false; }
bool Host_RendererIsFullscreen()                       { return false; }
bool Host_TASInputHasFocus()                           { return false; }
void Host_Message(HostMessageID)                       {}
void Host_PPCSymbolsChanged()                          {}
void Host_PPCBreakpointsChanged()                      {}
void Host_RequestRenderWindowSize(int, int)            {}
void Host_UpdateDisasmDialog()                         {}
void Host_JitCacheInvalidation()                       {}
void Host_JitProfileDataWiped()                        {}
void Host_UpdateTitle(const std::string&)              {}
void Host_YieldToUI()                                  {}
void Host_TitleChanged()                               {}
void Host_UpdateDiscordClientID(const std::string&)    {}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&,
    const std::string&, const std::string&, const std::string&, const std::string&,
    int64_t, int64_t, int, int)                        { return false; }
std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) { return nullptr; }

// ── UICommon ──────────────────────────────────────────────────────────────────
namespace UICommon {

[[nodiscard]] Common::EventHook AddFlushUnsavedDataCallback(std::function<void()>) {
    return nullptr;
}

std::string FormatSize(u64 bytes, int decimals) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double val = static_cast<double>(bytes);
    int i = 0;
    while (val >= 1024.0 && i < 4) { val /= 1024.0; i++; }
    return fmt::format("{:.{}f} {}", val, decimals, units[i]);
}

}  // namespace UICommon

// ── Discord (full stubs — USE_DISCORD_PRESENCE is OFF) ───────────────────────
namespace Discord {
struct Handler;
void Init() {}
void InitNetPlayFunctionality(Handler&) {}
void CallPendingCallbacks() {}
void UpdateClientID(const std::string&) {}
bool UpdateDiscordPresenceRaw(const std::string&, const std::string&,
    const std::string&, const std::string&, const std::string&, const std::string&,
    int64_t, int64_t, int, int) { return false; }
void UpdateDiscordPresence(int, SecretType, const std::string&, const std::string&, bool) {}
std::string CreateSecretFromIPAddress(const std::string&, int) { return {}; }
void Shutdown() {}
void SetDiscordPresenceEnabled(bool) {}
}  // namespace Discord
