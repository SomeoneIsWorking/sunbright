// Copyright 2026 Sunbright contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Standalone maintainer tool: attempts an exact GMSE01 boot through gcnport's public
// PowerPC::GcnPort adapter (BootAuthenticatedImage / ExecuteJitBlock), entirely from Sunbright.
// This intentionally never touches gcnport's own repository or tests: gcnport must never link or
// embed a copyrighted game asset, so the GMSE01 image bytes are read from disk here and passed
// across gcnport's public API boundary as an in-memory span, exactly like a title consumer would.
//
// This does not attempt disc/apploader emulation. gcnport's BootAuthenticatedImage is deliberately
// scoped to a raw in-memory image plus an explicit load address and entry point (see
// shared/gcnport/docs/dolphin-embedding-contract.md); it owns no DVD/volume/apploader pipeline. This
// tool therefore boots GMSE01's already-extracted main.dol directly: it parses the DOL header,
// assembles one contiguous flat image spanning the DOL's lowest to highest loaded address (gaps
// zero-filled, matching how the loader would place each section), and boots at the DOL's own
// entry point. This exercises real GMSE01 code without the disc/apploader/OS-init pipeline gcnport
// does not yet implement; it does not claim to reach gameplay.

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <mbedtls/sha256.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/GcnPortRuntime.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"

namespace
{
// Diagnostic-only: guest code this tool boots may fault on a host page the minimal (non-disc,
// non-apploader) boot path never mapped or initialized -- e.g. a memory access to a region
// InitFastmemArena/HW::Init would ordinarily back. A raw crash would lose the ExecutionCounters
// evidence of what DID run before that point, which is the entire finding this tool exists to
// report. This handler is deliberately narrow: it reads the one live RuntimeSession's counters and
// writes them with async-signal-safe primitives, then re-raises the default handler. It is not a
// recovery mechanism and the process still terminates on the fault.
PowerPC::GcnPort::RuntimeSession* g_diagnostic_runtime = nullptr;

void WriteDecimal(char* buffer, std::size_t buffer_size, unsigned long long value)
{
  std::snprintf(buffer, buffer_size, "%llu", value);
}

extern "C" void ReportCountersOnFault(int signal_number)
{
  if (g_diagnostic_runtime != nullptr)
  {
    const auto counters = g_diagnostic_runtime->GetExecutionCounters();
    char line[256];
    char compiled[32];
    char executions[32];
    char cold[32];
    char cache_hit[32];
    WriteDecimal(compiled, sizeof(compiled), counters.jit_blocks_compiled);
    WriteDecimal(executions, sizeof(executions), counters.jit_block_executions);
    WriteDecimal(cold, sizeof(cold), counters.cold_block_executions);
    WriteDecimal(cache_hit, sizeof(cache_hit), counters.cache_hit_block_executions);
    const int length =
        std::snprintf(line, sizeof(line),
                      "gmse01_boot: fault before shutdown -- jit_blocks_compiled=%s "
                      "jit_block_executions=%s cold_block_executions=%s cache_hit_block_executions=%s\n",
                      compiled, executions, cold, cache_hit);
    if (length > 0)
      static_cast<void>(write(STDERR_FILENO, line, static_cast<std::size_t>(length)));
  }
  std::signal(signal_number, SIG_DFL);
  std::raise(signal_number);
}

struct DolImage
{
  std::vector<u8> flat;
  u32 load_address = 0;
  u32 entry_point = 0;
};

u32 ReadBigEndianU32(const std::vector<u8>& bytes, std::size_t offset)
{
  return (static_cast<u32>(bytes[offset]) << 24) | (static_cast<u32>(bytes[offset + 1]) << 16) |
         (static_cast<u32>(bytes[offset + 2]) << 8) | static_cast<u32>(bytes[offset + 3]);
}

// Parses a raw GameCube DOL (the format Sunbright's own tooling already extracts from the retail
// disc) into one contiguous flat image plus the DOL's own load address and entry point. This is a
// narrow, self-contained parser -- not a copy of Dolphin's DOL loader -- because gcnport's public
// contract takes a flat span, and duplicating Dolphin's internal DolReader class across the library
// boundary is unnecessary for this one-shot diagnostic.
DolImage LoadDolAsFlatImage(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
  {
    std::fprintf(stderr, "gmse01_boot: cannot open DOL image '%s'\n", path.c_str());
    std::exit(1);
  }
  const std::vector<u8> header_and_body((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
  if (header_and_body.size() < 0x100)
  {
    std::fprintf(stderr, "gmse01_boot: '%s' is too small to be a DOL image\n", path.c_str());
    std::exit(1);
  }

  struct Section
  {
    u32 file_offset;
    u32 address;
    u32 size;
  };
  std::vector<Section> sections;
  for (int i = 0; i < 7; ++i)
  {
    const u32 size = ReadBigEndianU32(header_and_body, 0x90 + 4 * i);
    if (size == 0)
      continue;
    sections.push_back({ReadBigEndianU32(header_and_body, 0x00 + 4 * i),
                         ReadBigEndianU32(header_and_body, 0x48 + 4 * i), size});
  }
  for (int i = 0; i < 11; ++i)
  {
    const u32 size = ReadBigEndianU32(header_and_body, 0xAC + 4 * i);
    if (size == 0)
      continue;
    sections.push_back({ReadBigEndianU32(header_and_body, 0x1C + 4 * i),
                         ReadBigEndianU32(header_and_body, 0x64 + 4 * i), size});
  }
  if (sections.empty())
  {
    std::fprintf(stderr, "gmse01_boot: '%s' has no non-empty DOL sections\n", path.c_str());
    std::exit(1);
  }

  u32 lowest = sections.front().address;
  u32 highest = sections.front().address + sections.front().size;
  for (const Section& section : sections)
  {
    lowest = std::min(lowest, section.address);
    highest = std::max(highest, section.address + section.size);
  }

  DolImage image;
  image.load_address = lowest;
  image.entry_point = ReadBigEndianU32(header_and_body, 0xE0);
  image.flat.assign(highest - lowest, 0);
  for (const Section& section : sections)
  {
    if (static_cast<u64>(section.file_offset) + section.size > header_and_body.size())
    {
      std::fprintf(stderr, "gmse01_boot: '%s' section overruns the file\n", path.c_str());
      std::exit(1);
    }
    std::memcpy(image.flat.data() + (section.address - lowest),
                header_and_body.data() + section.file_offset, section.size);
  }
  return image;
}

void RunBoot(const DolImage& image)
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    std::fprintf(stderr, "gmse01_boot: failed to create an isolated Dolphin user directory\n");
    std::exit(1);
  }
  UICommon::SetUserDirectory(profile_path);

  PowerPC::GcnPort::ExecutionIdentity identity;
  // A real content digest of the exact booted bytes, matching the "caller-computed digest" contract
  // BootAuthenticatedImage documents (it does not itself recompute or verify the hash).
  mbedtls_sha256_ret(image.flat.data(), image.flat.size(), identity.image.sha256.data(), 0);
  identity.module_generation = 1;

  Core::System& system = Core::System::GetInstance();
  // apply_gamecube_os_init=true: reproduce the exact GameCube MSR/HID/BAT register setup every
  // retail title's real BS2/IPL performs before jumping to a disc's DOL entry point (gcnport's
  // PowerPC::GcnPort::BootAuthenticatedImage, backed by CBoot::SetupGameCubeBS2Registers, which
  // reuses CBoot::EmulatedBS2_GC's own SetupMSR/SetupHID/SetupBAT -- see
  // shared/gcnport/docs/dolphin-embedding-contract.md). Without it MSR.DR/IR stay at their
  // power-on-reset value of 0 (real mode), so GMSE01's own effective addresses (0x80xxxxxx) get
  // used as physical addresses far outside the console's 24 MiB of RAM and fault immediately.
  //
  // GMSE01's own linked __start does NOT need this tool to seed r1/r2/r13: its first instructions
  // (decomp/sms's __init_registers, see src/dolphin/os/__start.c) load r1/r2/r13 from the DOL's own
  // linked _stack_addr/_SDA2_BASE_/_SDA_BASE_ immediates via lis/ori before any memory access, so a
  // manually guessed stack pointer here would be redundant at best and wrong at worst.
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, image.flat, image.load_address, image.entry_point,
      /*apply_gamecube_os_init=*/true);
  if (!booted.ok)
  {
    std::fprintf(stderr, "gmse01_boot: BootAuthenticatedImage failed: %s\n", booted.detail.c_str());
    std::exit(1);
  }

  std::printf("gmse01_boot: booted at entry 0x%08x, load address 0x%08x, image size %zu bytes\n",
              image.entry_point, image.load_address, image.flat.size());

  {
    PowerPC::GcnPort::RuntimeSession runtime(system, identity);
    g_diagnostic_runtime = &runtime;
    std::signal(SIGSEGV, ReportCountersOnFault);

    // Bounded: this is a diagnostic boot attempt, not a gameplay loop. GMSE01's own OS-init path
    // will eventually touch an MMIO region or hardware state this minimal boot does not initialize
    // (no disc/apploader pipeline, no HW::Init); the exact block count where that happens is the
    // finding, not a target to reach. 4096 one-block dispatches is a generous bound for observing
    // early boot code translate and execute before any such fault.
    constexpr u32 MAX_BLOCKS = 4096;
    u32 blocks_run = 0;
    for (; blocks_run < MAX_BLOCKS; ++blocks_run)
    {
      const auto outcome = runtime.ExecuteJitBlock();
      if (blocks_run < 32 || blocks_run % 256 == 0)
      {
        std::printf("gmse01_boot: block %u kind=%d pc=0x%08x instructions=%u\n", blocks_run,
                    static_cast<int>(outcome.kind), outcome.guest_pc, outcome.instruction_count);
        std::fflush(stdout);
      }
      if (outcome.kind == PowerPC::GcnPort::JitBlockKind::BackendFault)
      {
        std::printf("gmse01_boot: backend fault at block %u, pc=0x%08x: %s\n", blocks_run,
                    outcome.guest_pc, outcome.detail.c_str());
        break;
      }
    }

    const auto counters = runtime.GetExecutionCounters();
    std::printf("gmse01_boot: dispatched %u one-block calls\n", blocks_run);
    std::printf("gmse01_boot: jit_blocks_compiled=%llu jit_block_executions=%llu "
                "cold_block_executions=%llu cache_hit_block_executions=%llu "
                "fallback_events=%llu\n",
                static_cast<unsigned long long>(counters.jit_blocks_compiled),
                static_cast<unsigned long long>(counters.jit_block_executions),
                static_cast<unsigned long long>(counters.cold_block_executions),
                static_cast<unsigned long long>(counters.cache_hit_block_executions),
                static_cast<unsigned long long>(counters.fallback_events));

    std::signal(SIGSEGV, SIG_DFL);
    g_diagnostic_runtime = nullptr;
  }

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}
}  // namespace

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 2)
  {
    std::fprintf(stderr, "usage: %s <path-to-extracted-main.dol>\n", argv[0]);
    return 1;
  }

  const DolImage image = LoadDolAsFlatImage(argv[1]);

  // Dolphin's BLR-return optimization installs a guard in the current CPU thread's own stack; a
  // dedicated thread gives this tool the same fully mapped stack contract Dolphin's shipping CPU
  // thread has (matching gcnport's own GcnPortRuntimeTest.cpp convention).
  std::thread cpu_thread(RunBoot, image);
  cpu_thread.join();
  return 0;
}
