// Sunbright fork-hook installer — replaces the linker --wrap interception mechanism.
//
// Each Dolphin function we intercept exposes a function-pointer slot in the fork
// (Source/Core/Common/SunbrightHooks.h: sb_slot_*, default null = original behavior). The
// runtime's hook bodies live in their original files (pe_token_wrap.cpp, gpfifo_wrap.cpp,
// coretiming_trace.cpp, mixer_trace.cpp, vpb_trace.cpp, overrides/aid_native.cpp,
// overrides/zelda_ucode_native.cpp) as plainly-named sb_hook_* functions. This single init call
// points the fork's slots at them.
//
// Called once at the top of main() (main_sdl.cpp). Idempotent.
//
// JitTrampoline is NOT here: its definition and call sites live entirely inside
// Source/Core/Core/PowerPC/ (JitCommon/JitBase.cpp + Jit64/JitArm64 JitAsm.cpp), which the
// project forbids modifying. It therefore keeps the linker --wrap seam (runtime/jit_hook.cpp).

#include "Common/SunbrightHooks.h"

#include <cstddef>
#include <cstdint>

// The hook bodies, defined in the runtime TUs listed above (extern "C", plainly named).
extern "C" {
void sb_hook_pe_set_token(void* self, u16 token, bool interrupt, int cycles_into_future);

void sb_hook_gpfifo_write8(void* self, u8 v);
void sb_hook_gpfifo_write16(void* self, u16 v);
void sb_hook_gpfifo_write32(void* self, u32 v);
void sb_hook_gpfifo_write64(void* self, u64 v);

void sb_hook_dsp_gen_interrupt(void* self, u64 dsp_int_type, s64 cycles_late);
void sb_hook_dsp_update_audio_dma(void* self);
void sb_hook_dsp_gen_interrupt_from_emu(void* self, int type, int cycles_into_future);

void sb_hook_ct_schedule_event(void* self, s64 cycles, void* event_type, u64 userdata, int from);
void sb_hook_ct_remove_event(void* self, void* event_type);

void sb_hook_mixer_push_samples(void* self, const s16* samples, std::size_t n);
std::size_t sb_hook_mixer_mix(void* self, s16* samples, std::size_t n);
void sb_hook_mixer_push_streaming(void* self, const s16* samples, std::size_t n);
void sb_hook_mixer_set_dma_divisor(void* self, u32 divisor);
void sb_hook_mixer_set_stream_divisor(void* self, u32 divisor);
void sb_hook_mixer_set_streaming_volume(void* self, u32 lvolume, u32 rvolume);

void* sb_hook_ucode_factory(u32 crc, void* dsphle, bool wii);
void sb_hook_zelda_fetch_vpb(void* self, u16 voice_id, void* vpb);
}  // extern "C"

void sb_install_hooks() {
  // PixelEngine
  sb_slot_pe_set_token = &sb_hook_pe_set_token;

  // GPFifo
  sb_slot_gpfifo_write8 = &sb_hook_gpfifo_write8;
  sb_slot_gpfifo_write16 = &sb_hook_gpfifo_write16;
  sb_slot_gpfifo_write32 = &sb_hook_gpfifo_write32;
  sb_slot_gpfifo_write64 = &sb_hook_gpfifo_write64;

  // DSP
  sb_slot_dsp_gen_interrupt = &sb_hook_dsp_gen_interrupt;
  sb_slot_dsp_update_audio_dma = &sb_hook_dsp_update_audio_dma;
  sb_slot_dsp_gen_interrupt_from_emu = &sb_hook_dsp_gen_interrupt_from_emu;

  // CoreTiming
  sb_slot_ct_schedule_event = &sb_hook_ct_schedule_event;
  sb_slot_ct_remove_event = &sb_hook_ct_remove_event;

  // Mixer
  sb_slot_mixer_push_samples = &sb_hook_mixer_push_samples;
  sb_slot_mixer_mix = &sb_hook_mixer_mix;
  sb_slot_mixer_push_streaming = &sb_hook_mixer_push_streaming;
  sb_slot_mixer_set_dma_divisor = &sb_hook_mixer_set_dma_divisor;
  sb_slot_mixer_set_stream_divisor = &sb_hook_mixer_set_stream_divisor;
  sb_slot_mixer_set_streaming_volume = &sb_hook_mixer_set_streaming_volume;

  // DSPHLE
  sb_slot_ucode_factory = &sb_hook_ucode_factory;
  sb_slot_zelda_fetch_vpb = &sb_hook_zelda_fetch_vpb;
}
