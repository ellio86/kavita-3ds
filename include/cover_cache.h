#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

/* Store / load raw cover image bytes under sdmc:/3ds/kavita-3ds/covers/<fp>/
   where fp is derived from base_url. kind: 's' series, 'v' volume, 'c' chapter. */

bool cover_cache_read(const char* base_url, char kind, int id,
                      u8** out_data, size_t* out_size);

bool cover_cache_write(const char* base_url, char kind, int id,
                       const u8* data, size_t size);
