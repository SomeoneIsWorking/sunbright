// OWNED COPY of reference/sms/src/JSystem/J3D/J3DGraphLoader/J3DJointFactory.cpp
// — keep in sync. 64-bit-port fix (pointer-truncation class): J3DJointBlock's
// "mpXxx" fields are pointer-typed but on disk hold a 32-bit FILE OFFSET; the
// decomp casts `(u32)block.mpXxx` to feed JSUConvertOffsetToPtr(base, u32). On
// LP64 `(u32)ptr` is an ill-formed narrowing cast; widening to `(u32)(uintptr_t)`
// is value-preserving (the stored offset is <4GB). Only those 2 casts changed.
#include <JSystem/J3D/J3DGraphLoader/J3DJointFactory.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/JSupport.hpp>

J3DJointFactory::J3DJointFactory(const J3DJointBlock& jointBlock)
{
	mJointInitData = JSUConvertOffsetToPtr<J3DJointInitData>(
	    &jointBlock, (u32)(uintptr_t)jointBlock.mpJointInitData);
	mIndexTable    = JSUConvertOffsetToPtr<u16>(
	    &jointBlock, (u32)(uintptr_t)jointBlock.mpIndexTable);
}

J3DJoint* J3DJointFactory::create(int jntNo)
{
	J3DJoint* joint = new J3DJoint();
	joint->mJntNo   = jntNo;

	joint->mKind            = getKind(jntNo);
	joint->mScaleCompensate = getScaleCompensate(jntNo);
	joint->mTransformInfo   = getTransformInfo(jntNo);

	joint->mRadius = getRadius(jntNo);

	joint->mMin = getMin(jntNo);
	joint->mMax = getMax(jntNo);

	joint->mMtxCalc    = nullptr;
	joint->mOldMtxCalc = nullptr;

	if (joint->mScaleCompensate == 0xFF) {
		joint->mScaleCompensate = false;
	}

	return joint;
}
