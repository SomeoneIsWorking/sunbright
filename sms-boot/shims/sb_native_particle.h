#pragma once

class JPADrawContext;
class JPABaseParticle;

// Returns true when the particle inputs were accepted by the PC-native semantic sink. The caller
// still runs its original GX body today so Aurora remains an independent content oracle.
extern "C" bool sb_native_particle_submit_billboard(const JPADrawContext* context,
                                                    JPABaseParticle* particle);
