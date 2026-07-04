// sms_boot_light_search.h — pure name-search helper shared by the four
// TLightWithDBSet::makeDrawBuffer subclass ports (@0x802289ac / 0x80228b74 /
// 0x80228d08 / 0x80228eac). Each subclass makeDrawBuffer runs the same
// strcmp-loop shape twice — once over TLightAry::mLights[] (stride 0x6c =
// sizeof(TIdxLight)) and once over TAmbAry::mAmbColors[] (stride 0x18 =
// sizeof(TAmbColor)) — to resolve a Light-Group / Amb-Group index it hands to
// each per-drawbuffer TLightCommon owner.
//
// Extracted so the search shape (which IS the pure computable logic) can be
// unit-tested spec-derived from the disasm without a live Light Group.
//
// Regressions this catches:
//   * "no match → 0" (would leak Light-Group[0] into every unnamed set).
//   * Empty/null array not returning -1 (SEGV when TLightAry->mLights is
//     nullptr before scene-load).
//   * "return LAST match" vs "return FIRST match" — the guest loop breaks on
//     first match (bne skip; b out), so first-wins is the RE'd behaviour.
//   * Off-by-one loop bound (< count vs <= count).
#pragma once

#include <cstring>

namespace sb::light_search {

template <typename T>
inline int find_named_index(T* arr, int count, const char* needle)
{
	if (!arr || !needle) return -1;
	for (int i = 0; i < count; ++i) {
		const char* n = arr[i].getName();
		if (n && std::strcmp(n, needle) == 0) return i;
	}
	return -1;
}

} // namespace sb::light_search
