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
bool ParseIdList(std::string_view text, std::uint32_t* out,
                 std::size_t capacity);

// Parses "0xSRC=0xDST,0xSRC2=0xDST2" into a map. Whitespace tolerant.
std::map<std::uint32_t, std::uint32_t> ParseKtidPairs(std::string_view text);

// Reads the whole ini and republishes every key into the shared atomics.
// param is null on hot reload, in which case the directory of the previous
// config path is reused.
void LoadConfig(const Nioh3PluginInitializeParam* param);

// --- the only values LoadConfig touches that state.h/core.h do not declare ---
// Everything else it reads or writes is already declared in state.h, or in
// core.h for the discover-mode state. These four are declared here because
// state.h was generated from the modules' needs and predates the split:
//   g_mapRankMode / g_mapRankEvery -> defined in src/maps.cpp (ApplyMapRank)
//   g_ktidSwapMap / g_identityTags -> defined in main.cpp
extern std::atomic<std::uint32_t> g_mapRankMode;
extern std::atomic<std::uint32_t> g_mapRankEvery;
extern std::atomic<std::shared_ptr<const std::map<std::uint32_t, std::uint32_t>>>
    g_ktidSwapMap;
extern std::atomic<std::shared_ptr<const std::map<std::uint32_t, std::uint32_t>>>
    g_identityTags;
