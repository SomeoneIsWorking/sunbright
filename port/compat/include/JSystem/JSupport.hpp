#ifndef J_SUPPORT_HPP
#define J_SUPPORT_HPP

#include <types.h>

template <typename T> T* JSUConvertOffsetToPtr(const void* ptr, u32 offset)
{
	if (offset == nullptr) {
		return nullptr;
	} else {
		return (T*)((uintptr_t)ptr + offset);
	}
}

template <typename T>
T* JSUConvertOffsetToPtr(const void* ptr, const void* offset)
{
	if (offset == nullptr) {
		return nullptr;
	} else {
		return (T*)((uintptr_t)ptr + (uintptr_t)offset);
	}
}

inline u8 JSULoByte(u16 in) { return in & 0xff; }
inline u8 JSUHiByte(u16 in) { return in >> 8; }

#endif
