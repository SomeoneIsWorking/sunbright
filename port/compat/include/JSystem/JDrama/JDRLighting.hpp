// SHADOW of reference/sms/include/JSystem/JDrama/JDRLighting.hpp — keep in sync.
//
// 64-bit-port fix (the "stray long" class): TViewObj declares the pure virtual
// `virtual void perform(u32, TGraphics*) = 0`. TLightMap declared its override
// as `perform(unsigned long, TGraphics*)`. On the ILP32 GameCube `unsigned long`
// and `u32` were the SAME 4-byte type, so it overrode the pure virtual. On LP64
// `unsigned long` is 8 bytes != `u32` (unsigned int, 4) → the signature differs
// → the pure virtual is NOT overridden → TLightMap is abstract → `new TLightMap`
// (JDRActor.cpp) fails. The .cpp definition is already `TLightMap::perform(u32 …)`
// (JDRLighting.cpp:182), so the `unsigned long` in the declaration was the stray;
// changing it to `u32` both restores the override AND matches the definition.
// Only that one token changed; the rest is a verbatim copy of the decomp header.
#ifndef JDR_LIGHTING_HPP
#define JDR_LIGHTING_HPP

#include <JSystem/JUtility/JUTColor.hpp>
#include <JSystem/JStage/JSGLight.hpp>
#include <JSystem/JDrama/JDRPlacement.hpp>
#include <JSystem/JStage/JSGAmbientLight.hpp>
#include <dolphin/gx/GXLighting.h>

namespace JDrama {

class TLightMap : public TViewObj {
public:
	class TLightInfo {
	public:
		TLightInfo()
		    : unk0(0)
		    , unk4(nullptr)
		{
		}

	public:
		u32 unk0;
		TViewObj* unk4;
	};

	TLightMap(const char* name = "<LightMap>")
	    : TViewObj(name)
	    , mLightInfoCount(0)
	    , mLightInfos(nullptr)
	{
	}

	virtual void load(JSUMemoryInputStream&);
	virtual void perform(u32, TGraphics*); // was `unsigned long` — see shadow note (LP64 override fix)

public:
	/* 0x10 */ s32 mLightInfoCount;
	/* 0x14 */ TLightInfo* mLightInfos;
}; // size 0x18

class TLight : public TPlacement, public JStage::TLight {
public:
	TLight(const char* name = "<Light>")
	    : TPlacement(name)
	    , mLightType(JStage::TELIGHT_Unk1)
	{
		GXInitLightAttn(&unk24, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
		const JUtility::TColor& color
		    = JUtility::TColor(0xff, 0xff, 0xff, 0xff);
		GXInitLightColor(&unk24, color);
	}

	virtual void load(JSUMemoryInputStream&);
	virtual void perform(u32, TGraphics*);

	virtual JStage::TELight JSGGetLightType() const;
	virtual void JSGSetLightType(JStage::TELight);

	virtual void JSGGetPosition(Vec*) const;
	virtual void JSGSetPosition(const Vec&);

	virtual GXColor JSGGetColor() const;
	virtual void JSGSetColor(GXColor);

	void correct(TGraphics*) const;

public:
	/* 0x24 */ GXLightObj unk24;
	/* 0x64 */ JStage::TELight mLightType;
};

class TIdxLight : public TLight {
public:
	TIdxLight(const char* name = "<IdxLight>")
	    : TLight(name)
	    , unk68(0)
	{
	}

	void setLightIdx(u32 idx) { unk68 = idx; }

public:
	/* 0x68 */ u32 unk68;
};

class TLightAry : public TViewObj {
public:
	TLightAry(const char* name = "<LightAry>")
	    : TViewObj(name)
	    , mLights(nullptr)
	    , mLightCount(0)
	{
		setLightNum(0);
	}

	virtual void load(JSUMemoryInputStream&);
	virtual TNameRef* searchF(u16, const char*);
	virtual void perform(u32, TGraphics*);

	void setLightNum(long);

public:
	/* 0x10 */ TIdxLight* mLights;
	/* 0x14 */ s32 mLightCount;
};

class TAmbColor : public TViewObj, public JStage::TAmbientLight {
public:
	TAmbColor(const char* name = "<AmbColor>")
	    : TViewObj(name)
	    , mColor(0x4C, 0x4C, 0x4C, 0xFF)
	{
	}

	virtual void load(JSUMemoryInputStream&);
	virtual void perform(u32, TGraphics*);
	virtual GXColor JSGGetColor() const;
	virtual void JSGSetColor(GXColor color);

public:
	/* 0x14 */ JUtility::TColor mColor;
};

class TAmbAry : public TViewObj {
public:
	TAmbAry(const char* name = "<AmbAry>")
	    : TViewObj(name)
	    , mAmbColors(nullptr)
	    , mAmbColorCount(0)
	{
		setAmbNum(0);
	}

	virtual void load(JSUMemoryInputStream&);
	virtual TNameRef* searchF(u16, const char*);
	virtual void perform(u32, TGraphics*) { }

	void setAmbNum(long);

public:
	/* 0x10 */ TAmbColor* mAmbColors;
	/* 0x14 */ s32 mAmbColorCount;
};

} // namespace JDrama

#endif
