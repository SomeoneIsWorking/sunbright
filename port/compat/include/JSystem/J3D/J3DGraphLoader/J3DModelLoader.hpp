// SHADOW of reference/sms/include/JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp
// — keep in sync. ONLY change vs the decomp: the FILE-OVERLAY block structs
// declare their offset members as `u32` instead of `void*`/typed pointers.
//
// These structs are overlaid directly on the big-endian BMD file image, where
// each offset field is 4 bytes. On the GameCube `sizeof(void*)==4`, so the decomp
// overlay works. On a 64-bit host `sizeof(void*)==8`, which both mis-aligns every
// subsequent field AND over-reads 8 bytes per offset (-> wild pointers; readDraw
// SEGV). Declaring the offsets as `u32` restores the 4-byte file layout; the
// loader passes them to JSUConvertOffsetToPtr(base, u32) (a real overload), which
// yields the correct host pointer. (LP64 struct-overlay class — see
// docs/re_notes/first_flip_endianness.md. The bmd_swap layer makes the VALUES
// host-endian; this shadow makes the LAYOUT host-correct.)
#ifndef J3D_MODEL_LOADER_HPP
#define J3D_MODEL_LOADER_HPP

#include <types.h>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JUtility/JUTDataHeader.hpp>

struct J3DModelInfoBlock : public JUTDataBlockHeader {
	/* 0x08 */ u16 mFlags;
	/* 0x0C */ u32 mPacketNum;
	/* 0x10 */ u32 mVtxNum;
	/* 0x14 */ u32 mpHierarchy;       // was void*
}; // Size: 0x18

struct J3DVertexBlock : public JUTDataBlockHeader {
	/* 0x08 */ u32 mpVtxAttrFmtList;  // was void*
	/* 0x0C */ u32 mpVtxPosArray;
	/* 0x10 */ u32 mpVtxNrmArray;
	/* 0x14 */ u32 mpVtxNBTArray;
	/* 0x18 */ u32 mpVtxColorArray[2];
	/* 0x20 */ u32 mpVtxTexCoordArray[8];
}; // Size: 0x40

struct J3DEnvelopBlock : public JUTDataBlockHeader {
	/* 0x08 */ u16 mWEvlpMtxNum;
	/* 0x0C */ u32 mpWEvlpMixMtxNum;    // was void*
	/* 0x10 */ u32 mpWEvlpMixMtxIndex;
	/* 0x14 */ u32 mpWEvlpMixWeight;
	/* 0x18 */ u32 mpInvJointMtx;
}; // Size: 0x1C

struct J3DDrawBlock : public JUTDataBlockHeader {
	/* 0x08 */ u16 mMtxNum;
	/* 0x0C */ u32 mpDrawMtxFlag;     // was void*
	/* 0x10 */ u32 mpDrawMtxIndex;
}; // Size: 0x14

struct J3DJointBlock;
struct J3DMaterialBlock;
struct J3DMaterialBlock_v21;

struct J3DMaterialDLBlock : public JUTDataBlockHeader {
	/* 0x08 */ u16 mMaterialNum;
	/* 0x0C */ u32 mpDisplayListInit; // was void*
	/* 0x10 */ u32 mpPatchingInfo;
	/* 0x14 */ u32 mpCurrentMtxInfo;
	/* 0x18 */ u32 field_0x18;
	/* 0x1C */ u32 field_0x1c;
	/* 0x20 */ u32 mpNameTable;
	/* more */
};

struct J3DShapeBlock;

struct J3DTextureBlock : public JUTDataBlockHeader {
	/* 0x08 */ u16 mTextureNum;
	/* 0x0C */ u32 mpTextureRes;      // was void*
	/* 0x10 */ u32 mpNameTable;
};

class J3DModelLoaderDataBase {
public:
	static J3DModelData* load(const void*, u32);
	static J3DMaterialTable* loadMaterialTable(const void*);
};

class J3DModelLoader {
public:
	J3DModelLoader()
	{
		mpModelData     = nullptr;
		mpShapeBlock    = nullptr;
		mpMaterialTable = nullptr;
	}

	virtual J3DModelData* load(const void*, u32);
	virtual J3DMaterialTable* loadMaterialTable(const void*);
	virtual void setupBBoardInfo();
	virtual ~J3DModelLoader() { }
	virtual void readMaterial(const J3DMaterialBlock*, u32) { }
	virtual void readMaterial_v21(const J3DMaterialBlock_v21*, u32) { }
	virtual void readMaterialTable(const J3DMaterialBlock*, u32) { }
	virtual void readMaterialTable_v21(const J3DMaterialBlock_v21*, u32) { }

	void readInformation(const J3DModelInfoBlock*, u32);
	void readVertex(const J3DVertexBlock*);
	void readEnvelop(const J3DEnvelopBlock*);
	void readDraw(const J3DDrawBlock*);
	void readJoint(const J3DJointBlock*);
	void readShape(const J3DShapeBlock*, u32);
	void readTexture(const J3DTextureBlock*);
	void readTextureTable(const J3DTextureBlock*);

protected:
	/* 0x04 */ J3DModelData* mpModelData;
	/* 0x08 */ J3DMaterialTable* mpMaterialTable;
	/* 0x0C */ const J3DShapeBlock* mpShapeBlock;
};

class J3DModelLoader_v26 : public J3DModelLoader {
public:
	virtual ~J3DModelLoader_v26() { }
	virtual void readMaterial(const J3DMaterialBlock*, u32);
	virtual void readMaterialTable(const J3DMaterialBlock*, u32);
};

class J3DModelLoader_v21 : public J3DModelLoader {
public:
	~J3DModelLoader_v21() { }
	void readMaterial_v21(const J3DMaterialBlock_v21*, u32);
	void readMaterialTable_v21(const J3DMaterialBlock_v21*, u32);
};

#endif
