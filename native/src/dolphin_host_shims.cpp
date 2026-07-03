// dolphin_host_shims.cpp — Host_* / Discord no-op stubs for sms-boot.
//
// Dolphin's Core / VideoCommon / InputCommon libraries call into a set of Host_*
// symbols the frontend is expected to provide. For the sunbright oracle
// (runtime/main_sdl.cpp) those are wired to a real SDL window + event loop.
//
// sms-boot uses Dolphin's video backend as a raster SINK for the GX_ORACLE
// render path (see runtime/engine.h), not as a running emulator — no PPC boot,
// no input routing, no symbol dialogs, no discord. All these callbacks are
// safely no-ops. If the GX_ORACLE sink ever needs a real host (e.g. to route
// render-target size changes to sms-boot's SDL3 window), swap the relevant
// stub for a real impl.

#include "Core/Host.h"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// Forward-declared by Core/Host.h header chain; providing implementations only.
class GBAHostInterface;
namespace HW::GBA { class Core; }

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
bool Host_UIBlocksControllerState()                 { return false; }
bool Host_RendererHasFocus()                        { return true; }
bool Host_RendererHasFullFocus()                    { return true; }
bool Host_RendererIsFullscreen()                    { return false; }
bool Host_TASInputHasFocus()                        { return false; }
void Host_Message(HostMessageID)                    {}
void Host_PPCSymbolsChanged()                       {}
void Host_PPCBreakpointsChanged()                   {}
void Host_RequestRenderWindowSize(int, int)         {}
void Host_UpdateDisasmDialog()                      {}
void Host_JitCacheInvalidation()                    {}
void Host_JitProfileDataWiped()                     {}
void Host_UpdateTitle(const std::string&)           {}
void Host_YieldToUI()                               {}
void Host_TitleChanged()                            {}
void Host_UpdateDiscordClientID(const std::string&) {}

bool Host_UpdateDiscordPresenceRaw(
    const std::string&, const std::string&, const std::string&, const std::string&,
    const std::string&, const std::string&, int64_t, int64_t, int, int) { return false; }

std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) { return nullptr; }

// Discord stubs — USE_DISCORD_PRESENCE is OFF; uicommon doesn't compile these.
namespace Discord {
enum class SecretType;  // enum defined in UICommon/DiscordPresence.h; only stubs use it
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
