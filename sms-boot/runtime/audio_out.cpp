// audio_out.cpp — per-frame host audio pump, called once per VIWaitForRetrace
// from the frame seam.
//
// Milestone 1 (docs/audio_native_mixer_plan.md) landed: the decomp's own JASystem
// KERNEL (Kernel::init/updateDac/vframeWork, DSPBuf's triple-buffer pipeline,
// TDSPChannel::updateAll) now runs synchronously on the game thread, driven from
// here — see sms-boot/runtime/jas_kernel_native.cpp for the full call-chain
// writeup. The one still-missing piece is the DSP-ucode VOICE RENDERER itself
// (DsyncFrame2, the Zelda-ucode per-voice mix — milestone 2): it's a documented
// LOUD seam that silences its output buffers, so the game is silent by an
// explicit, tracked gap, not by omission. Once milestone 2 lands, no change is
// needed here — DsyncFrame2 starts producing real samples and this pump starts
// forwarding real audio automatically (registerDacCallback wiring already carries
// whatever Kernel::vframeWork() produces to aurora::audio).

extern "C" void sb_jas_kernel_init(void);
extern "C" void sb_jas_kernel_frame(void);

extern "C" void sb_audio_frame(void) {
    sb_jas_kernel_init();
    sb_jas_kernel_frame();
}
