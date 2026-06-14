// SHADOW of reference/sms/include/JSystem/JKernel/JKRFileLoader.hpp.
// Identical to the decomp EXCEPT the virtual getResSize (_30) is declared
// returning literal `long`, while every concrete override (e.g.
// JKRArchive::getResSize in JKRArchive.hpp) returns `s32`. On the 32-bit GC
// `long` and `s32`(int) were both 4 bytes, so the override matched. With the
// shadow dolphin/types.h making s32==int(4) on LP64 while `long`==8, the virtual
// override is rejected ("conflicting return type"). Fix: declare the virtual
// returning `s32`, matching the GC's 4-byte width and the overrides. The static
// (non-virtual) getResSize overload is changed to `s32` too for consistency (it
// has no covariance constraint but represents the same 4-byte resource size).
// Behavior-preserving. Keep in sync with the decomp.
#ifndef JKR_FILELOADER_HPP
#define JKR_FILELOADER_HPP

#include <JSystem/JKernel/JKREnum.hpp>
#include <JSystem/JKernel/JKRDisposer.hpp>
#include <JSystem/JSupport/JSUList.hpp>

class JKRFileFinder;

class JKRFileLoader : public JKRDisposer {
public:
	JKRFileLoader();

	virtual ~JKRFileLoader(); // _08
	virtual void unmount();   // _0C
	static JKRFileLoader* getVolume(const char*);
	virtual bool becomeCurrent(const char*)               = 0; // _10
	virtual void* getResource(const char* path)           = 0; // _14
	virtual void* getResource(u32 type, const char* name) = 0; // _18
	virtual size_t readResource(void* resourceBuffer, u32 bufferSize,
	                            const char* path)
	    = 0; // _1C
	virtual size_t readResource(void* resourceBuffer, u32 bufferSize, u32 type,
	                            const char* name)
	    = 0;                                                    // _20
	virtual void removeResourceAll()                       = 0; // _24
	virtual bool removeResource(void*)                     = 0; // _28
	virtual bool detachResource(void*)                     = 0; // _2C
	virtual s32 getResSize(const void*) const              = 0; // _30
	virtual u32 countFile(const char*) const               = 0; // _34
	virtual JKRFileFinder* getFirstFile(const char*) const = 0; // _38

	bool isMounted() const { return mIsMounted; }
	u32 getVolumeType() const { return mVolumeType; }

	static void changeDirectory(const char* dir);

	static void* getGlbResource(const char*);
	static void* getGlbResource(const char*, JKRFileLoader* fileLoader);
	static s32 getResSize(void* resourceBuffer, JKRFileLoader* fileLoader);
	static size_t readGlbResource(void* resourceBuffer, u32 bufferSize,
	                              const char* path,
	                              JKRExpandSwitch expandSwitch);

	static bool removeResource(void* resourceBuffer, JKRFileLoader* fileLoader);
	static bool detachResource(void* resourceBuffer, JKRFileLoader* fileLoader);

	static JKRFileLoader* findVolume(const char**);
	static JKRFileFinder* findFirstFile(const char*);
	static const char* fetchVolumeName(char*, long, const char*);

	static JKRFileLoader* getCurrentVolume() { return sCurrentVolume; }
	static void setCurrentVolume(JKRFileLoader* fileLoader)
	{
		sCurrentVolume = fileLoader;
	}
	static JSUList<JKRFileLoader>& getVolumeList() { return sVolumeList; }

	static JKRFileLoader* sCurrentVolume;
	static JSUList<JKRFileLoader> sVolumeList;

protected:
	/* 0x00 */                              // vtable
	/* 0x04 */                              // JKRDisposer
	JSULink<JKRFileLoader> mFileLoaderLink; // 0x18
	const char* mVolumeName;                // 0x28
	u32 mVolumeType;                        // 0x2C
	bool mIsMounted;                        // 0x30
	u8 field_0x31[3];                       // 0x31
	u32 mMountCount;                        // 0x34
};

inline bool JKRDetachResource(void* resource, JKRFileLoader* fileLoader)
{
	return JKRFileLoader::detachResource(resource, fileLoader);
}

inline void* JKRGetNameResource(const char* name, JKRFileLoader* loader)
{
	return JKRFileLoader::getGlbResource(name, loader);
}

inline void* JKRGetResource(const char* name)
{
	return JKRFileLoader::getGlbResource(name);
}

#endif
