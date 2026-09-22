// config.h - the ini contract of RandomBoss.
//
// Included after core.h. Kept separate from core.h because core.h is also
// included from inside main.cpp's anonymous namespace; anything declared here
// must have external linkage, which is what the config translation unit needs.
#pragma once

#include "state.h"

// Parses a comma/semicolon/whitespace separated id list ("0x1361D,0x1A2E6")
// into out, returning the number of entries written. Used by LoadConfig and by
// the per-source pool reader below.
//
// The return type is std::size_t because every caller stores it as a length.
// It was declared `bool` until 2026-09-21, which silently turned any non-empty
// list into 1: only the first MapSources entry was ever treated as a source key
// (so Shunobon, entries 19-26, never swapped) and every MapPool_<SRC> pool was
// capped at one key. Keep this in sync with the definition in config.cpp.
std::size_t ParseIdList(std::string_view text, std::uint32_t* out,
                        std::size_t capacity);

// Reads the whole ini and republishes every key into the shared atomics.
// param is null on hot reload, in which case the directory of the previous
// config path is reused.
void LoadConfig(const Nioh3PluginInitializeParam* param);

// g_mapRankMode / g_mapRankEvery were declared here. DELETED 2026-09-22 together
// with MapRank / MapRankEvery / ApplyMapRank - see the tombstone in src/maps.cpp.
