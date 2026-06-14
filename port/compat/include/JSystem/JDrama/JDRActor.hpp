// SHADOW of reference/sms/include/JSystem/JDrama/JDRActor.hpp — keep in sync.
//
// 64-bit-port fix (the "stray long" class): TActor::issueGXLight was declared
// `issueGXLight(unsigned long, TGraphics*)` but defined as
// `TActor::issueGXLight(u32 …)` (JDRActor.cpp:28). On ILP32 GC `unsigned long`
// == `u32` (both 4 bytes) so they matched; on LP64 `unsigned long` is 8 bytes
// != `u32` (4) → "no declaration matches" for the definition. Changing the
// declaration to `u32` makes it match its own definition. Only that one token
// changed; the rest is a verbatim copy of the decomp header.
#ifndef JDR_ACTOR_HPP
#define JDR_ACTOR_HPP

#include <JSystem/JDrama/JDRPlacement.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <JSystem/JStage/JSGActor.hpp>

namespace JDrama {

class TCharacter;

class TActor : public TPlacement, public JStage::TActor {
public:
	TActor(const char* name)
	    : TPlacement(name)
	{
		mScaling.setAll(1.0f);
		mRotation.setAll(0.0f);

		unk3C = nullptr;
		unk40 = nullptr;
	}

	~TActor();

	virtual int getType() const;
	virtual void load(JSUMemoryInputStream&);
	void issueGXLight(u32, JDrama::TGraphics*); // was `unsigned long` — see shadow note (LP64 match fix)

	virtual void perform(u32, TGraphics*);

	virtual void JSGGetTranslation(Vec*) const;
	virtual void JSGSetTranslation(const Vec&);
	virtual void JSGGetScaling(Vec*) const;
	virtual void JSGSetScaling(const Vec&);
	virtual void JSGGetRotation(Vec*) const;
	virtual void JSGSetRotation(const Vec&);

	// fabricated
	const JGeometry::TVec3<f32>& getRotation() const { return mRotation; }
	const JGeometry::TVec3<f32>& getScaling() const { return mScaling; }

public:
	/* 0x24 */ JGeometry::TVec3<f32> mScaling;
	/* 0x30 */ JGeometry::TVec3<f32> mRotation;
	/* 0x3C */ TCharacter* unk3C;
	/* 0x40 */ TViewObj* unk40;
};

} // namespace JDrama

#endif
