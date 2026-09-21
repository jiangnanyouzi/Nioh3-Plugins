// state.h - declarations for the shared mutable state of RandomBoss.
//
// GENERATED from main.cpp's definitions (which stay in main.cpp). The split-out
// modules include this to compile against that state; nothing is defined here.
// Re-generate with analysis/experiments/ if a global is added or renamed.
//
// The definitions previously sat in an anonymous namespace. That is why this
// header alone is not enough - main.cpp's namespace had to be opened so the
// objects actually have external linkage.
#pragma once

#include "core.h"

// Container sizes referenced by the state below.
inline constexpr std::size_t kMapMemoSlots = 256;
inline constexpr std::size_t kAssetTraceSlots = 512;
inline constexpr std::size_t kPairMapMax = 16;
inline constexpr std::size_t kPairPatchSlots = 256;

extern std::atomic_bool g_mapPurpleHooked;
extern std::atomic<std::uint64_t> g_fepLogged;
extern std::atomic<std::uint64_t> g_fepSeen;
extern std::atomic<std::uint32_t> g_mapPurpleMarkLogs;
extern std::atomic_bool g_mapBoss;
extern std::atomic_bool g_mapHooked;
extern std::atomic<std::uint32_t> g_mapRandomMode;
extern std::atomic<std::uint32_t> g_mapSources[kMaxListEntries];
extern std::atomic<std::size_t> g_mapSourceCount;
extern std::atomic<std::uint32_t> g_mapPool[kMaxListEntries];
extern std::atomic<std::size_t> g_mapPoolCount;
extern std::atomic<std::uint64_t> g_mapSwapCount;
extern std::atomic<std::uint32_t> g_mapMemoRecord[kMapMemoSlots];
extern std::atomic<std::uint32_t> g_mapMemoSource[kMapMemoSlots];
extern std::atomic<std::uint64_t> g_mapSweepHits;
extern std::atomic<std::size_t> g_mapKeyPoolCount;
extern std::atomic_bool g_mapHookEnabled;
extern std::atomic_bool g_mapTableDone;
extern std::atomic<std::uint64_t> g_mapTableHits;
extern std::atomic<std::uint32_t> g_targetId;
extern std::atomic<std::uint32_t> g_targetFlags;
extern std::atomic<std::uint32_t> g_mapPurpleFlagWrites;
extern std::atomic<std::uint32_t> g_sourceIds[kMaxListEntries];
extern std::atomic<std::size_t> g_sourceCount;
extern std::atomic_bool g_swapAll;
extern std::atomic<std::uint32_t> g_blacklist[kMaxListEntries];
extern std::atomic<std::size_t> g_blacklistCount;
extern std::atomic<std::uint64_t> g_swapCount;
extern std::atomic<std::uint64_t> g_seenCount;
extern std::atomic<std::uint64_t> g_assetSwapCount;
extern std::atomic<std::uint64_t> g_assetMissCount;
extern std::atomic<std::uint32_t> g_assetTraceKey[kAssetTraceSlots];
extern std::atomic<std::uint32_t> g_assetTraceHits[kAssetTraceSlots];
extern std::atomic<std::uint64_t> g_assetTraceTotal;
extern std::atomic_bool g_assetTrace;
extern std::atomic_bool g_factoryDiag;
extern std::atomic_bool g_factorySwap;
extern std::atomic_bool g_assetSwap;
extern std::atomic_bool g_createSwap;
extern std::atomic_bool g_catalogSwap;
extern std::atomic_bool g_rosterSwap;
extern std::atomic<std::uint32_t> g_createSources[kMaxListEntries];
extern std::atomic<std::size_t> g_createSourceCount;
extern std::atomic<std::uint32_t> g_createTarget;
extern std::atomic<std::uint64_t> g_createSwapCount;
extern std::atomic<std::uint32_t> g_createMode;
extern std::atomic_uint64_t g_createSwapTimes[8];
extern std::atomic<std::uint32_t> g_createSwapSlot;
extern std::atomic<std::uint32_t> g_createMaxPerMinute;
extern std::filesystem::path g_configPath;
extern FILETIME g_configMtime;
extern std::atomic_bool g_identitySwap;
extern std::atomic<std::uint32_t> g_identityFrom[kMaxListEntries];
extern std::atomic<std::size_t> g_identityFromCount;
extern std::atomic<std::uint32_t> g_identityTo;
extern std::atomic<std::uint64_t> g_identitySwapCount;
extern std::atomic_bool g_pairSwap;
extern std::atomic<std::uint32_t> g_pairFromKey;
extern std::atomic<std::uint32_t> g_pairToKey;
extern std::atomic<std::uint32_t> g_pairTags[kMaxListEntries];
extern std::atomic<std::size_t> g_pairTagCount;
extern std::atomic_bool g_pairRevert;
extern std::atomic<std::uint32_t> g_pairMapTag[kPairMapMax];
extern std::atomic<std::uint32_t> g_pairMapKey[kPairMapMax];
extern std::atomic<std::size_t> g_pairMapCount;
extern std::atomic<std::uint64_t> g_configGeneration;
extern std::atomic<std::uintptr_t> g_pairPatchedAddr[kPairPatchSlots];
extern std::atomic<std::uint32_t> g_pairPatchedOrig[kPairPatchSlots];
extern std::atomic<std::size_t> g_pairPatchedCount;
extern std::atomic<std::uint64_t> g_pairSwapCount;

// Placement key pools (defined in main.cpp, used by maps.cpp).
extern MapKeyPool g_mapKeyPools[kMapPoolSlots];

// --- hook resume addresses and hook objects ---------------------------------
// Defined in main.cpp next to the stub they belong to. The stubs themselves are
// written in asm (HookStub.asm) and jump through these, so the names must keep
// C linkage.
extern "C" std::uint64_t g_mapResume;
extern "C" std::uint64_t g_mapPurpleResume;
extern safetyhook::InlineHook g_mapPurpleHook;
extern safetyhook::InlineHook g_mapHook;

// --- purple (ichi-nan) state ------------------------------------------------
// kEntityVariantOffset/kEntityKeyOffset deliberately stay in purple.cpp: only
// that module reads the entity layout.
extern std::atomic<std::uint32_t> g_mapPurple;
extern std::atomic<std::uint64_t> g_mapPurpleFlagged;

// MapForceEmpower (ini key, default 0 = off). NOPs the two-byte `jne` in the
// entity activation path that sends an "already killed" placement down the
// plain branch (see kRevivePlainBranchPattern). This is the ONLY thing found so
// far that actually makes the plain Gozuki purple, because the decision is made
// per spawn from the record's instanceId, after every record field has been
// read. Done once, in memory only; the original bytes are never overwritten on
// disk and the patch disappears when the game exits.
extern std::atomic_bool g_mapForceEmpower;
// True once the patch has been applied (or found already applied).
extern std::atomic_bool g_reviveBranchPatched;

// Log cap for the factory-entry sampling in the purple module.
inline constexpr std::uint64_t kFepLogMax = 256;

// --- lifecycle --------------------------------------------------------------
extern std::atomic_bool g_shutdown;
