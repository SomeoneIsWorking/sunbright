#pragma once

// Seed the frame seam with the result of the host's first aurora_begin_frame(). The seam must
// never infer this state: replaying a FIFO stream without a matching active Aurora frame reaches
// the renderer with no frame packet.
void sbr_frame_set_initial_active(bool active) noexcept;
