// Diagnostic (SUNBRIGHT_SEQ_DIAG=1): log every JASystem::TSeqParser::mainProc (0x80321300) call —
// the BMS sequence-command interpreter. mainProc reads ONE command at ctrl->pos (TSeqCtrl+4),
// advances pos, dispatches, and returns: -1 finish, -2 keep-parsing (no-wait command), >=0 wait.
// The caller (TTrack::updateSeq) loops while it returns -2; the post-w1stLoad freeze is an
// infinite run of -2 commands (a BMS loop/jump cycle that never yields a wait). Logging each
// command byte + position + return makes the spinning cycle visible so we can own/port it.
// Super-calls the recomp body (kept diffable/A-B); delete once the parser is ported native.
#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include <cstdlib>
#include <cstdio>

static void ov_seqparser_mainproc(CPUState& cpu) {
    const bool diag = getenv("SUNBRIGHT_SEQ_DIAG");
    auto ok = [](u32 p){ return p >= 0x80000000u && p < 0x81800000u; };
    u32 track = cpu.gpr[3], ctrl = cpu.gpr[4];
    u32 pos = ok(ctrl) ? mem_r32(ctrl + 4) : 0;
    u32 cmd = ok(pos)  ? mem_r8(pos)       : 0;

    if (RecompFunc o = recomp_raw(0x80321300u)) o(cpu); else call_ppc(cpu, cpu.lr);

    if (diag) {
        static int n = 0;
        u32 newpos = ok(ctrl) ? mem_r32(ctrl + 4) : 0;
        if (n++ < 600) {
            std::fprintf(stderr, "[seq] track=%08x pos=%08x cmd=%02x -> ret=%d newpos=%08x\n",
                         track, pos, cmd, (int)cpu.gpr[3], newpos);
            std::fflush(stderr);
        }
    }
}

static const bool seqparser_diag_reg = [] {
    if (getenv("SUNBRIGHT_SEQ_DIAG")) register_override(0x80321300u, &ov_seqparser_mainproc);
    return true;
}();
