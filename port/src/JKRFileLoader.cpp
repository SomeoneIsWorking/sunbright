// OWNED COPY of reference/sms/src/JSystem/JKernel/JKRFileLoader.cpp — keep in sync.
//
// 64-bit-port fix (covariant-return / stray-long class): the shadow
// JKRFileLoader.hpp declares the static `getResSize(void*, JKRFileLoader*)`
// returning `s32` (the virtual getResSize(_30) was retyped long->s32 so the
// JKRArchive override matches on LP64; see the shadow header note). The decomp
// .cpp defined the static as `long`, which is != s32 on LP64 -> "no declaration
// matches". The function only ever returns -1 or the virtual's `s32`, so
// retyping the definition to `s32` is behavior-preserving. Only the return type
// (and its local `ret`) changed; the rest is a verbatim copy of the decomp.
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <ctype.h>
#include <macros.h>

JSUList<JKRFileLoader> JKRFileLoader::sVolumeList;
JKRFileLoader* JKRFileLoader::sCurrentVolume;

JKRFileLoader::JKRFileLoader()
    : JKRDisposer()
    , mFileLoaderLink(this)
{
	mVolumeName = nullptr;
	mVolumeType = 0;
	mMountCount = 0;
}

JKRFileLoader::~JKRFileLoader()
{
	if (sCurrentVolume == this)
		sCurrentVolume = nullptr;
}

void JKRFileLoader::unmount()
{
	if (mMountCount != 0) {
		if (--mMountCount == 0)
			delete this;
	}
}

JKRFileLoader* JKRFileLoader::getVolume(const char* name)
{
	for (JSUListIterator<JKRFileLoader> it = sVolumeList.getFirst();
	     it != sVolumeList.getEnd(); ++it) {
		if (strcmp(name, it->mVolumeName) == 0)
			return it.getObject();
	}

	return nullptr;
}

void JKRFileLoader::changeDirectory(const char* dir)
{
	JKRFileLoader* vol = findVolume(&dir);
	if (vol)
		vol->becomeCurrent(dir);
}

void* JKRFileLoader::getGlbResource(const char* path)
{
	JKRFileLoader* loader = findVolume(&path);
	return (loader == nullptr) ? nullptr : loader->getResource(path);
}

void* JKRFileLoader::getGlbResource(const char* name, JKRFileLoader* fileLoader)
{
	void* resource = nullptr;
	if (fileLoader) {
		return fileLoader->getResource(0, name);
	}

	JSUList<JKRFileLoader>& volumeList = getVolumeList();
	for (JSUListIterator<JKRFileLoader> it = volumeList.getFirst();
	     it != volumeList.getEnd(); ++it) {
		resource = it->getResource(0, name);
		if (resource)
			break;
	}
	return resource;
}

s32 JKRFileLoader::getResSize(void* resourceBuffer, JKRFileLoader* fileLoader)
{
	s32 ret = -1; // long->s32 (LP64); see file header note

	if (fileLoader != nullptr)
		return fileLoader->getResSize(resourceBuffer);

	for (JSUListIterator<JKRFileLoader> it = sVolumeList.getFirst();
	     it != sVolumeList.getEnd(); ++it) {
		ret = it.getObject()->getResSize(resourceBuffer);
		if (ret >= 0)
			break;
	}

	return ret;
}

JKRFileLoader* JKRFileLoader::findVolume(const char** volumeName)
{
	if (*volumeName[0] != '/') {
		return sCurrentVolume;
	}

	char volumeNameBuffer[0x101];
	*volumeName = fetchVolumeName(volumeNameBuffer,
	                              ARRAY_COUNT(volumeNameBuffer), *volumeName);

	for (JSUListIterator<JKRFileLoader> it = sVolumeList.getFirst();
	     it != sVolumeList.getEnd(); ++it) {
		if (strcmp(volumeNameBuffer, it->mVolumeName) == 0)
			return it.getObject();
	}
	return nullptr;
}

JKRFileFinder* JKRFileLoader::findFirstFile(const char* volumeName)
{
	JKRFileFinder* ret = nullptr;

	JKRFileLoader* vol = findVolume(&volumeName);
	if (vol)
		ret = vol->getFirstFile(volumeName);

	return ret;
}

const char* JKRFileLoader::fetchVolumeName(char* buffer, long bufferSize,
                                           const char* path)
{
	static char rootPath[] = "/";
	if (strcmp(path, "/") == 0) {
		strcpy(buffer, rootPath);
		return rootPath;
	} else {
		path++;
		while (*path != 0 && *path != '/') {
			if (1 < bufferSize) {
				*buffer = _tolower(*path);
				buffer++;
				bufferSize--;
			}
			path++;
		}
		buffer[0] = '\0';
		if (path[0] == '\0')
			path = rootPath;
	}

	return path;
}
