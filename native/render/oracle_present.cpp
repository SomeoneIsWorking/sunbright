// oracle_present.cpp — GX_ORACLE render sink for sms-boot.
//
// When sb::engine::mode() == GX_ORACLE, sms_boot_present.cpp's VI-present hook
// forwards to sb_oracle_present_frame() here instead of running the SDL3 GPU
// pipeline. The sink is expected to consume the SAME captured GX state /
// geometry the NATIVE_PC sink does (via sb_boot_capture_tev_take,
// sb_imm_take_batch, gx_state.h globals), route it through Dolphin's
// videovulkan backend, and push the resulting frame to the window via
// sb::gxsdl::inject_cpu_frame.
//
// STATUS (Path C step 3): plumbing stub only. The sink currently paints a
// distinctive magenta+diagonal-stripe pattern into the window frame so that
// running `SB_RENDER=oracle build/native/sms-boot` visibly differs from the
// NATIVE_PC sink — proving end-to-end that the runtime toggle takes effect.
// Real Dolphin VideoCommon initialisation + GX-call routing lands in step 4.
//
// This file is ONLY compiled+linked when Dolphin's videovulkan target is
// visible in the CMake scope (see native/CMakeLists.txt: `if(TARGET
// videovulkan)`). A standalone `native/`-only build omits it, and
// sms_boot_present.cpp's dispatch falls back to NATIVE_PC.

#include "gx_sdlgpu.h"

#include "Common/Config/Config.h"
#include "Common/MsgHandler.h"     // SetEnableAlert — silence Y/N prompts
#include "Common/WindowSystemInfo.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

// The captured GX geometry contract — same format sms-boot's SDL3 renderer consumes.
#include "gx_geom.h"
// GX-seam FIFO recorder — the buffer we drain into Dolphin's OpcodeDecoder.
#include "../platform/gx_fifo.h"

#include "Common/MathUtil.h"       // Rectangle<int>

#include <cstdint>
#include <cstdio>
#include <vector>

// Bridges from the game-side GX seam — the SAME captures sms-boot's SDL3
// renderer consumes at present time. We route them into Dolphin's utility
// draw path instead.
extern "C" void sb_gx_get_clear_color(float* rgba);
// Weak: defined in native/render/oracle_direct.cpp when linked. Marks Dolphin
// ready so sb::oracle::bp_write / xf_write route into LoadBPReg/LoadXFReg.
extern "C" void sb_oracle_direct_mark_ready(int up) __attribute__((weak));
extern "C" void sb_gx_call_trace_dump(void) __attribute__((weak));
extern "C" void sb_gx_emit_full_state(void) __attribute__((weak));
// Non-extern (C++ linkage in sms_boot_j3d_capture.cpp) — must be in the global
// namespace like the definition.
int sb_boot_capture_tev_take(const sb::render::NvkTevVertex** verts,
                             const sb::render::NvkTevBatch** batches, int* nbatch);

namespace {

void tame_dolphin_config();

// Once-per-process init logging so the user sees the oracle sink is active.
bool s_announced = false;
bool s_video_up = false;
bool s_video_tried = false;

// Per-frame Dolphin resources, allocated on first successful init.
std::unique_ptr<AbstractTexture> s_color_rt;
std::unique_ptr<AbstractFramebuffer> s_fbo;
std::unique_ptr<AbstractStagingTexture> s_staging;
int s_rt_w = 0, s_rt_h = 0;


bool ensure_rt(int w, int h) {
    if (s_color_rt && s_rt_w == w && s_rt_h == h) return true;
    if (w <= 0 || h <= 0) return false;
    TextureConfig cfg(w, h, 1, 1, 1, AbstractTextureFormat::RGBA8,
                      AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);
    s_color_rt = g_gfx->CreateTexture(cfg, "oracle_sink_color");
    if (!s_color_rt) {
        std::fprintf(stderr, "[oracle] CreateTexture(color RT) failed\n");
        return false;
    }
    s_fbo = g_gfx->CreateFramebuffer(s_color_rt.get(), nullptr);
    if (!s_fbo) {
        std::fprintf(stderr, "[oracle] CreateFramebuffer failed\n");
        s_color_rt.reset();
        return false;
    }
    s_staging = g_gfx->CreateStagingTexture(StagingTextureType::Readback, cfg);
    if (!s_staging) {
        std::fprintf(stderr, "[oracle] CreateStagingTexture(Readback) failed\n");
        s_fbo.reset(); s_color_rt.reset();
        return false;
    }
    s_rt_w = w; s_rt_h = h;
    std::fprintf(stderr, "[oracle] allocated Dolphin GPU resources: %dx%d RGBA8\n", w, h);
    return true;
}

// Try to bring up Dolphin's video backend once. Returns true on success. Logs
// pass/fail loudly so we can name what's blocking the standalone init on first
// failure (see Path C step 4 notes).
bool try_init_video_backend() {
    if (s_video_tried) return s_video_up;
    s_video_tried = true;
    std::fprintf(stderr, "[oracle] attempting standalone Dolphin video backend init...\n");

    // Silence PanicAlert prompts (Init/backend may hit assertions we want to
    // log-and-continue on, not block on a Y/N stdin prompt).
    Common::SetEnableAlert(false);

    // Point Dolphin at a user directory before Init — otherwise the FS backend
    // panics on empty m_root_path. Isolated per-process to avoid clobbering the
    // oracle build/sunbright's <home>/.config/dolphin-emu. SB_ORACLE_USERDIR overrides.
    if (const char* ud = std::getenv("SB_ORACLE_USERDIR"))
        UICommon::SetUserDirectory(ud);
    else
        UICommon::SetUserDirectory(std::string("./scratch/sms_boot_oracle_userdir"));

    // UICommon::Init brings up the Config layer stack, SConfig, g_Config,
    // LogManager, and calls ActivateBackend(MAIN_GFX_BACKEND). It's the
    // sanctioned entry point (same one runtime/main_sdl.cpp uses) — safer than
    // hand-picking pieces.
    UICommon::Init();
    UICommon::CreateDirectories();

    // Force Vulkan as the graphics backend (BaseConfigLoader may default to
    // OpenGL). PopulateBackendInfo below re-reads MAIN_GFX_BACKEND, so setting
    // this after UICommon::Init is what actually picks Vulkan.
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Vulkan"));

    // Tame Dolphin's threading: sms-boot's game thread runs a cooperative
    // scheduler, and Dolphin's backend worker threads / shader-compile threads
    // race with it (present_hook stops firing after 1 frame with these on).
    tame_dolphin_config();

    // Headless WSI for the first probe — no window, no swapchain. Dolphin's
    // Vulkan backend supports this (see VKMain.cpp: `enable_surface = wsi.type
    // != Headless`). If we get this far without crashing we've proven the
    // instance / physical-device / device / vertex-manager path works
    // standalone; step 4b wires up a real WSI so present goes to the window.
    WindowSystemInfo wsi{};
    wsi.type = WindowSystemType::Headless;

    // PopulateBackendInfo fills g_backend_info (feature flags used during
    // Initialize). It also calls ActivateBackend(config value) — that's why
    // we set MAIN_GFX_BACKEND above.
    VideoBackendBase::PopulateBackendInfo(wsi);

    if (!g_video_backend) {
        std::fprintf(stderr, "[oracle] g_video_backend null after PopulateBackendInfo\n");
        return false;
    }
    std::fprintf(stderr, "[oracle] backend activated: %s\n", g_video_backend->GetDisplayName().c_str());

    std::fprintf(stderr, "[oracle] calling VideoBackend::Initialize...\n");
    std::fflush(stderr);
    const bool ok = g_video_backend->Initialize(wsi);
    std::fprintf(stderr, "[oracle] VideoBackend::Initialize returned %d\n", (int)ok);
    if (!ok) {
        std::fprintf(stderr, "[oracle] VideoBackend::Initialize returned false — check Dolphin's PanicAlertFmt output above\n");
        return false;
    }
    std::fprintf(stderr, "[oracle] Dolphin video backend UP (headless). Real rendering wires in step 4b.\n");
    s_video_up = true;
    // Notify the direct-write bridge (sb::oracle::bp_write / xf_write / ...)
    // that Dolphin is ready. Subsequent GX SDK setters skip the FIFO byte
    // reconstruction path and call Dolphin's LoadBPReg/LoadXFReg directly.
    if (&sb_oracle_direct_mark_ready) sb_oracle_direct_mark_ready(1);
    std::fflush(stderr);
    return true;
}

// Turn off Dolphin's backend multithreading (spawns a worker thread that races
// with sms-boot's cooperative scheduler). Also disable shader cache disk I/O
// and other threaded features that fight the game's execution model.
void tame_dolphin_config() {
    Config::SetBase(Config::GFX_BACKEND_MULTITHREADING, false);
    Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                    ShaderCompilationMode::Synchronous);
}

// Paint pattern helper — distinctive so a visual diff vs NATIVE_PC is obvious.
void paint_stub(std::vector<uint8_t>& rgba, int w, int h) {
    // Magenta base + white diagonal stripes every 32 px. Frame counter tints the
    // top strip so a static screen still animates (proves per-frame invocation).
    static int s_tick = 0;
    ++s_tick;
    const uint8_t tint = (uint8_t)((s_tick * 7) & 0xff);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = rgba.data() + ((size_t)y * w + x) * 4;
            const bool stripe = ((x + y) / 32) & 1;
            p[0] = stripe ? 0xff : 0xff;       // R
            p[1] = stripe ? 0xff : 0x20;       // G — magenta base, white stripe
            p[2] = stripe ? 0xff : 0xff;       // B
            p[3] = 0xff;                        // A
            if (y < 16) p[0] = tint;            // top strip: animating tick
        }
    }
}

} // namespace

// Explicit init entry point for the direct-write bridge (oracle_direct.cpp).
// Brings Dolphin's video backend UP without rendering a frame — so the
// harness can prime Dolphin, then let the scenario populate live bpmem/xfmem
// via direct writes, THEN issue the first real present. Returns true on
// successful init; idempotent across calls.
extern "C" int sb_oracle_init_only(void) {
    if (s_video_up) return 1;
    if (!s_announced) { s_announced = true; std::fprintf(stderr, "[oracle] sink active (init-only path)\n"); }
    return try_init_video_backend() ? 1 : 0;
}

extern "C" void sb_oracle_present_frame(void* /*framebuffer*/, void* /*user*/) {
    static long s_beat = 0;
    ++s_beat;
    if ((s_beat % 60) == 1) std::fprintf(stderr, "[oracle-beat] calls=%ld\n", s_beat);
    static const bool skip_init = [](){ const char* v = std::getenv("SB_ORACLE_SKIP_INIT"); return v && v[0] && v[0] != '0'; }();
    if (!s_announced) {
        s_announced = true;
        std::fprintf(stderr, "[oracle] sink active\n");
        if (!skip_init) try_init_video_backend();  // one-shot probe; sets s_video_up
    }

    // Fixed render target size. sms-boot's SDL3 backbuffer is 0x0 when SB_WINDOW=0
    // (headless — the default); ignore it and use a native 640x480 which matches
    // the GC XFB. The result gets both inject_cpu_frame'd (if a window exists) and
    // PPM-dumped so a headless run still produces verifiable output.
    int w = 640, h = 480;
    int win_w = 0, win_h = 0;
    sb::gxsdl::backbuffer_size(&win_w, &win_h);
    if (win_w > 0 && win_h > 0) { w = win_w; h = win_h; }

    // FAIL FAST (CLAUDE.md § "FAIL FAST"): if Dolphin's video backend didn't
    // come up, we USED TO paint a magenta stub pattern and continue. That's
    // exactly the class of silent failure the discipline forbids — a bogus
    // output that looks like "Tier 2 rendered something" while it's really
    // just fill. Any dependent measurement (parity harness, oracle A/B) will
    // report a wrong number without any indication it's wrong.
    // Now: abort with a clear message. SB_ORACLE_ALLOW_STUB=1 preserves the
    // old fallback for when someone genuinely wants a placeholder (rare;
    // documented for archaeology).
    if (!s_video_up || !g_gfx) {
        static const bool allow_stub = std::getenv("SB_ORACLE_ALLOW_STUB") != nullptr;
        if (allow_stub) {
            static std::vector<uint8_t> s_buf;
            const size_t need = (size_t)w * h * 4;
            if (s_buf.size() != need) s_buf.assign(need, 0);
            paint_stub(s_buf, w, h);
            if (win_w > 0) sb::gxsdl::inject_cpu_frame(s_buf.data(), w, h);
            return;
        }
        std::fprintf(stderr,
            "\n[oracle] ABORT — Dolphin video backend did NOT come up. "
            "s_video_up=%d g_gfx=%p. Look above for the last "
            "[oracle] init line — the failure mode is whatever line stopped "
            "printing. Common causes: missing Vulkan driver, headless WSI "
            "unsupported, UICommon config path invalid. Set "
            "SB_ORACLE_ALLOW_STUB=1 to fall back to the magenta stub (NOT "
            "what you want in a parity harness).\n",
            (int)s_video_up, (void*)g_gfx.get());
        std::fflush(stderr);
        std::abort();
    }

    // Diagnostic: is present_hook itself being called repeatedly? Skip all
    // Dolphin work every OTHER call to isolate whether Dolphin's GPU submit
    // stalls the game thread. SB_ORACLE_SKIP_DOLPHIN=1 skips all Dolphin work.
    static const bool skip_dolphin = [](){ const char* v = std::getenv("SB_ORACLE_SKIP_DOLPHIN"); return v && v[0] && v[0] != '0'; }();
    if (skip_dolphin) return;

    // Dolphin path: clear a Dolphin-allocated color RT, read back to CPU,
    // inject into sms-boot's window. Solid color driven by frame counter
    // proves both the GPU pipeline (clear + submit) and the readback path
    // work. FAIL FAST: RT allocation failure would previously return silently;
    // now aborts — a silent skip produces a stale/reused readback buffer.
    if (!ensure_rt(w, h)) {
        std::fprintf(stderr,
            "\n[oracle] ABORT — ensure_rt(%d, %d) FAILED. Dolphin's g_gfx "
            "couldn't allocate the color RT / framebuffer / staging texture. "
            "Check the [oracle] error line printed inside ensure_rt for the "
            "specific SDL/Vulkan failure. Cannot continue: a silent return "
            "would reuse the last frame's readback buffer as if it were "
            "fresh — the exact silent-fail case FAIL FAST catches.\n", w, h);
        std::fflush(stderr);
        std::abort();
    }

    static int s_frame = 0;
    ++s_frame;

    // FIRST GX STATE BRIDGE: read the game's current GXSetCopyClear color and
    // hand it to Dolphin's SetAndClearFramebuffer. When the game changes its
    // clear color (per-scene, per-effect), Dolphin's output visibly follows.
    // This is the smallest possible "GX drives Dolphin" wire — proof that the
    // seam bridge works end-to-end before we scale to the other 191 GX calls.
    //
    // NOTE: at the title screen the game clears to (0,0,0); the visible sky
    // colour comes from the sky-dome BATCH drawn on top of the clear. Until
    // step 4c-B bridges vertex submissions too, oracle output is a solid
    // black — that's the GAME's request, not a bug. Set SB_ORACLE_FORCE_CLEAR=
    // "r,g,b" (floats 0..1) to override for smoke-testing the pipeline.
    float game_clear[4] = {0, 0, 0, 1};
    sb_gx_get_clear_color(game_clear);
    if (const char* fc = std::getenv("SB_ORACLE_FORCE_CLEAR")) {
        float r = 0, g = 0, b = 0;
        if (std::sscanf(fc, "%f,%f,%f", &r, &g, &b) == 3) {
            game_clear[0] = r; game_clear[1] = g; game_clear[2] = b;
        }
    }
    ClearColor clear{{game_clear[0], game_clear[1], game_clear[2], game_clear[3]}};

    // Diagnostic (2026-07-04, task #29): BeginUtilityDrawing/EndUtilityDrawing
    // were kept from previous session's WIP. Utility drawing is Dolphin's path
    // for post-processing/EFB-copy blits (which use their own pipelines); real
    // GX draws through VertexManager expect the NORMAL renderer state. Wrap in
    // SB_ORACLE_USE_UTILITY=1 as a keep-old-behavior knob; default OFF.
    static const bool use_utility = std::getenv("SB_ORACLE_USE_UTILITY") != nullptr;
    if (use_utility) g_gfx->BeginUtilityDrawing();
    // Bind DOLPHIN's own EFB framebuffer (managed by g_framebuffer_manager) so
    // any DRAW opcodes the OpcodeDecoder emits below actually land there.
    // Previously we cleared our own s_fbo, but Dolphin's rendering targets its
    // internal EFB and left s_fbo untouched — so the readback showed only the
    // clear color. Now clear + read the EFB itself.
    AbstractFramebuffer* efb = g_framebuffer_manager->GetEFBFramebuffer();
    if (!efb) {
        std::fprintf(stderr,
            "\n[oracle] ABORT — g_framebuffer_manager->GetEFBFramebuffer() "
            "returned null. Dolphin's FramebufferManager is not set up. "
            "Something went wrong between VideoBackend::Initialize returning "
            "true and now; check the CreateFramebufferManager path.\n");
        std::fflush(stderr);
        std::abort();
    }
    // 2026-07-04: skip the explicit clear when running a REAL game frame —
    // the game issues its own EFB clears via BPMEM_COPY_YSCALE/CLEAR_AR
    // + a CopyDisp with clear flag, and my SetAndClearFramebuffer was
    // binding + clearing the EFB such that later draws still hit it —
    // BUT synth scenarios need this call (they have no game-side clear).
    // Gate on SB_ORACLE_NO_HOST_CLEAR so I can probe the title path.
    if (!std::getenv("SB_ORACLE_NO_HOST_CLEAR"))
    // Depth clear = 1.0 (GC "clear to far plane"). Depth-test with GX_LESS
    // requires incoming z < depth-buffer z; clearing to 0.0 made every
    // compare fail and blackholed synth-depth on Tier 2. GC's clearZ
    // register default is 0xFFFFFF (~1.0 normalised), so this is the
    // faithful default even before any GXSetCopyClear.
    g_gfx->SetAndClearFramebuffer(efb, clear, 1.0f);

    // ── Real GX pipeline through Dolphin (step 4c) ─────────────────────────
    // sms-boot's GX seam (native/platform/gx_impl.cpp + gx_imm_impl.cpp) writes
    // GC FIFO command bytes into sb::gxfifo during the frame — every BP write,
    // XF write, CP write, and DRAW opcode. We drain those bytes through
    // Dolphin's OpcodeDecoder::RunFifo<false>, which invokes the SAME callback
    // chain Dolphin's CommandProcessor uses on a real emulator run. That
    // decoder writes to BPMemory / XFMemory / VertexShaderManager / etc. and
    // triggers real draws through g_vertex_manager. Same code path as the
    // build/sunbright oracle — output matches by construction.
    static const bool skip_fifo = std::getenv("SB_ORACLE_SKIP_FIFO") != nullptr;
    if (!skip_fifo) {
        // Big-hammer: dump full captured state as FIFO writes RIGHT BEFORE the
        // decoder consumes the frame. This gives Dolphin a complete BP/XF
        // snapshot even for state fields whose setters don't yet emit their own
        // FIFO write, at the cost of re-emitting every frame.
        if (&sb_gx_emit_full_state) sb_gx_emit_full_state();
        const uint8_t* fifo_data = sb::gxfifo::data();
        const size_t   fifo_size = sb::gxfifo::size();

        // SB_ORACLE_DUMP_FIFO=1: annotate each command in the FIFO stream
        // before Dolphin decodes it. Shows exactly which BP/XF/CP register
        // writes reach Dolphin (and, by omission, which state the game
        // expects but sb_gx_emit_full_state doesn't emit). Diagnostic for
        // the Tier-2 geometry blocker (task #29).
        static const bool dump_fifo = std::getenv("SB_ORACLE_DUMP_FIFO") != nullptr;
        if (dump_fifo && fifo_size > 0) {
            std::fprintf(stderr, "[fifo] f%d %zu bytes:\n", s_frame, fifo_size);
            // GC FIFO opcodes (per Dolphin VideoCommon/OpcodeDecoding.h):
            //   0x00 = NOP
            //   0x08 = LOAD_CP_REG  (1 byte reg + 4 bytes value)
            //   0x10 = LOAD_XF_REG  (2 bytes cnt-1 + 2 bytes reg + N*4 payload)
            //   0x20..0x38 = LOAD_INDX_A/B/C/D
            //   0x40 = CALL_DL
            //   0x48 = INVL_VC
            //   0x61 = LOAD_BP_REG  (4 bytes; high byte = reg, low 24 = value)
            //   0x80..0xB8 = DRAW_*
            size_t i = 0;
            while (i < fifo_size) {
                const uint8_t op = fifo_data[i];
                if (op == 0x00) { i += 1; continue; }
                if (op == 0x08 && i + 6 <= fifo_size) {
                    // LOAD_CP_REG: reg byte + 4 bytes value.
                    const uint8_t reg = fifo_data[i+1];
                    const uint32_t val = ((uint32_t)fifo_data[i+2] << 24)
                                        | ((uint32_t)fifo_data[i+3] << 16)
                                        | ((uint32_t)fifo_data[i+4] << 8)
                                        |  (uint32_t)fifo_data[i+5];
                    std::fprintf(stderr, "  CP  reg=0x%02x val=0x%08x\n", reg, val);
                    i += 6; continue;
                }
                if (op == 0x10 && i + 5 <= fifo_size) {
                    const uint16_t cnt_m1 = ((uint16_t)fifo_data[i+1] << 8) | fifo_data[i+2];
                    const uint16_t xf_reg = ((uint16_t)fifo_data[i+3] << 8) | fifo_data[i+4];
                    const uint16_t nwords = cnt_m1 + 1;
                    std::fprintf(stderr, "  XF  reg=0x%04x nwords=%u", xf_reg, (unsigned)nwords);
                    for (int w = 0; w < nwords && i + 5 + 4*w + 4 <= fifo_size; ++w) {
                        uint32_t u = ((uint32_t)fifo_data[i+5+4*w+0] << 24)
                                    | ((uint32_t)fifo_data[i+5+4*w+1] << 16)
                                    | ((uint32_t)fifo_data[i+5+4*w+2] << 8)
                                    |  (uint32_t)fifo_data[i+5+4*w+3];
                        float f; std::memcpy(&f, &u, 4);
                        std::fprintf(stderr, " %g", (double)f);
                    }
                    std::fprintf(stderr, "\n");
                    i += 5 + 4 * nwords; continue;
                }
                if (op == 0x61 && i + 5 <= fifo_size) {
                    // LOAD_BP_REG: 4 bytes; high byte = reg, low 24 = value.
                    const uint8_t reg = fifo_data[i+1];
                    const uint32_t val = ((uint32_t)fifo_data[i+2] << 16)
                                        | ((uint32_t)fifo_data[i+3] << 8)
                                        |  (uint32_t)fifo_data[i+4];
                    std::fprintf(stderr, "  BP  reg=0x%02x val=0x%06x\n", reg, val);
                    i += 5; continue;
                }
                // DRAW opcodes 0x80..0xB8.
                if ((op & 0xC0) == 0x80 && i + 3 <= fifo_size) {
                    const uint16_t nverts = ((uint16_t)fifo_data[i+1] << 8) | fifo_data[i+2];
                    static const char* prim[] = {"QUAD","QUAD2","TRI","TRISTRIP",
                                                 "TRIFAN","LINE","LINESTRIP","POINT"};
                    const int pidx = (op >> 3) & 7;
                    std::fprintf(stderr, "  DRAW op=%02x %s n=%u (stopping dump — "
                                         "vertex payload size depends on VAT)\n",
                                 op, prim[pidx], (unsigned)nverts);
                    break;
                }
                std::fprintf(stderr, "  ??  op=%02x (unknown, stopping dump at offset %zu)\n",
                             op, i);
                break;
            }
        }

        if (fifo_size > 0) {
            u32 cycles = 0;
            OpcodeDecoder::RunFifo<false>(
                DataReader((u8*)fifo_data, (u8*)(fifo_data + fifo_size)),
                &cycles);
        }
        // Log any frame with FIFO activity — silence the many zero-byte frames
        // that happen when the game is between rendering phases.
        if (fifo_size > 0 || s_frame % 60 == 0) {
            std::fprintf(stderr, "[oracle] f%d ran %zu FIFO bytes\n", s_frame, fifo_size);
            if (&sb_gx_call_trace_dump) sb_gx_call_trace_dump();
        }
        sb::gxfifo::reset_frame();
    }
    // Legacy scene-batch drain (leave the buffers empty so they don't grow
    // unbounded even though we don't feed them into Dolphin any more).
    const sb::render::NvkTevVertex* scene = nullptr;
    const sb::render::NvkTevBatch*  batches = nullptr;
    int nbatch = 0;
    (void)sb_boot_capture_tev_take(&scene, &batches, &nbatch);

    static std::vector<uint8_t> s_buf;
    const size_t need = (size_t)w * h * 4;
    if (s_buf.size() != need) s_buf.assign(need, 0);

    // Download DOLPHIN's EFB color texture (which contains the actual DRAW
    // output) to CPU. The staging texture we allocated is sized w×h — Dolphin's
    // EFB is 640×528 at 1x scale. We copy the game's 640×480 top region.
    AbstractTexture* efb_color = g_framebuffer_manager->GetEFBColorTexture();
    if (!efb_color) {
        std::fprintf(stderr,
            "\n[oracle] ABORT — GetEFBColorTexture() returned null "
            "post-clear. Cannot read back.\n");
        std::fflush(stderr);
        std::abort();
    }
    if (s_frame == 1) {
        std::fprintf(stderr, "[oracle] EFB texture = %ux%u\n",
                     efb_color->GetWidth(), efb_color->GetHeight());
    }
    MathUtil::Rectangle<int> src_rect(0, 0, std::min(w, (int)efb_color->GetWidth()),
                                             std::min(h, (int)efb_color->GetHeight()));
    MathUtil::Rectangle<int> full(0, 0, w, h);
    s_staging->CopyFromTexture(efb_color, src_rect, 0, 0, full);
    s_staging->Flush();
    s_staging->ReadTexels(full, s_buf.data(), (u32)(w * 4));

    if (use_utility) g_gfx->EndUtilityDrawing();
    // PresentBackbuffer cycles per-frame resources (descriptor pools, command
    // buffer ring). Skipping it grows GPU memory unbounded — see Presenter.cpp
    // comment around line 1183.
    g_gfx->PresentBackbuffer();

    if (win_w > 0) sb::gxsdl::inject_cpu_frame(s_buf.data(), w, h);

    // Dump PPMs at spaced intervals to catch different game phases. FAIL FAST:
    // if fopen or fwrite fails, abort with the specific reason — a silent
    // fopen-null was hiding "the harness relocated an empty file" bugs.
    if (s_frame == 1 || s_frame == 30 || s_frame == 60 || s_frame == 120 ||
        s_frame == 200 || s_frame == 300 || s_frame == 400 || s_frame == 500) {
        std::fprintf(stderr, "[oracle] f%d clear=(%.3f,%.3f,%.3f)\n", s_frame,
                     game_clear[0], game_clear[1], game_clear[2]);
        char path[256];
        std::snprintf(path, sizeof(path), "scratch/frames/oracle_%04d.ppm", s_frame);
        FILE* f = std::fopen(path, "wb");
        if (!f) {
            std::fprintf(stderr, "\n[oracle] ABORT — fopen(%s, wb) failed. "
                                 "scratch/frames/ may not exist or be writable.\n",
                         path);
            std::fflush(stderr);
            std::abort();
        }
        if (std::fprintf(f, "P6\n%d %d\n255\n", w, h) <= 0) {
            std::fclose(f);
            std::fprintf(stderr, "\n[oracle] ABORT — PPM header write to %s "
                                 "failed.\n", path);
            std::fflush(stderr);
            std::abort();
        }
        // s_buf is RGBA — write RGB triplets.
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (int i = 0; i < w * h; ++i) {
            rgb[i * 3 + 0] = s_buf[i * 4 + 0];
            rgb[i * 3 + 1] = s_buf[i * 4 + 1];
            rgb[i * 3 + 2] = s_buf[i * 4 + 2];
        }
        const size_t got = std::fwrite(rgb.data(), 1, rgb.size(), f);
        if (got != rgb.size()) {
            std::fclose(f);
            std::fprintf(stderr, "\n[oracle] ABORT — PPM body write to %s short: "
                                 "got %zu of %zu bytes.\n", path, got, rgb.size());
            std::fflush(stderr);
            std::abort();
        }
        std::fclose(f);
        std::fprintf(stderr, "[oracle] wrote %s\n", path);
    }

    // NOTE: the NATIVE_PC path also drains sb_boot_capture_tev_take + the imm
    // capture buffers every present. Not yet drained here — step 4c wires the
    // captured GX state into Dolphin's pipeline, at which point that drain
    // becomes the input to the Dolphin renderer instead of unused work.
}
