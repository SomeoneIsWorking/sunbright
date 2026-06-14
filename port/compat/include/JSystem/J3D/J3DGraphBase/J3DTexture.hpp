#ifndef J3D_TEXTURE_HPP
#define J3D_TEXTURE_HPP

#include <JSystem/ResTIMG.hpp>

class J3DTexture {
public:
	/* 0x0 */ u16 mResourceCount;
	/* 0x4 */ ResTIMG* mResources;

	J3DTexture(u16 num, ResTIMG* res)
	    : mResourceCount(num)
	    , mResources(res)
	{
	}
	virtual ~J3DTexture() { }

	u16 getNum() const { return mResourceCount; }
	ResTIMG* getResTIMG(u16 entry) const { return &mResources[entry]; }
	void setResTIMG(u16 entry, const ResTIMG& timg)
	{
		mResources[entry] = timg;
		// SHADOW (64-bit port): the offsets are relative byte deltas between two
		// pointers in the same resource block; compute the difference in
		// uintptr_t so neither pointer is truncated, then store the (small)
		// result into the u32 offset field. Original used (u32) on each pointer.
		mResources[entry].imageDataOffset
		    = ((mResources[entry].imageDataOffset + (uintptr_t)&timg
		        - (uintptr_t)(mResources + entry)));
		mResources[entry].paletteOffset
		    = ((mResources[entry].paletteOffset + (uintptr_t)&timg
		        - (uintptr_t)(mResources + entry)));
	}
};

#endif
