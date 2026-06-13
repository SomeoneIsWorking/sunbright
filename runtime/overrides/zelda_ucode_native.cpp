// Native Zelda-DAC ucode — our own DSPHLE ucode state machine, tailored to this port.
//
// WHY: Dolphin's ZeldaUCode embeds HLE pacing assumptions that keep biting under our hybrid
// (instant mails, strict mail phase). The killer: a sync mail (low16==0) arriving while the
// state machine is WAITING and no render is in progress → permanent MailState::HALTED → audio
// dead for the rest of the run ("Sync mail received when rendering was not active. Halting.").
// On hardware the mail handler is an ISR on a running DSP; a stray/早 sync mail is consumed and
// ignored — nothing can latch the DSP into a dead state. Same for unknown commands.
//
// WHAT: UCodeFactory is wrapped; for SMS's DAC ucode (CRC 0x56D36052) we return this class —
// a port of the SMS path of Dolphin's ZeldaUCode (standard protocol, SYNC_PER_FRAME, NO_CMD_0D;
// the LIGHT_PROTOCOL/GBA/Wii branches are dropped — SMS never takes them), reusing Dolphin's
// pure-math ZeldaAudioRenderer (no timing in it). Differences from Dolphin, all
// resilience-tailored:
//   * out-of-phase sync mail  → dropped (counted), never HALTED
//   * crashy/unknown command  → logged + acked, never HALTED
// Everything else (command set, ack mails, per-frame sync flag protocol, render loop) is the
// faithful protocol. All other ucodes (ROM/INIT/CARD task switching) stay Dolphin's.
#include "../overrides.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"
#include "Core/HW/DSPHLE/UCodes/Zelda.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/DSP.h"
#include "Core/System.h"
#include <array>
#include <cstdio>
#include <memory>

#ifdef HAVE_DOLPHIN_CORE
// Native JAS frame driver hooks (jas_driver_native.cpp) — declared at file scope so the
// references don't acquire this file's anonymous-namespace linkage.
extern void sunbright_jas_driver_engage();
extern bool sunbright_jas_driver_engaged();

namespace {

using DSP::HLE::DSPHLE;
using DSP::HLE::UCodeInterface;
using DSP::HLE::ZeldaAudioRenderer;

constexpr u32 kSmsDacCrc = 0x56D36052;  // SMS DAC ucode: SYNC_PER_FRAME | NO_CMD_0D
constexpr u32 kSyncPerFrame = 0x00000040;

class NativeZeldaUCode final : public UCodeInterface
{
public:
  NativeZeldaUCode(DSPHLE* dsphle, u32 crc)
      : UCodeInterface(dsphle, crc), m_renderer(dsphle->GetSystem())
  {
    m_renderer.SetFlags(kSyncPerFrame);
    fprintf(stderr, "[zelda_native] native SMS DAC ucode loaded (crc=%08x)\n", crc);
  }

  void Initialize() override
  {
    m_mail_handler.PushMail(DSP_INIT, true);
    m_mail_handler.PushMail(0xF3551111);  // handshake
  }

  void Update() override
  {
    if (NeedsResumeMail())
      m_mail_handler.PushMail(DSP_RESUME, true);
  }

  void DoState(PointerWrap& p) override { DoStateShared(p); }  // no savestates on our port

  void HandleMail(u32 mail) override
  {
    // SUNBRIGHT_DBG_ZN=1: every CPU→DSP mail with the state it lands in.
    static const bool dbg = getenv("SUNBRIGHT_DBG_ZN") != nullptr;
    if (dbg)
      fprintf(stderr, "[zn] mail=%08x state=%d rend=%u/%u voice=%u/%u maxv=%u canexec=%d\n", mail,
              (int)m_state, m_curr_frame, m_requested_frames, m_curr_voice, m_voices_per_frame,
              m_sync_max_voice_id, (int)m_cmd_can_execute);
    if (m_upload_setup_in_progress)  // ucode switch (CARD task etc.) — shared SDK protocol
    {
      PrepareBootUCode(mail);
      return;
    }

    switch (m_state)
    {
    case State::WAITING:
      if (mail & 0x80000000)
      {
        if ((mail & TASK_MAIL_MASK) != TASK_MAIL_TO_DSP)
          mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);  // real ucode ignores the prefix

        switch (mail)
        {
        case MAIL_NEW_UCODE:
          m_cmd_can_execute = true;
          RunPendingCommands();
          m_upload_setup_in_progress = true;
          break;
        case MAIL_RESET:
          m_dsphle->SetUCode(UCODE_ROM);
          break;
        case MAIL_CONTINUE:
          m_cmd_can_execute = true;
          RunPendingCommands();
          break;
        default:
          // MAIL_RESUME / unknown end-rendering action: Dolphin HALTs forever here.
          // Tailored: ignore — a stray mail must not kill audio for the run.
          if (++m_ignored_mails <= 8)
            fprintf(stderr, "[zelda_native] ignored end-rendering mail %08x\n", mail);
          break;
        }
      }
      else if ((mail & 0xFFFF) == 0)
      {
        if (RenderingInProgress())
        {
          m_state = State::RENDERING;
        }
        else
        {
          // THE tailored fix: out-of-phase sync mail. Dolphin: permanent HALT (the
          // dead-jingle bug). Hardware: consumed by the ISR, no lasting effect. Drop it.
          if (++m_stray_syncs <= 8)
            fprintf(stderr, "[zelda_native] dropped out-of-phase sync mail (%u)\n",
                    m_stray_syncs);
        }
      }
      else
      {
        m_state = State::WRITING_CMD;
        m_expected_cmd_mails = mail & 0xFFFF;
      }
      break;

    case State::RENDERING:
    {
      // SYNC_PER_FRAME: two sync mails per frame carry 4x16 voice skip flags.
      const int base = m_sync_second_half ? 2 : 0;
      m_sync_skip_flags[base] = mail >> 16;
      m_sync_skip_flags[base + 1] = mail & 0xFFFF;
      if (m_sync_second_half)
        m_sync_max_voice_id = 0xFFFF;
      RenderAudio();
      if (m_sync_second_half)
        m_state = State::WAITING;
      m_sync_second_half = !m_sync_second_half;
      break;
    }

    case State::WRITING_CMD:
      Write32(mail);
      if (--m_expected_cmd_mails == 0)
      {
        m_pending_commands++;
        m_state = State::WAITING;
        RunPendingCommands();
      }
      break;
    }
  }

private:
  enum class State
  {
    WAITING,
    RENDERING,
    WRITING_CMD,
  };

  u32 Read32()
  {
    if (m_read_offset == m_write_offset)
    {
      fprintf(stderr, "[zelda_native] command param underrun\n");
      return 0;
    }
    u32 res = m_cmd_buffer[m_read_offset];
    m_read_offset = (m_read_offset + 1) % m_cmd_buffer.size();
    return res;
  }

  void Write32(u32 val)
  {
    m_cmd_buffer[m_write_offset] = val;
    m_write_offset = (m_write_offset + 1) % m_cmd_buffer.size();
  }

  void RunPendingCommands()
  {
    if (RenderingInProgress() || !m_cmd_can_execute)
      return;

    while (m_pending_commands)
    {
      const u32 cmd_mail = Read32();
      if ((cmd_mail & 0x80000000) == 0)
        continue;

      const u32 command = (cmd_mail >> 24) & 0x7F;
      const u32 sync = cmd_mail >> 16;
      const u32 extra_data = cmd_mail & 0xFFFF;
      m_pending_commands--;

      switch (command)
      {
      // Command 01: setup — VPB base + mixing/resampling/AFC coefficient tables.
      case 0x01:
      {
        m_voices_per_frame = (u16)extra_data;
        m_vpb_base = Read32();
        m_renderer.SetVPBBaseAddress(m_vpb_base);

        auto& memory = m_dsphle->GetSystem().GetMemory();
        const u32 address = Read32();

        std::array<s16, 0x100> resampling_coeffs{};
        memory.CopyFromEmuSwapped(resampling_coeffs.data(), address, sizeof(resampling_coeffs));
        m_renderer.SetResamplingCoeffs(std::move(resampling_coeffs));

        std::array<s16, 0x100> const_patterns{};
        memory.CopyFromEmuSwapped(const_patterns.data(), address + 0x200, sizeof(const_patterns));
        m_renderer.SetConstPatterns(std::move(const_patterns));

        std::array<s16, 0x80> sine_table{};
        memory.CopyFromEmuSwapped(sine_table.data(), address + 0x400, sizeof(sine_table));
        m_renderer.SetSineTable(std::move(sine_table));

        std::array<s16, 0x20> afc_coeffs{};
        memory.CopyFromEmuSwapped(afc_coeffs.data(), Read32(), sizeof(afc_coeffs));
        m_renderer.SetAfcCoeffs(std::move(afc_coeffs));

        m_renderer.SetReverbPBBaseAddress(Read32());

        SendCommandAck(Ack::STANDARD, (u16)sync);
        break;
      }

      // Command 02: render N frames. Hijacks the mail flow until rendering completes.
      case 0x02:
      {
        // Steady-state render cycle exists: hand the cadence to the native JAS frame driver
        // (jas_driver_native.cpp). From here on cmd02s arrive synchronously from the driver's
        // finishDSPFrame calls; ack mails have no consumer and are suppressed (see
        // SendCommandAck).
        sunbright_jas_driver_engage();
        m_requested_frames = (cmd_mail >> 16) & 0xFF;
        m_renderer.SetOutputVolume((u16)(cmd_mail & 0xFFFF));
        m_renderer.SetOutputLeftBufferAddr(Read32());
        m_renderer.SetOutputRightBufferAddr(Read32());
        m_curr_frame = 0;
        m_curr_voice = 0;
        // Driver mode: no sync-mail ping-pong — render every voice of every subframe now.
        // AddVoice itself skips disabled/done VPBs, which is the ground truth the voice-map
        // mails were derived from (DSP_CreateMap reads the same enabled bits).
        if (sunbright_jas_driver_engaged())
        {
          m_sync_max_voice_id = 0xFFFFFFFF;
          m_sync_skip_flags.fill(0xFFFF);
        }
        RenderAudio();
        return;
      }

      // NOPs on the SMS ucode (incl. 0C/0D which are NOP'd for this CRC).
      case 0x00:
      case 0x03:
      case 0x0A:
      case 0x0B:
      case 0x0C:
      case 0x0D:
      case 0x0F:
        SendCommandAck(Ack::STANDARD, (u16)sync);
        break;

      default:
        // 04..09 crash the real DAC ucode; anything else doesn't exist. Dolphin HALTs.
        // Tailored: log + ack so the command stream keeps flowing.
        fprintf(stderr, "[zelda_native] unknown/crashy command %02x — ignored\n", command);
        SendCommandAck(Ack::STANDARD, (u16)sync);
        break;
      }
    }
  }

  enum class Ack
  {
    STANDARD,
    DONE_RENDERING,
  };

  void SendCommandAck(Ack type, u16 sync_value)
  {
    // Driver mode: nothing consumes acks (no audioproc message pump) — pushing them would
    // only grow the pending-mail queue and raise interrupts nobody wants.
    if (sunbright_jas_driver_engaged())
      return;
    m_mail_handler.PushMail(type == Ack::STANDARD ? DSP_SYNC : DSP_FRAME_END, true);
    if (type == Ack::STANDARD)
      m_mail_handler.PushMail(0xF3550000 | sync_value);
  }

  bool RenderingInProgress() const { return m_curr_frame != m_requested_frames; }

  void RenderAudio()
  {
    if (!RenderingInProgress())
      return;

    while (m_curr_frame < m_requested_frames)
    {
      if (m_curr_voice == 0)
        m_renderer.PrepareFrame();

      while (m_curr_voice < m_voices_per_frame)
      {
        if (m_curr_voice >= m_sync_max_voice_id)
          return;  // wait for the next sync mail

        const u16 flags = m_sync_skip_flags[m_curr_voice >> 4];
        const u8 bit = 0xF - (m_curr_voice & 0xF);
        if (flags & (1 << bit))
        {
          // SUNBRIGHT_DBG_VOICE=1: guest VPB fields for every voice actually rendered —
          // resampling ratio (4.12), source type, ARAM base — read straight from guest RAM.
          static const bool dbgv = getenv("SUNBRIGHT_DBG_VOICE") != nullptr;
          if (dbgv)
          {
            u16 vw[0x90];
            m_dsphle->GetSystem().GetMemory().CopyFromEmuSwapped(
                vw, m_vpb_base + (u32)m_curr_voice * 0x180u, sizeof(vw));
            if (vw[0])
              fprintf(stderr,
                      "[voice] v%u ratio=%04x src=%u base=%04x%04x loop=%u pos=%u:%u vol2c=%d\n",
                      m_curr_voice, vw[2], vw[0x80], vw[0x8C], vw[0x8D], vw[0x81], vw[0x34],
                      vw[0x35], (s16)vw[0x2A]);
          }
          m_renderer.AddVoice((u16)m_curr_voice);
        }

        m_curr_voice++;
      }

      SendCommandAck(Ack::STANDARD, (u16)(0xFF00 | m_curr_frame));
      m_renderer.FinalizeFrame();

      m_curr_voice = 0;
      m_sync_max_voice_id = 0;
      m_curr_frame++;
    }

    SendCommandAck(Ack::DONE_RENDERING, 0);
    m_cmd_can_execute = false;  // until the next MAIL_CONTINUE/MAIL_NEW_UCODE
  }

  State m_state = State::WAITING;
  u32 m_expected_cmd_mails = 0;
  u32 m_stray_syncs = 0;
  u32 m_ignored_mails = 0;

  u32 m_sync_max_voice_id = 0;
  std::array<u16, 256> m_sync_skip_flags{};
  bool m_sync_second_half = false;

  std::array<u32, 64> m_cmd_buffer{};
  u32 m_read_offset = 0;
  u32 m_write_offset = 0;
  u32 m_pending_commands = 0;
  bool m_cmd_can_execute = true;

  u32 m_vpb_base = 0;
  u32 m_requested_frames = 0;
  u16 m_voices_per_frame = 0;
  u32 m_curr_frame = 0;
  u32 m_curr_voice = 0;

  ZeldaAudioRenderer m_renderer;
};

}  // namespace

// Fork hook (installed via sb_install_hooks → sb_hook_ucode_factory): SMS DAC ucode → our native
// state machine. Returns a UCodeInterface* to override, or nullptr to fall through to Dolphin's
// UCodeFactory (everything else, and oracle runs which must stay pure Dolphin).
extern "C" void* sb_hook_ucode_factory(u32 crc, void* dsphle, bool wii)
{
  (void)wii;
  static const bool oracle = getenv("SUNBRIGHT_DISABLE_RECOMP") != nullptr;
  if (crc == kSmsDacCrc && !oracle)
    return new NativeZeldaUCode(static_cast<DSPHLE*>(dsphle), crc);
  return nullptr;  // fall through to Dolphin's UCodeFactory
}
#endif  // HAVE_DOLPHIN_CORE
