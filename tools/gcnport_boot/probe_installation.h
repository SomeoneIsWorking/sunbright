// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "Core/PowerPC/GcnPortRuntime.h"
#include "gcnport/dolphin_adapter.h"

namespace sunbright::gcnport_boot {

// Installs one probe per address, and refuses the run if the runtime did not take a hook.
//
// Every probe in this tool is installed the same way, and the verification is the reason: a hook
// that silently failed to install leaves its probe reporting zero entries, which reads as a finding
// about the title rather than as an instrument that was never connected. That check has to happen
// for every probe or it protects none of them, so it lives here rather than beside each one.
//
// The probes are returned by owning pointer because the runtime keeps the raw address of each as
// its callback's context: a container that rehomed its elements on the next insertion would leave
// the runtime calling into a moved-from object.
template <typename Probe, typename Make, typename Announce>
std::vector<std::unique_ptr<Probe>> install_guest_probes(gcnport::DolphinRuntimeAdapter& adapter,
                                                         PowerPC::GcnPort::RuntimeSession& runtime,
                                                         std::span<const std::uint32_t> addresses,
                                                         const char* refusal, Make make,
                                                         Announce announce) {
    std::vector<std::unique_ptr<Probe>> probes;
    probes.reserve(addresses.size());
    for (const std::uint32_t address : addresses) {
        probes.push_back(make(address));
        adapter.install_hook({.identity = adapter.identity(), .address = address},
                             std::ref(*probes.back()));
        if (!runtime.HasNativeHook(address)) {
            std::fprintf(stderr,
                         "gmse01_boot: the hook at 0x%08x did not install; refusing to %s\n",
                         address, refusal);
            std::exit(1);
        }
        announce(address);
    }
    return probes;
}

} // namespace sunbright::gcnport_boot
