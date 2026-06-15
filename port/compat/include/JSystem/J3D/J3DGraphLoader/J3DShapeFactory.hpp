// SHADOW of reference/sms/include/JSystem/J3D/J3DGraphLoader/J3DShapeFactory.hpp
// — keep in sync. ONLY change vs the decomp: allocVcdVatCmdBuffer's parameter
// declared `unsigned long` (== u32 on the ILP32 GameCube) is `u32` here. On LP64
// `unsigned long` (8) != `u32` (unsigned int, 4), so the decl no longer matches
// the .cpp definition `allocVcdVatCmdBuffer(u32)` ("no declaration matches") and
// callers reference a differently-mangled symbol. (stray-unsigned-long class.)
#ifndef J3D_SHAPE_FACTORY_HPP
#define J3D_SHAPE_FACTORY_HPP

#include <dolphin/types.h>
#include <dolphin/mtx.h>
#include <dolphin/gx.h>

class J3DShape;
class J3DShapeMtx;
class J3DShapeDraw;
struct ResNTAB;

struct J3DShapeInitData {
	/* 0x00 */ u8 mShapeMtxType;
	/* 0x02 */ u16 mMtxGroupNum;
	/* 0x04 */ u16 mVtxDescListIndex;
	/* 0x06 */ u16 mMtxInitDataIndex;
	/* 0x08 */ u16 mDrawInitDataIndex;
	/* 0x0C */ f32 mRadius;
	/* 0x10 */ Vec mMin;
	/* 0x1C */ Vec mMax;
};

struct J3DShapeMtxInitData {
	/* 0x00 */ u16 mUseMtxIndex;
	/* 0x02 */ u16 mUseMtxCount;
	/* 0x04 */ u32 mFirstUseMtxIndex;
};

struct J3DShapeDrawInitData {
	/* 0x00 */ u32 mDisplayListSize;
	/* 0x04 */ u32 mDisplayListIndex;
};

// FILE-OVERLAY block: offset members declared `u32` (not typed pointers) for the
// LP64 struct-overlay fix — see the J3DModelLoader.hpp shadow + the
// first_flip_endianness.md note. (Second change in this shadow beyond the
// allocVcdVatCmdBuffer mangling fix below.)
struct J3DShapeBlock {
	/* 0x00 */ u8 mMagic[4];
	/* 0x04 */ u32 mSize;

	/* 0x08 */ u16 mShapeNum;
	/* 0x0A */ u16 _pad;

	/* 0x0C */ u32 mpShapeInitData;   // was J3DShapeInitData*
	/* 0x10 */ u32 mpIndexTable;      // was u16*
	/* 0x14 */ u32 mpNameTable;       // was ResNTAB*
	/* 0x18 */ u32 mpVtxDescList;     // was GXVtxDescList*
	/* 0x1C */ u32 mpMtxTable;        // was u16*
	/* 0x20 */ u32 mpDisplayListData; // was u8*
	/* 0x24 */ u32 mpMtxInitData;     // was J3DShapeMtxInitData*
	/* 0x28 */ u32 mpDrawInitData;    // was J3DShapeDrawInitData*
}; // Size: 0x2C

enum J3DMdlDataFlag {
	J3DMdlDataFlag_ConcatView   = 0x10,
	J3DMdlDataFlag_NoUseDrawMtx = 0x20,
	J3DMdlDataFlag_NoAnimation  = 0x100,
};

class J3DShapeFactory {
public:
	J3DShapeFactory(const J3DShapeBlock&);
	J3DShape* create(int, J3DMdlDataFlag, GXVtxDescList*);
	J3DShapeMtx* newShapeMtx(int, int) const;
	J3DShapeDraw* newShapeDraw(int, int, J3DMdlDataFlag) const;
	void allocVcdVatCmdBuffer(u32); // was `unsigned long` (LP64 mismatch fix)

	u32 getMtxGroupNum(int no) const
	{
		return mpShapeInitData[mpIndexTable[no]].mMtxGroupNum;
	}
	GXVtxDescList* getVtxDescList(int no) const
	{
		return (GXVtxDescList*)((u8*)mpVtxDescList
		                        + mpShapeInitData[mpIndexTable[no]]
		                              .mVtxDescListIndex);
	}
	f32 getRadius(int no) const
	{
		return mpShapeInitData[mpIndexTable[no]].mRadius;
	}
	Vec& getMin(int no) const { return mpShapeInitData[mpIndexTable[no]].mMin; }
	Vec& getMax(int no) const { return mpShapeInitData[mpIndexTable[no]].mMax; }

public:
	/* 0x00 */ J3DShapeInitData* mpShapeInitData;
	/* 0x04 */ u16* mpIndexTable;
	/* 0x08 */ GXVtxDescList* mpVtxDescList;
	/* 0x0C */ u16* mpMtxTable;
	/* 0x10 */ u8* mpDisplayListData;
	/* 0x14 */ J3DShapeMtxInitData* mpMtxInitData;
	/* 0x18 */ J3DShapeDrawInitData* mpDrawInitData;
	/* 0x1C */ u8* mpVcdVatCmdBuffer;
};

#endif
