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
extern std::atomic<std::uint32_t> g_targetFlags;
extern std::atomic<std::uint32_t> g_mapPurpleFlagWrites;
extern std::atomic<std::uint32_t> g_blacklist[kMaxListEntries];
extern std::atomic<std::size_t> g_blacklistCount;
extern std::filesystem::path g_configPath;
extern FILETIME g_configMtime;
extern std::atomic_bool g_factoryDiag;

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

// MapIgnoreBlocked (ini key, default 0 = off). NOPs the six-byte `jnl` in the
// placement disable pass that keeps a "placement+0x8D4 == 3" placement from ever
// producing a live enemy (see kBlockedPlacementPattern). Unlike MapForceEmpower
// this does NOT change how an enemy looks - it only stops the engine from
// permanently disabling the placement, so the load-time construction of the
// placement's components can run normally. Independent of MapPurple; in memory
// only, and gone when the game exits.
extern std::atomic_bool g_mapIgnoreBlocked;
// True once the patch has been applied (or found already applied).
extern std::atomic_bool g_blockedPlacementPatched;

// Log cap for the factory-entry sampling in the purple module.
inline constexpr std::uint64_t kFepLogMax = 256;

// --- lifecycle --------------------------------------------------------------
extern std::atomic_bool g_shutdown;
