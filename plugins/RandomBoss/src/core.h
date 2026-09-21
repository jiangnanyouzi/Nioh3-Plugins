// core.h - shared constants, types and function declarations for RandomBoss.
//
// RandomBoss was a single 2700-line main.cpp; it is being split by feature.
// This header holds what more than one module needs. Only constants and
// declarations live here - every definition stays in exactly one .cpp.
//
// Note: main.cpp still keeps its anonymous namespace and includes this header
// from inside it. That is why the declarations below are written without a
// namespace of their own: the includer supplies it.
#pragma once

#define NOMINMAX
#include <Windows.h>

#include <HookUtils.h>
#include <LogUtils.h>
#include <PluginAPI.h>
#include <Relocation.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Plugin identity + config keys. inline constexpr so all translation units
// share one object each instead of getting private copies.
// ---------------------------------------------------------------------------
inline constexpr const char* kPluginName = "RandomBoss";
inline constexpr const char* kConfigSection = "RandomBoss";
inline constexpr const char* kConfigKeyTarget = "TargetId";
inline constexpr const char* kConfigKeySources = "SourceIds";
inline constexpr const char* kConfigKeyBlacklist = "Blacklist";
inline constexpr const char* kConfigKeyDiscover = "Discover";
inline constexpr const char* kConfigKeyCreateSwap = "CreateSwap";
inline constexpr const char* kConfigKeyCreateSources = "CreateSources";
inline constexpr const char* kConfigKeyCreateTarget = "CreateTarget";
// IdentitySwap (2026-09-18 CE 结论): rewrite the packed entity identity at the
// slot-identity writer. Opt-in; the tag must come from the canonical table.
inline constexpr const char* kConfigKeyIdentitySwap = "IdentitySwap";
inline constexpr const char* kConfigKeyIdentityFrom = "IdentityFrom";
inline constexpr const char* kConfigKeyIdentityTo = "IdentityTo";
inline constexpr const char* kConfigKeyIdentityTags = "IdentityTags";
// PairSwap (2026-09-18 CE 终局结论): a summon's enemy is decided by the PAIR of
// (request descriptor, parameter-table entry); rewriting only one side aborts
// the spawn silently, rewriting the key on both sides swaps the enemy.
inline constexpr const char* kConfigKeyPairSwap = "PairSwap";
inline constexpr const char* kConfigKeyPairFrom = "PairFromKey";
inline constexpr const char* kConfigKeyPairTo = "PairToKey";
inline constexpr const char* kConfigKeyPairTags = "PairTags";
inline constexpr const char* kConfigKeyPairRevert = "PairRevert";
inline constexpr const char* kConfigKeyPairMap = "PairMap";
// AssetTrace (diagnostic, 2026-09-19). Records which ktids pass through
// AssetIdManager::GetResIdByFileKtid and reports the top-16 per 10 s window.
// It answers the two questions a file-layer (ktid) redirect depends on:
//   1. does the asset load path even go through this function?
//   2. which ids does a summon actually resolve (and when)?
// Rationale: a failed swap test proved the *mechanism* works (redirect fired,
// target resolved, 0 misses) while the target file was never read from disk —
// i.e. the redirected id was not the one that produced the visible model.
inline constexpr const char* kConfigKeyAssetTrace = "AssetTrace";
// FactoryDiag (diagnostic, 2026-09-21). Installs the factory-REAL-entry hook
// (kFactoryEntryPattern) on its own, without enabling any marking, and logs a
// field snapshot for the first N target-key entities that pass through it.
// Purpose: the 一難/purple variant is NOT the runtime bit at entity+0xE8 (that
// was disproven by controlled write tests) and NOT the placement record's
// flags (the same record yields purple on some loads and plain on others).
// This hook is the one point that sees every creation, so a single map load
// yields the field comparison that pins the real determinant down.
inline constexpr const char* kConfigKeyFactoryDiag = "FactoryDiag";
// Source-level map swap (see kMapPlacementKeyPattern / MapBossHookStub).
inline constexpr const char* kConfigKeyMapBoss = "MapBoss";
inline constexpr const char* kConfigKeyMapSources = "MapSources";
inline constexpr const char* kConfigKeyMapPool = "MapPool";
inline constexpr const char* kConfigKeyMapRandomMode = "MapRandomMode";

// Upper bound for every id list parsed out of the ini.
inline constexpr std::size_t kMaxListEntries = 256;

// ---------------------------------------------------------------------------
// Default values written into the ini (and used as the in-memory fallback) when
// a key is missing or empty. They live here because src/config.cpp reads them
// and main.cpp seeds g_targetId / g_createTarget with them.
// ---------------------------------------------------------------------------
// Default target: Gozuki (牛头鬼) from the live-verified enemy catalog.
inline constexpr std::uint32_t kDefaultTargetId = 0xA263C;
// The map SOURCE set used to live here as kDefaultMapSources (18 Jailer Oni + 8
// Shunobon variants). It is now config-only: the list lives in RandomBoss.ini
// under MapSources, and an empty MapSources means "no source keys", i.e. no
// swapping at all — config.cpp logs a warning in that case instead of silently
// substituting a built-in list. Do not reintroduce a table here: the whole point
// of moving it was that adding an enemy variant must not require a rebuild.
// Default map TARGET pool: random per placement (MapRandomMode=1 keeps each
// placement stable, so the same spot always yields the same boss). 0xA263C
// Gozuki is the only target verified end-to-end; prune any entry that crashes,
// or force the NG++ tier with MapRank.
inline constexpr std::uint32_t kDefaultMapPool[] = {
    0x0A263C, 0x0782F6, 0x0C76B4, 0x040A3B,
    0x05AAF9, 0x041DB6, 0x093F79, 0x0D1F46};
// The generic battle-data component (id 100) must never be swapped: every
// entity's HP container creation flows through it.
inline constexpr std::uint32_t kDefaultBlacklist[] = {0x64};
// CreateSwap default sources: the three training-room bosses whose keys were
// verified by multi-capture on 2026-09-18 (武者/忍者/狱卒鬼).
inline constexpr std::uint32_t kDefaultCreateSources[] = {0x93457, 0x1B6CC,
                                                          0xBC496};

// Enemy-id range and the discover-mode scan budget
constexpr std::uint32_t kIdRangeMin = 0x1000;
constexpr std::uint32_t kIdRangeMax = 0xFFFFF;
// RDX is the big spawn-director object (fields observed up to +0x44D0);
// the identity dword is beyond the first KB, so scan the whole thing.
// Cost: 0x4800/4 dwords per assembly-main call — trivial even in a storm.
constexpr std::uint32_t kDiscoverScanBytes = 0x4800;
constexpr std::uint32_t kDiscoverMaxHits = 65536;

// ---------------------------------------------------------------------------
// Placement key pools: per-source-enemy target pools from the ini, used by the
// map swap (src/maps.cpp) and filled by the ini loader.
//
// kMapPoolSlots counts DISTINCT source keys, not ini lines: writing the same
// MapPool_<SRC> twice reuses the slot and the last line wins, so 40 lines over
// 3 sources still need only 3 slots. Raised 32 -> 256 on 2026-09-21 so it can
// never be the binding limit: MapSources itself holds up to kMaxListEntries
// (256) keys, and "every configured source may also have its own pool" is a
// far easier invariant to explain than "256 sources, but only 32 pools".
//
// The loader's loop is bounded by this constant (`slots < kMapPoolSlots`), so
// exhausting it stops the scan and silently ignores the remaining lines - with
// the two limits now equal, reaching it requires more than 256 distinct
// MapPool_ sources, which ParseIdList would have truncated anyway.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMapPoolMaxKeys = 32;
inline constexpr std::size_t kMapPoolSlots = 256;

struct MapKeyPool {
  std::atomic<std::uint32_t> source{0};  // 0 = free slot
  std::atomic<std::size_t> count{0};
  std::atomic<std::uint32_t> keys[kMapPoolMaxKeys];
};

// Training-room roster injection (src/roster.cpp).
inline constexpr std::uint32_t kRosterTrio[] = {0x93457, 0x1B6CC, 0xBC496};
inline constexpr std::uint32_t kRosterPatchSlot = 0x93457;  // 武者 slot -> target
inline constexpr std::uint32_t kRosterWindowBytes = 0x800;
inline constexpr std::uintptr_t kRosterScanMaxRegion = 0;  // 0 = no region cap

// A chunk of writable memory to sweep. Shared by roster.cpp and pair.cpp,
// both of which walk committed writable regions looking for packed keys.
struct RosterShard {
  const std::uint8_t* base;
  std::size_t size;
};

// Discover mode (src/discover.cpp)
//
// Records every dword in the spawn-request object that falls inside the enemy-id
// range, so the identity field offset calibrates itself from one load storm.
// ---------------------------------------------------------------------------
struct DiscoverHit {
  std::uint32_t offset;
  std::uint32_t value;
};
extern DiscoverHit g_discoverHits[kDiscoverMaxHits];
extern std::atomic<std::uint32_t> g_discoverCount;
extern std::atomic_bool g_discoverOverflow;
extern std::atomic_bool g_discoverMode;
extern std::filesystem::path g_discoverPath;

void DiscoverScan(void* requestObject);
void WriteDiscoverCensus();
