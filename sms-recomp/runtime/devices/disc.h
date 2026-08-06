// disc.h — raw random-access reads of the mounted disc image. See disc.cpp.
#pragma once

#include "cpu_state.h"

bool disc_open(const char* path);
bool disc_is_open();
void disc_read(u64 offset, void* out, u32 len);
void disc_close();
