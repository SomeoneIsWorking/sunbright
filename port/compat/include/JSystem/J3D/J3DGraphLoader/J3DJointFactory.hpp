// SHADOW of reference/sms/include/JSystem/J3D/J3DGraphLoader/J3DJointFactory.hpp
// — keep in sync. ONLY change vs the decomp: the FILE-OVERLAY J3DJointBlock
// declares its offset members as `u32` instead of typed pointers (LP64
// struct-overlay fix — see the J3DModelLoader.hpp shadow + first_flip_endianness.md).
// The factory's *internal* pointers (mJointInitData/mIndexTable) are real runtime
// host pointers and stay typed.
#ifndef J3D_JOINT_FACTORY_HPP
#define J3D_JOINT_FACTORY_HPP

#include <JSystem/J3D/J3DGraphBase/J3DTransform.hpp>
#include <dolphin/types.h>

class J3DJoint;
struct ResNTAB;

struct J3DJointInitData {
	/* 0x00 */ u16 mKind;
	/* 0x02 */ bool mScaleCompensate;
	/* 0x04 */ J3DTransformInfo mTransformInfo;
	/* 0x24 */ f32 mRadius;
	/* 0x28 */ Vec mMin;
	/* 0x2C */ Vec mMax;
}; // Size: 0x30

struct J3DJointBlock {
	/* 0x00 */ u8 mMagic[4];
	/* 0x04 */ u32 mSize;

	/* 0x08 */ u16 mJointNum;
	/* 0x0A */ u16 _pad;

	/* 0x0C */ u32 mpJointInitData;   // was J3DJointInitData*
	/* 0x10 */ u32 mpIndexTable;      // was u16*
	/* 0x14 */ u32 mpNameTable;       // was ResNTAB*
}; // Size: 0x18

class J3DJointFactory {
public:
	J3DJointFactory(const J3DJointBlock&);
	J3DJoint* create(int);

	u16 getKind(int no) const { return mJointInitData[mIndexTable[no]].mKind; }
	u8 getScaleCompensate(int no) const
	{
		return mJointInitData[mIndexTable[no]].mScaleCompensate;
	}
	const J3DTransformInfo& getTransformInfo(int no) const
	{
		return mJointInitData[mIndexTable[no]].mTransformInfo;
	}
	f32 getRadius(int no) const
	{
		return mJointInitData[mIndexTable[no]].mRadius;
	}
	Vec& getMin(int no) const { return mJointInitData[mIndexTable[no]].mMin; }
	Vec& getMax(int no) const { return mJointInitData[mIndexTable[no]].mMax; }

public:
	/* 0x0 */ J3DJointInitData* mJointInitData;
	/* 0x4 */ u16* mIndexTable;
};

#endif
