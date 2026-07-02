// sms_boot_lazy_singleton.h — pure shim for the "load, then some ctor nulls,
// then a downstream reader wants the value back" load-order problem the
// TLightCommon subsystem exhibits at file-select (see the block above
// sb_light_ary_or_search() in reference/sms/src/MarioUtil/LightUtil.cpp).
//
// The RE ctor unconditionally nulls a set of SDA singletons; loadAfter then
// re-populates them; but between loadAfter and the perform-list phase where
// the singletons are read, TLightWithDBSet::makeDrawBuffer creates 4 more
// TLightCommon "owner" objects whose ctors null the singletons AGAIN. On
// hardware/Dolphin this is masked by the plain TLightCommon scene object
// never actually taking the group-read path at file-select (mEnabled=0 on
// the DB sets, DB-set perform never fires). Native does trip it.
//
// This header captures the tiny pure contract of the shim so a Dolphin-free
// unit test can lock it in place:
//   - if the cached pointer is non-null, return it AS IS (RE-faithful path);
//   - if null, invoke the searcher (typically TNameRefGen::search<T>(name)),
//     write the result BACK into the cache, and return it. Null result stays
//     null and does not deadlock the caller (accessors must still guard).
//
// The caller-supplied Searcher captures the actual search — in production the
// searcher is `[]{ return TNameRefGen::search<TLightAry>("Light Group"); }`.
// The template avoids the need to link the entire scene tree into the test.

#pragma once

namespace sb::lazy_singleton {

// Returns `cache` when non-null, else calls `search()` and writes result to
// `cache` before returning. `search` is any invocable returning `T*`.
//
// PPC RE (getLightPosition group path, @0x80229ccc):
//   lwz r5, -0x6114(r13)       ; r5 = gp  (NO NULL GUARD)
//   ...                        ; deref r5 immediately
// Native shim inserts the (r5 == 0 → search + writeback) branch upstream so
// r5 arrives non-null when the scene has the Light Group registered.
template <typename T, typename Searcher>
T* get_or_search(T*& cache, Searcher search)
{
	if (cache) return cache;
	cache = search();
	return cache;
}

// Idempotence contract, for the unit test to lock in:
//   - Calling twice in a row with the same non-null cache returns the same
//     pointer without calling `search`.
//   - Calling with a null cache invokes `search` exactly once, stores the
//     result, and a subsequent call returns without invoking `search` again.
//   - `search` returning null keeps `cache` null AND is the shim's honest
//     failure mode (upstream accessors must still null-guard).

} // namespace sb::lazy_singleton
