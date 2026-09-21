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

// --- the only values LoadConfig touches that state.h/core.h do not declare ---
// These two are declared here because state.h was generated from the modules'
// needs and predates the split:
//   g_mapRankMode / g_mapRankEvery -> defined in src/maps.cpp (ApplyMapRank)
extern std::atomic<std::uint32_t> g_mapRankMode;
extern std::atomic<std::uint32_t> g_mapRankEvery;
