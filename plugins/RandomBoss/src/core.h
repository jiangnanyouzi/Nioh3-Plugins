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
inline constexpr const char* kConfigKeyBlacklist = "Blacklist";
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
// a key is missing or empty. They live here because src/config.cpp reads them.
// ---------------------------------------------------------------------------
// TargetId used to live here as kDefaultTargetId (0xA263C, Gozuki) and was
// removed on 2026-09-21. It was a SECOND place to state "which enemy is ours"
// alongside MapPool, the two could disagree, and when they did the purple
// marking was silently disabled: the marking test compared against TargetId
// alone, so changing MapPool to another enemy left every spawn plain with no log
// line saying why. Membership of MapPool / MapPool_<SRC> is now the only
// definition of "ours" (see IsMapTargetKey), so there is nothing left for a
// standalone target id to express. Do not reintroduce one.
// The map SOURCE set used to live here as kDefaultMapSources (18 Jailer Oni + 8
// Shunobon variants) and the map TARGET pool as kDefaultMapPool (8 boss ids).
// Both are config-only now: the lists live in RandomBoss.ini under MapSources /
// MapPool, and an empty value means "nothing configured", i.e. no swapping at
// all — config.cpp logs a warning in that case instead of silently substituting
// a built-in list. Do not reintroduce a table here: the whole point of moving
// them was that adding an enemy variant must not require a rebuild, and a
// hidden list is what made "why did adding a variant change nothing?"
// unanswerable from the log. Practical note for MapPool: the replacement is
// picked at random per placement (MapRandomMode=1 keeps each placement stable,
// so the same spot always yields the same boss), and 0xA263C Gozuki is the only
// target verified end-to-end, so prune any entry that crashes.
// The generic battle-data component (id 100) must never be swapped: every
// entity's HP container creation flows through it.
inline constexpr std::uint32_t kDefaultBlacklist[] = {0x64};

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
