// audio_out.cpp — per-frame host audio pump, called once per VIWaitForRetrace
// from the frame seam.
//
// Output path: JAS mixer -> aurora_audio_push (SDL3 audio stream, see
// extern/aurora/include/aurora/audio.h). The GC pipeline synthesized samples
// in DSP ucode (JASystem AudioThread -> DSPBuf -> AI DMA); that mixer is NOT
// ported yet — reference/sms's dspproc/dsptask TUs are PPC-DSP register code
// with no native body, and JASystem::AudioThread::audioproc returns null under
// SMS_NATIVE_PLATFORM. Until the native mixer arc lands, this pump has nothing
// to push and the game is silent BY OMISSION, not by design.
//
// Mixer arc (next audio milestone): port Kernel::updateDac + the DSP-frame
// synth (channel mixing, ADPCM decode) to native C, call it here for one
// frame's worth of 32 kHz stereo S16, aurora_audio_open on first samples,
// pace against aurora_audio_queued_frames().

extern "C" void sb_audio_frame(void) {
    // No native mixer yet — see header comment.
}
