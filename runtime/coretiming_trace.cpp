// CoreTiming schedule/remove tracer (linker --wrap, no Dolphin modification).
//
// Diagnostic for the CARDMount lost-completion deadlock (2026-06-10): a memcard EXI DMA's
// "memcardTransferComplete" CoreTiming event never fired — after 20M idle advances the PI EXI
// cause never rose, every guest thread stayed blocked, and the idle driver declared deadlock.
// This wrap ring-traces (probe /tracelog) every ScheduleEvent/RemoveEvent whose event-type name
// mentions the memcard/EXI, with the scheduling thread's CPU-token status and the from mode —
// enough to see whether the completion was scheduled at all, from where, and whether something
// removed it. Permanent env-gated diagnostic (SUNBRIGHT_DBG_CARD), per keep-diagnostics.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/System.h"

namespace CoreTiming {
struct EventType;   // layout: { callback fn ptr; const std::string* name; } (CoreTiming.h)
}

extern "C" void sb_trace(const char* tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

namespace {
bool dbg_card() {
    static const bool on = getenv("SUNBRIGHT_DBG_CARD") != nullptr;
    return on;
}
// EventType is opaque here; read its second pointer (the name) by layout.
const std::string* type_name(void* event_type) {
    if (!event_type) return nullptr;
    void* p;
    std::memcpy(&p, (char*)event_type + sizeof(void*), sizeof p);
    return (const std::string*)p;
}
bool interesting(void* event_type) {
    const std::string* n = type_name(event_type);
    return n && (n->find("memcard") != std::string::npos || n->find("EXI") != std::string::npos);
}
}  // namespace

extern "C" void __real__ZN10CoreTiming17CoreTimingManager13ScheduleEventElPNS_9EventTypeEmNS_10FromThreadE(
    void* self, int64_t cycles, void* event_type, uint64_t userdata, int from);
extern "C" void __wrap__ZN10CoreTiming17CoreTimingManager13ScheduleEventElPNS_9EventTypeEmNS_10FromThreadE(
    void* self, int64_t cycles, void* event_type, uint64_t userdata, int from) {
    // Tiny-delay reschedulers (cycles < 1000) starve the idle Advance: slice_length gets pinned
    // to ~0 and far events (the memcard completion) become unreachable. Trace them all; the ring
    // keeps the last 512 so the spammer identifies itself (map d=EventType* to a name via gdb).
    if (dbg_card() && (interesting(event_type) || cycles < 1000)) {
        // b = GetTicks() at schedule time (low 32): the event's timeout base. Compared against
        // the global timer at the deadlock this shows whether the timeout was reachable at all.
        const uint64_t now = Core::System::GetInstance().GetCoreTiming().GetTicks();
        sb_trace("ctsched", (uint32_t)cycles, (uint32_t)now,
                 ((uint32_t)from << 1) | (Core::IsCPUThread() ? 1u : 0u),
                 (uint32_t)(uintptr_t)event_type);
    }
    __real__ZN10CoreTiming17CoreTimingManager13ScheduleEventElPNS_9EventTypeEmNS_10FromThreadE(
        self, cycles, event_type, userdata, from);
}

extern "C" void __real__ZN10CoreTiming17CoreTimingManager11RemoveEventEPNS_9EventTypeE(
    void* self, void* event_type);
extern "C" void __wrap__ZN10CoreTiming17CoreTimingManager11RemoveEventEPNS_9EventTypeE(
    void* self, void* event_type) {
    if (dbg_card() && interesting(event_type))
        sb_trace("ctrm", (uint32_t)(uintptr_t)event_type, 0, Core::IsCPUThread() ? 1u : 0u, 0);
    __real__ZN10CoreTiming17CoreTimingManager11RemoveEventEPNS_9EventTypeE(self, event_type);
}
