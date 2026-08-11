// shape_identity.cpp — is (J3DShape, mDrawMatrices) one object, or several wearing one name?
//
// WHY THIS EXISTS. The interpolation audit's discontinuity gate refused 12,791 paired draws in one
// 282-tick run of Pianta Village for moving >= 100 world units in a single tick. The refused deltas
// are NOT a far-off cluster: 12,657 of them land in [100,1k) — immediately adjacent to the accepted
// [10,100) bulk of 41,939 — with only 134 beyond. A gate cutting a continuous distribution is either
// severing real motion or catching a systematic mispair, and those demand opposite fixes.
//
// The refusals concentrate: 26 distinct shapes, the top one refused 3,491 times over 282 ticks —
// about twelve per tick, every tick. Sustained and structured is not what fast-moving scenery looks
// like; it is what ONE NAME SHARED BY TWELVE OBJECTS looks like, with pairing falling back to draw
// order inside the name and the order changing between ticks. The observed magnitude fits too: the
// distance between two Piantas standing in a village is tens to hundreds of units.
//
// So this measures the premise instead of arguing about it. Within one tick it asks how often two
// draws carry the SAME (shape, mDrawMatrices) tag, and whether those draws come from different
// J3DShapePackets. A packet belongs to one model instance and is allocated once, so:
//
//   same tag, DIFFERENT packet  -> the tag collapses distinct instances. The packet is a real
//                                  identity the tag is throwing away, and the fix is to use it.
//   same tag, SAME packet       -> one instance drawn more than once (multi-pass). The packet is no
//                                  help; identity has to come from the game side.
//
// HOW THE PACKET IS REACHED. J3DShapePacket::draw ends with `unk14->draw()` at 0x802ede94, with the
// packet in the callee-saved r31 (verified by disassembly: the same r31 supplies unk18/unk1c/unk20
// into the shape at +0x50/+0x54/+0x58 in the four instructions before the call). r31 is callee-saved,
// so at the moment of the call it still holds the caller's value. That makes it readable at
// J3DShape::draw's entry — but ONLY from that call site, which is why the return address is checked
// rather than assumed. Every other caller reports `no packet` and is counted separately, because
// "read a packet for every draw" and "read r31 for every draw and called it a packet" look identical
// in a total.
//
// It measures and does not act: the tag is unchanged while this runs.

#include "populations.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdint>
#include <unordered_map>

namespace {

// The one call site where r31 is a J3DShapePacket. This is the RETURN address — the instruction
// after `bl J3DShape::draw` inside J3DShapePacket::draw.
constexpr u32 kPacketDrawReturn = 0x802ede98u;

struct Seen {
    uint32_t packet = 0;
    long tick = -1;
};
std::unordered_map<uint64_t, Seen> g_seen;

long g_tick = 0;
unsigned long g_draws = 0;
unsigned long g_fromPacket = 0, g_fromElsewhere = 0;
unsigned long g_collideDifferentPacket = 0, g_collideSamePacket = 0;
unsigned long g_collideUnknownPacket = 0;

} // namespace

void sbr_shape_identity_tick() { ++g_tick; }

void sbr_shape_identity_probe(u32 shape, u32 instance, u32 lr, u32 callerR31) {
    ++g_draws;
    const uint32_t packet = (lr == kPacketDrawReturn) ? callerR31 : 0;
    if (packet != 0) ++g_fromPacket; else ++g_fromElsewhere;

    const uint64_t tag = ((uint64_t)shape << 32) | (uint64_t)instance;
    Seen& s = g_seen[tag];
    if (s.tick == g_tick) {
        // Second or later draw of this tag WITHIN one tick — exactly the situation where pairing
        // has to fall back to an ordinal.
        if (packet == 0 || s.packet == 0) {
            ++g_collideUnknownPacket;
        } else if (packet != s.packet) {
            ++g_collideDifferentPacket;
        } else {
            ++g_collideSamePacket;
        }
    }
    s.tick = g_tick;
    s.packet = packet;
}

void sbr_shape_identity_report() {
    if (g_draws == 0) {
        lucent::warn("shapeid", "shape-identity probe saw NO draw at all. That is not 'the tag is "
                                "unambiguous' — nothing was measured. Check the run rendered with "
                                "SBR_LERP60=1.");
        return;
    }
    const unsigned long collisions =
        g_collideDifferentPacket + g_collideSamePacket + g_collideUnknownPacket;
    lucent::info("shapeid",
                 "shape identity over {} tick(s): {} shape draw(s), {} of them reached from "
                 "J3DShapePacket::draw (r31 readable) and {} from somewhere else (no packet). "
                 "{} draw(s) repeated a (shape, mDrawMatrices) tag within ONE tick — of those, {} "
                 "came from a DIFFERENT packet (distinct instances the tag cannot separate; the "
                 "packet can), {} from the SAME packet (one instance drawn twice — the packet is no "
                 "help there), {} could not be attributed to a packet at all.{}",
                 g_tick, g_draws, g_fromPacket, g_fromElsewhere, collisions,
                 g_collideDifferentPacket, g_collideSamePacket, g_collideUnknownPacket,
                 collisions == 0
                     ? "   <-- NO tag was ever reused inside a tick, so the ordinal fallback never "
                       "ran and this cannot explain any refused pairing. Look elsewhere."
                     : "");
}
