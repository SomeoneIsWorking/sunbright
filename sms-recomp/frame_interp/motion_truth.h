// motion_truth.h — the control for the pairing audit's "no object moves that far in 1/30 s" claim.
// See motion_truth.cpp. Sampled once per simulation tick from the frame loop.

#pragma once

void sbr_motion_truth_tick();
void sbr_motion_truth_report();
