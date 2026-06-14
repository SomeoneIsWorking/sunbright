// SHADOW of reference/sms/include/JSystem/JDrama/JDRViewport.hpp — keep in sync.
//
// 64-bit-port fix (the "stray long" class, same as JDRLighting/JDRActor):
// TViewObj declares pure `virtual void perform(u32, TGraphics*) = 0`; TViewport
// declared its override as `perform(unsigned long, TGraphics*)`. On ILP32 GC
// `unsigned long` == `u32` so it overrode; on LP64 they differ (8 vs 4 bytes)
// → not an override → TViewport abstract → `new TViewport` (JDRNameRefGen.cpp)
// fails. The .cpp definition is already `TViewport::perform(u32 …)`
// (JDRViewport.cpp:13); changing the declaration to `u32` restores the override
// and matches the definition. Only that one token changed.
#ifndef JDR_VIEWPORT_HPP
#define JDR_VIEWPORT_HPP

#include <JSystem/JDrama/JDRViewObj.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>

namespace JDrama {

class TViewport : public TViewObj {
public:
	TViewport(const TRect& = TRect(0, 0, 640, 480), const char* = "<Viewport>");

	virtual ~TViewport() { }
	virtual void load(JSUMemoryInputStream&);
	virtual void perform(u32, TGraphics*); // was `unsigned long` — see shadow note (LP64 override fix)

public:
	/* 0x10 */ TRect unk10;
	/* 0x20 */ u16 unk20;
};

}; // namespace JDrama

#endif
