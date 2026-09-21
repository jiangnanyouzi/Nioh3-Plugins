#define NOMINMAX

// Shared declarations and engine signatures live in their own headers.
#include "core.h"
#include "patterns.h"
#include "state.h"   // runtime state owned by this file / the split-out modules
#include "config.h"  // LoadConfig + the ini-derived knobs it publishes

// NOTE: these definitions used to sit in a single anonymous namespace wrapping
// the whole file. That namespace has been opened so the definitions have
// external linkage and the split-out modules (src/*.cpp) can reference them.
// The four genuinely file-local objects below stay 'static', which already
// gives them internal linkage.

#include "src/patterns.h"

// The remaining ini fallback values (kDefaultTargetId / kDefaultMapPool /
// kDefaultBlacklist / kDefaultCreateSources) live in src/core.h, and the ini
// reader that consumes them is src/config.cpp. The map SOURCE list is NOT here:
// it is config-only (RandomBoss.ini -> MapSources) since 2026-09-21.

// Discover mode: plausible enemy-id dword range (catalog keys are 20-bit).
using FnFactoryCore = void (*)(void* arg1, void* arg2, std::uint32_t spawnId,
                               void* paramObject);
using FnOrchestrator = void (*)(void* spawnObject, unsigned char flag);
using FnMenuEnqueue = void (*)(void* arg1, void* entity, void* arg3,
                               void* arg4);
using FnCatalogQuery = void (*)(void* arg1, void* arg2, void* keyPtr,
                                void* arg4);
// Factory REAL entry. RCX = handler object, RDX = spawn entity, R8D = catalog
// key, R9D = category. Per-frame "ensure" passes pass RDX = 0, so a non-null
// entity argument isolates the actual creation/ensure moment. Unlike the
// placement-record hook (RVA 0x679895), which only sees the subset of spawns
// that read a placement record, this entry is reached for every entity that is
// built or ensured - which is what the 一難 marking needs for full coverage.
using FnFactoryEntry = void (*)(void* handler, void* entity, std::uint32_t key,
                                std::uint32_t category);

// Register-capture bridge for the menu-enqueue hook (see HookStub.asm).
// (The original RDI/R13 menu-summon gate is superseded by the pre-load hook;
// the stub still works as a plain detour and is kept.)
extern "C" std::uint64_t g_hookRdi;
extern "C" std::uint64_t g_hookR13;
extern "C" std::uint64_t g_hookRsp;
extern "C" std::uint64_t g_hookRbp;
extern "C" std::uint64_t g_hookR8;
extern "C" std::uint64_t g_hookR9;
extern "C" void CreateSwapHookBody(void* arg1, void* entity, void* arg3,
                                   void* arg4);
extern "C" void CatalogQueryHookBody(void* arg1, void* arg2, void* keyPtr,
                                     void* arg4);
std::uint64_t g_hookRdi = 0;
std::uint64_t g_hookR13 = 0;
std::uint64_t g_hookRsp = 0;
std::uint64_t g_hookRbp = 0;
std::uint64_t g_hookR8 = 0;
std::uint64_t g_hookR9 = 0;
static FnMenuEnqueue g_entryOriginal = nullptr;
static FnCatalogQuery g_catalogOriginal = nullptr;
extern "C" void CaptureHookContext();
extern "C" void CaptureCatalogContext();

// Map source-level swap (see kMapPlacementKeyPattern). MapBossHookStub is the
// asm half: it saves the volatile registers, calls MapBossHookBody with the
// placement record, writes the returned key into R14D, replays the displaced
// instructions and resumes at g_mapResume. g_mapResume is written by the
// installer (hook site + 7) and read by the stub, so it must have C linkage.
extern "C" void MapBossHookStub();
extern "C" std::uint64_t g_mapResume = 0;

// Purple (一難) flag hook, see kMapPurpleFlagPattern. Same contract as the map
// hook: the asm stub names the register the engine already has the entity in,
// MapPurpleHookBody does the work, then the displaced instruction is replayed
// and control resumes at g_mapPurpleResume (hook site + kMapPurpleDisplaced).
extern "C" void MapPurpleHookStub();
extern "C" std::uint64_t g_mapPurpleResume = 0;
std::atomic_bool g_mapPurpleHooked{false};
safetyhook::InlineHook g_mapPurpleHook;

// Factory-entry 一難 diagnostics (used by MapPurpleFactoryEntryBody below).
// Kept small: one capped log line per target entity that passes through the
// creation hook, so the in-game result ("which of these came out purple") can
// be matched against the field snapshot offline.
std::atomic<std::uint64_t> g_fepLogged{0};
std::atomic<std::uint64_t> g_fepSeen{0};
// kFepLogMax lives in src/state.h.
// Counts successful 一難 marks so their log lines stay capped (see
// MapPurpleMark). Distinct from g_fepLogged, which counts factory-entry hits.
std::atomic<std::uint32_t> g_mapPurpleMarkLogs{0};

// Factory-REAL-entry body (full-coverage creation hook). Declared here so the
// installer, which is defined much further down, can name it. C linkage keeps
// the name stable for the hook lambda.
extern "C" void MapPurpleFactoryEntryBody(void* handler, void* entity,
                                         std::uint32_t key,
                                         std::uint32_t category);

std::atomic_bool g_mapBoss{true};   // source-level map swap (default on)
std::atomic_bool g_mapHooked{false};
std::atomic<std::uint32_t> g_mapRandomMode{1};  // 1 = stable per placement,
                                                // 2 = re-roll every spawn
extern std::atomic<std::uint32_t> g_mapRankMode;  // defined below (MapRank)
extern std::atomic<std::uint32_t> g_mapRankEvery;  // defined below (MapRank)
extern std::atomic<std::uint32_t> g_mapPurple;  // defined below (purple / 一难)
std::atomic<std::uint32_t> g_mapSources[kMaxListEntries];
std::atomic<std::size_t> g_mapSourceCount{0};
std::atomic<std::uint32_t> g_mapPool[kMaxListEntries];
std::atomic<std::size_t> g_mapPoolCount{0};
std::atomic<std::uint64_t> g_mapSwapCount{0};
// The hook writes the target key THROUGH to the placement record (see
// MapBossHookBody), so after the first swap the record no longer holds the
// source key. This tiny memo keeps the original key per record so
// MapRandomMode=2 can still re-roll on later instantiations. Keyed by the low
// 32 bits of the record address; a collision only costs one wrong re-roll.
// Container sizes (kMapMemoSlots / kAssetTraceSlots / kPairMapMax /
// kPairPatchSlots) and kFepLogMax now come from src/state.h as inline constexpr
// so every module shares one object each.

std::atomic<std::uint32_t> g_mapMemoRecord[kMapMemoSlots];
std::atomic<std::uint32_t> g_mapMemoSource[kMapMemoSlots];
std::atomic<std::uint64_t> g_mapSweepHits{0};
// Per-source-key pools: the ini line "MapPool_<SOURCEHEX>=<list>" gives that one
// enemy its own target list, so 朱盆 and 狱卒鬼 no longer have to share a pool
// (the user's 2026-09-20 request). Looked up by the ORIGINAL placement key.
MapKeyPool g_mapKeyPools[kMapPoolSlots];
std::atomic<std::size_t> g_mapKeyPoolCount{0};
// One-shot source-table randomisation (see MapRandomizeTableOnce). MapHook=0
// (the default) means the plugin never patches code at all: the placement table
// the engine built at startup is rewritten once, then the game runs untouched.
std::atomic_bool g_mapHookEnabled{false};
std::atomic_bool g_mapTableDone{false};
std::atomic<std::uint64_t> g_mapTableHits{0};
safetyhook::InlineHook g_mapHook;
static safetyhook::InlineHook g_entryHook;
static safetyhook::InlineHook g_catalogHook;
using FnAssemblyMain = void (*)(void* arg1, void* arg2);
using FnGetResIdByFileKtid = std::uint32_t (*)(void* assetIdManager,
                                               std::uint32_t ktid);


std::atomic<std::uint32_t> g_targetId{kDefaultTargetId};
// Variant flags the placement record must carry for the target enemy to come
// out as the powered-up (紫皮 / 一難) variant. VERIFIED 2026-09-21 against the
// engine's own placement: the native purple Gozuki's record (id=CC15) reads
// 0x011E3701, while every ordinary placement for the SAME enemy reads
// 0x011F3701 - the single difference being bit 0x10000.
//
// Cross-checked across every flag value this project has ever observed
// (11E3701 11F3701 1F3701 1E3701 13701 13601 1E3601 11E3601 1013601):
//   bit24 set AND bit16 clear  ->  purple-capable  (11E3701, 11E3601)
//   bit16 set                  ->  ordinary        (11F3701, 1F3701, 13701)
// Bit 24 distinguished the engine's own placements in earlier rounds too
// (only 10 of 306 records carry it).
std::atomic<std::uint32_t> g_targetFlags{0x011E3701u};
// Counts record-flag rewrites (ApplyTargetFlags); capped logging uses it.
std::atomic<std::uint32_t> g_mapPurpleFlagWrites{0};
// Empty list = swap every id except the blacklist.
std::atomic<std::uint32_t> g_sourceIds[kMaxListEntries];
std::atomic<std::size_t> g_sourceCount{0};
std::atomic_bool g_swapAll{true};
std::atomic<std::uint32_t> g_blacklist[kMaxListEntries];
std::atomic<std::size_t> g_blacklistCount{0};
std::atomic<std::uint64_t> g_swapCount{0};
std::atomic<std::uint64_t> g_seenCount{0};
std::atomic<std::uint64_t> g_assetSwapCount{0};
// Redirects that could not be honoured because the TARGET ktid is not in the
// current scene's asset table (GetResIdByFileKtid answers 0xFFFFFFFF there).
std::atomic<std::uint64_t> g_assetMissCount{0};
// AssetTrace accumulator (see kConfigKeyAssetTrace). 512 slots, multiplicative
// hash; a collision only blurs the histogram, never the call total.
std::atomic<std::uint32_t> g_assetTraceKey[kAssetTraceSlots];
std::atomic<std::uint32_t> g_assetTraceHits[kAssetTraceSlots];
std::atomic<std::uint64_t> g_assetTraceTotal{0};
std::atomic_bool g_assetTrace{false};
// FactoryDiag: installs the factory-entry hook for sampling only (see
// kConfigKeyFactoryDiag). Independent of MapPurple so a load can be sampled
// without enabling the marking behaviour.
std::atomic_bool g_factoryDiag{false};
std::atomic_bool g_factorySwap{false};  // r8 battle-data swap (实验性,默认关)
std::atomic_bool g_assetSwap{true};     // KTID 归档重定向(v3 主线,默认开)
std::atomic_bool g_createSwap{false};   // creation-moment identity swap (上游,默认关)
std::atomic_bool g_catalogSwap{true};   // catalog-query key rewrite (默认开)
std::atomic_bool g_rosterSwap{true};    // training-roster key injection (默认开)
std::atomic<std::uint32_t> g_createSources[kMaxListEntries];
std::atomic<std::size_t> g_createSourceCount{0};
std::atomic<std::uint32_t> g_createTarget{kDefaultTargetId};
std::atomic<std::uint64_t> g_createSwapCount{0};
// 1 = full identity swap (entity+handler+R8). 2 = entity-only: leave the
// key/descriptor path untouched (the target's descriptor may not be loaded
// in-session — a full swap aborts the spawn, observed 10:56) and patch only
// [entity+0x00]; whether the model track follows the entity id is exactly
// what mode 2 measures.
std::atomic<std::uint32_t> g_createMode{2};
// Sliding-window rate cap for createSwap (see hook body).
std::atomic_uint64_t g_createSwapTimes[8]{};
std::atomic<std::uint32_t> g_createSwapSlot{0};
std::atomic<std::uint32_t> g_createMaxPerMinute{4};
std::filesystem::path g_configPath;
FILETIME g_configMtime{};
// KTID swap map; hot-reloaded when the ini mtime changes. Readers (game
// threads) take a snapshot via atomic shared_ptr — no locks in the hook.
std::atomic<std::shared_ptr<const std::map<std::uint32_t, std::uint32_t>>>
    g_ktidSwapMap{};
// IdentitySwap state: sources whose spawns get their identity rewritten, the
// single target key, and the canonical key -> type-tag table.
std::atomic_bool g_identitySwap{false};
std::atomic<std::uint32_t> g_identityFrom[kMaxListEntries];
std::atomic<std::size_t> g_identityFromCount{0};
std::atomic<std::uint32_t> g_identityTo{0};
std::atomic<std::uint64_t> g_identitySwapCount{0};
std::atomic<std::shared_ptr<const std::map<std::uint32_t, std::uint32_t>>>
    g_identityTags{};
// PairSwap state (see PairScanShard).
std::atomic_bool g_pairSwap{false};
std::atomic<std::uint32_t> g_pairFromKey{0};
std::atomic<std::uint32_t> g_pairToKey{0};
// Optional tag whitelist: when non-empty, only these tags are matched (and they
// may lie outside the default 0x90000..0x9FFFF band). Empty = the default band.
std::atomic<std::uint32_t> g_pairTags[kMaxListEntries];
std::atomic<std::size_t> g_pairTagCount{0};
// Reversible patching (2026-09-19): a tag's key lives in ONE global record, so
// every patch is visible in every scene. Remembering the sites lets us restore
// the original key as soon as the swap is switched off, which is what makes the
// feature usable for "only in the training room" workflows.
std::atomic_bool g_pairRevert{true};
// PairMap: tag -> target key. One entry per summon button (a roster tag).
// Unlike the older PairFromKey/PairToKey form this does not need to know the
// enemy's CURRENT key: the tag identifies the button, so the same config works
// whatever the scene considers canonical. Example (training room):
//   PairMap=0x946AD=0xA263C,0x92EA6=0x782F6,0x93EC7=0x41DB6,0x93077=0xC76B4
std::atomic<std::uint32_t> g_pairMapTag[kPairMapMax];
std::atomic<std::uint32_t> g_pairMapKey[kPairMapMax];
std::atomic<std::size_t> g_pairMapCount{0};
// Bumped on every config load so background threads can tell that the mapping
// may have changed and therefore need to undo their previous patches first.
std::atomic<std::uint64_t> g_configGeneration{0};
// Patched sites with their ORIGINAL key values, so a revert is exact even with
// several different targets in play.
std::atomic<std::uintptr_t> g_pairPatchedAddr[kPairPatchSlots];
std::atomic<std::uint32_t> g_pairPatchedOrig[kPairPatchSlots];
std::atomic<std::size_t> g_pairPatchedCount{0};
std::atomic<std::uint64_t> g_pairSwapCount{0};

// Parses "0xSRC=0xDST,0xSRC2=0xDST2" into a map. Whitespace tolerant.
std::map<std::uint32_t, std::uint32_t> ParseKtidPairs(std::string_view text);

// Implemented in src/purple.cpp; the placement-record sweep and the
// instantiation hook below both call them.
void ApplyTargetFlags(std::uintptr_t record);
void MapPurpleOnInstantiate(void* entity, std::uint32_t key);
// Implemented in src/purple.cpp. MapForceEmpower: NOP the two-byte `jne` that
// sends an "already killed" placement down the plain branch (see
// kRevivePlainBranchPattern). Takes the pattern's address, returns true when the
// patch is in place.
bool ApplyRevivePlainBranchPatch(std::uintptr_t patternAddress);

// Implemented in src/maps.cpp; the config loader, the record sweep and the
// factory hook below all call into it.
std::uint32_t MaybeSwap(std::uint32_t id);
bool IsMapSource(std::uint32_t key);
std::uint32_t MapPickTarget(std::uint32_t sourceKey, std::uint32_t instanceId);
void ApplyMapRank(std::uintptr_t record);
void MapSweepWorker();
// Implemented in src/roster.cpp and src/pair.cpp; started as threads below.
void RosterLoop();
void PairLoop();
bool MapRecordFlagsLookLikePlacement(std::uint32_t flags);

bool IsListed(const std::atomic<std::uint32_t>* list, std::size_t count,
              std::uint32_t id) {
  for (std::size_t i = 0; i < count; ++i) {
    if (list[i].load(std::memory_order_acquire) == id) {
      return true;
    }
  }
  return false;
}

// Source-level map swap body. Called from MapBossHookStub with RCX = placement
// record; returns the key to use (EAX). It re-reads the record itself so the
// asm stub only has to name one register, and so the "no opinion" answer is
// always the key the game was about to load.
extern "C" std::uint32_t MapBossHookBody(void* record, void* entity) {
  const auto* fields = reinterpret_cast<const std::uint32_t*>(record);
  const std::uint32_t sourceKey = fields[1];  // +0x04 = enemy key
  // Purple-variant hunt: log the (record, entity) pair for every instantiation.
  // The record identifies WHICH placement spawned (the engine's own 0xA263C
  // placements come out purple, ours plain), the entity is the live object the
  // difference must live in — the placement record is already ruled out: a
  // byte-identical clone of the purple record still spawned a plain enemy.
  {
    static std::atomic<std::uint32_t> instLogs{0};
    const std::uint32_t index =
        instLogs.fetch_add(1, std::memory_order_relaxed);
    if (index < 2048) {
      _MESSAGE("%s: map inst #%u rec=%p ent=%p id=%X key=%X flags=%X",
               kPluginName, index, record, entity, fields[0], sourceKey,
               fields[2]);
    }
  }
  // Every exit below hands the game the key it will actually build the entity
  // from, so the purple (一难) flag is applied on the way out — see
  // MapPurpleOnInstantiate for why the entity, not the record, is the lever.
  auto finish = [&](std::uint32_t key) -> std::uint32_t {
    // A record that ALREADY holds the target key never reaches the swap code
    // below: IsMapSource(0xA263C) is false, and the `target == sourceKey` guard
    // would bail out too. Both of those early returns used to skip
    // ApplyTargetFlags as well, so an engine-authored placement of the target
    // enemy that carries the ORDINARY variant word (0x011F3701) kept it: the
    // entity came out plain on every single load, forever. Fix the record here,
    // for every path that is about to hand the game the target key.
    if (key != 0 && key == g_targetId.load(std::memory_order_acquire)) {
      ApplyTargetFlags(reinterpret_cast<std::uintptr_t>(record));
    }
    MapPurpleOnInstantiate(entity, key);
    return key;
  };
  if (sourceKey == 0 || !g_mapBoss.load(std::memory_order_acquire)) {
    return finish(sourceKey);
  }

  const auto blacklistCount = g_blacklistCount.load(std::memory_order_acquire);
  if (blacklistCount != 0 &&
      IsListed(g_blacklist, blacklistCount, sourceKey)) {
    return finish(sourceKey);
  }

  // The write-through below turns the record itself into the target after the
  // first call, so the ORIGINAL key has to be remembered here for the
  // re-roll mode (an evicted slot simply falls through unchanged).
  const std::uint32_t recordLow =
      static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(record));
  std::uint32_t originalKey = sourceKey;
  {
    const std::size_t base = (recordLow >> 4) % kMapMemoSlots;
    for (std::size_t probe = 0; probe < kMapMemoSlots; ++probe) {
      const std::size_t index = (base + probe) % kMapMemoSlots;
      const std::uint32_t slotRecord =
          g_mapMemoRecord[index].load(std::memory_order_acquire);
      if (slotRecord == recordLow) {
        originalKey = g_mapMemoSource[index].load(std::memory_order_acquire);
        break;
      }
      if (slotRecord == 0) {
        g_mapMemoSource[index].store(sourceKey, std::memory_order_release);
        g_mapMemoRecord[index].store(recordLow, std::memory_order_release);
        break;
      }
    }
  }

  if (!IsMapSource(originalKey)) {
    return finish(sourceKey);
  }

  const std::uint32_t target = MapPickTarget(originalKey, fields[0]);
  if (target == 0 || target == sourceKey) {
    return finish(sourceKey);
  }

  const auto index = g_mapSwapCount.fetch_add(1, std::memory_order_relaxed);
  if (index < 64) {
    _MESSAGE("%s: map source swap %X -> %X (record %p inst %X)", kPluginName,
             originalKey, target, record, fields[0]);
  }
  // Write THROUGH to the placement record, not just into R14. The CE A/B that
  // worked patched the record itself, so every downstream reader (descriptor,
  // params lookup, asset job, spawn registry) saw one consistent key. Rewriting
  // only the register left the record at the source key: the game then built
  // the entity with the target while its model/asset side still followed the
  // old key, and the enemy came up invisible/absent (observed in game
  // 2026-09-19 23:52 — the log showed 8 swaps and an empty spawn point).
  reinterpret_cast<volatile std::uint32_t*>(record)[1] = target;
  // Set the variant bits in the same breath as the key. Leaving them at the
  // source placement's value is what produced the "some purple, some plain"
  // mix from one MapPool.
  ApplyTargetFlags(reinterpret_cast<std::uintptr_t>(record));
  ApplyMapRank(reinterpret_cast<std::uintptr_t>(record));
  return finish(target);
}

void LogLoop() {
  try {
    while (true) {
      Sleep(10000);
      // Hot reload: re-read the ini whenever its mtime changes so KTID
      // swap pairs can be tuned without restarting the game.
      if (!g_configPath.empty()) {
        if (const HANDLE file = CreateFileW(
                g_configPath.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            file != INVALID_HANDLE_VALUE) {
          FILETIME mtime{};
          GetFileTime(file, nullptr, nullptr, &mtime);
          CloseHandle(file);
          if (CompareFileTime(&mtime, &g_configMtime) != 0) {
            g_configMtime = mtime;
            _MESSAGE("%s: config change detected, reloading", kPluginName);
            LoadConfig(nullptr);
          }
        }
      }
      const std::uint64_t seen =
          g_seenCount.exchange(0, std::memory_order_relaxed);
      const std::uint64_t swapped =
          g_swapCount.exchange(0, std::memory_order_relaxed);
      const std::uint64_t assetSwapped =
          g_assetSwapCount.exchange(0, std::memory_order_relaxed);
      const std::uint64_t createSwapped =
          g_createSwapCount.exchange(0, std::memory_order_relaxed);
      const std::uint64_t assetMissed =
          g_assetMissCount.exchange(0, std::memory_order_relaxed);
      if (seen != 0 || assetSwapped != 0 || createSwapped != 0 ||
          assetMissed != 0) {
        _MESSAGE("%s: last 10s: seen=%llu factorySwapped=%llu "
                 "assetRedirected=%llu assetMissed=%llu createSwapped=%llu "
                 "(ktidPairs=%zu)",
                 kPluginName, static_cast<unsigned long long>(seen),
                 static_cast<unsigned long long>(swapped),
                 static_cast<unsigned long long>(assetSwapped),
                 static_cast<unsigned long long>(assetMissed),
                 static_cast<unsigned long long>(createSwapped),
                 g_ktidSwapMap.load()->size());
      }
      if (g_assetTrace.load(std::memory_order_acquire)) {
        const std::uint64_t traceCalls =
            g_assetTraceTotal.exchange(0, std::memory_order_relaxed);
        if (traceCalls != 0) {
          struct TraceTop {
            std::uint32_t key;
            std::uint32_t hits;
          };
          TraceTop top[16]{};
          std::size_t distinct = 0;
          for (std::size_t i = 0; i < kAssetTraceSlots; ++i) {
            const std::uint32_t hits =
                g_assetTraceHits[i].exchange(0, std::memory_order_relaxed);
            if (hits == 0) {
              continue;
            }
            ++distinct;
            if (hits <= top[15].hits) {
              continue;
            }
            const TraceTop entry{
                g_assetTraceKey[i].load(std::memory_order_relaxed), hits};
            std::size_t position = 15;
            while (position > 0 && top[position - 1].hits < hits) {
              top[position] = top[position - 1];
              --position;
            }
            top[position] = entry;
          }
          std::string line = "assetTrace 10s: calls=" +
                             std::to_string(traceCalls) + " distinct=" +
                             std::to_string(distinct) + " top:";
          for (const auto& entry : top) {
            if (entry.hits == 0) {
              continue;
            }
            char item[32]{};
            std::snprintf(item, sizeof(item), " 0x%08X:%u", entry.key,
                          entry.hits);
            line += item;
          }
          _MESSAGE("%s: %s", kPluginName, line.c_str());
        }
      }
      if (g_discoverMode.load(std::memory_order_acquire)) {
        WriteDiscoverCensus();
      }
    }
  } catch (...) {
  }
}

std::atomic_bool g_shutdown{false};
std::atomic_bool g_factoryHooked{false};
std::atomic_bool g_factoryEntryHooked{false};
std::atomic_bool g_getResHooked{false};
std::atomic_bool g_createHooked{false};
std::atomic_bool g_createOrcheHooked{false};
std::atomic_bool g_catalogHooked{false};
std::atomic_bool g_identityHooked{false};
std::atomic<std::uint64_t> g_createDiagCount{0};

// Sliding-window rate cap shared by the factory/orchestrator hooks: the
// shrine mass-respawn funnels dozens of spawns per burst; swapping them all
// with an unloaded target aborts the storm and freezes the game. Training
// summons are 1-3/min — a small window passes them and blocks the storm.
// Returns false when the caller must pass through unpatched this minute.
bool CreateRateAllow() {
  const auto now = GetTickCount64();
  std::uint32_t usedLastMinute = 0;
  for (const auto& t : g_createSwapTimes) {
    if (now - t.load(std::memory_order_relaxed) < 60000) {
      ++usedLastMinute;
    }
  }
  if (usedLastMinute >=
      g_createMaxPerMinute.load(std::memory_order_acquire)) {
    return false;
  }
  g_createSwapTimes[g_createSwapSlot.fetch_add(1) %
                    std::size(g_createSwapTimes)]
      .store(now, std::memory_order_relaxed);
  return true;
}

// Evidence collector for the menu-summon hook: walks UP from this hook
// (return addresses near the entry RSP / the RBP chain) so the caller that
// baked the wrapper's asset manifest can be located WITHOUT the flaky CE
// bridge, and snapshots the manifest bytes the orchestrator is about to
// read (its `lea rsi,[rcx+0x1060]`). Pure observation: nothing is patched
// here. Rate-deduped: human summons are seconds apart, so one snapshot per
// summon is plenty and a stray caller flood cannot bloat the log.
void DumpSummonEvidence(std::uintptr_t wrapper, std::uint32_t key, void* arg2,
                        void* arg3, void* arg4) {
  static std::atomic<std::uint64_t> s_lastDumpTick{0};
  const auto now = GetTickCount64();
  if (now - s_lastDumpTick.load(std::memory_order_relaxed) < 2000) {
    return;
  }
  s_lastDumpTick.store(now, std::memory_order_relaxed);

  const auto exeBase =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  std::uintptr_t exeEnd = exeBase + 0x4000000;  // fallback window
  if (const auto size = HookUtils::GetModuleSize(
          reinterpret_cast<HMODULE>(exeBase))) {
    exeEnd = exeBase + *size;
  }

  _MESSAGE("%s: dump key=0x%05X args=%p %p %p wrapper=%p rsp=%p rbp=%p "
           "exe=%p..%p",
           kPluginName, key, reinterpret_cast<void*>(wrapper), arg2, arg3,
           arg4, reinterpret_cast<void*>(g_hookRsp),
           reinterpret_cast<void*>(g_hookRbp),
           reinterpret_cast<void*>(exeBase),
           reinterpret_cast<void*>(exeEnd));

  // Direct caller: [entry RSP] is its return address, always valid.
  __try {
    const auto ret0 =
        *reinterpret_cast<const std::uint64_t*>(g_hookRsp);
    if (ret0 >= exeBase + 0x1000 && ret0 < exeEnd) {
      _MESSAGE("%s:   direct caller exe+0x%07X",
               kPluginName, static_cast<std::uint32_t>(ret0 - exeBase));
    } else {
      _MESSAGE("%s:   direct caller %p (outside exe)",
               kPluginName, reinterpret_cast<void*>(ret0));
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  // Frame-chain walk: valid only while callers keep RBP frame pointers
  // (FPO frames silently break the chain; the scan below covers that).
  auto fp = g_hookRbp;
  __try {
    for (std::uint32_t i = 0; i < 8; ++i) {
      if (fp < 0x10000 || (fp & 7) != 0 || fp >= 0x7FFF00000000ULL) {
        break;
      }
      const auto ret = *reinterpret_cast<const std::uint64_t*>(fp + 8);
      const auto next = *reinterpret_cast<const std::uint64_t*>(fp);
      if (ret >= exeBase + 0x1000 && ret < exeEnd) {
        _MESSAGE("%s:   rbp[%u] exe+0x%07X", kPluginName, i,
                 static_cast<std::uint32_t>(ret - exeBase));
      }
      fp = next;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  // Raw scan of the caller's stack region: any exe-code pointer near the
  // entry RSP is a candidate return address, FPO or not. Offsets identify
  // each frame's position relative to this hook.
  std::uint32_t logged = 0;
  __try {
    for (std::uint32_t off = 0; off < 0x600 && logged < 24; off += 8) {
      const auto v =
          *reinterpret_cast<const std::uint64_t*>(g_hookRsp + off);
      if (v >= exeBase + 0x1000 && v < exeEnd) {
        _MESSAGE("%s:   scan[rsp+0x%03X] exe+0x%07X", kPluginName, off,
                 static_cast<std::uint32_t>(v - exeBase));
        ++logged;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  // Manifest snapshot: the region the orchestrator addresses via
  // [rcx+0x1060], plus the wrapper area around the entity-pointer slot.
  char hex[160];
  auto hexDump = [&](std::uintptr_t addr, std::uint32_t size) {
    hex[0] = '\0';
    __try {
      for (std::uint32_t i = 0; i < size; ++i) {
        std::sprintf(hex + i * 2, "%02X",
                     *reinterpret_cast<const std::uint8_t*>(addr + i));
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      hex[0] = '\0';
    }
  };
  hexDump(wrapper + 0xC0, 0x40);
  _MESSAGE("%s: wrapper  +0x00C0: %s", kPluginName, hex);
  for (std::uint32_t off = 0x1000; off < 0x1200; off += 0x40) {
    hexDump(wrapper + off, 0x40);
    _MESSAGE("%s: manifest +0x%04X: %s", kPluginName, off, hex);
  }
}




// Menu-load orchestrator hook body (reached via CaptureHookContext in
// HookStub.asm). Entry RVA 0x2E60C0; kick-bp stack capture puts this frame
// exclusively on menu-summon loads (shrine/world respawn loads enter via
// 0x3635F2/0x3DF407/0x3B2141 and never reach here), so no scene gate is
// needed beyond the source-key hit. arg1 (RCX) is the spawn-request wrapper:
// it holds a pointer to the raw entity somewhere in its first ~0x1200 bytes.
// The entity is identified by its embedded id ([entity]>>4 == key) plus its
// identity record ([entity+0xF8]->record, record[1] == key). Patching BOTH
// before this function enqueues the asset load makes the worker resolve the
// TARGET's catalog entry and files — the game loads the target by itself,
// and the downstream builder/factory read one consistent identity. This is
// the pre-load, zero-inconsistency swap point.
extern "C" void CreateSwapHookBody(void* arg1, void* entity, void* arg3,
                                   void* arg4) {
  const auto target = g_createTarget.load(std::memory_order_acquire);
  const auto a1 = reinterpret_cast<std::uintptr_t>(arg1);
  if (g_createSwap.load(std::memory_order_acquire) && target != 0 &&
      a1 >= 0x10000000000ULL && a1 <= 0x800000000000ULL) {
    __try {
      const auto sourceCount =
          g_createSourceCount.load(std::memory_order_acquire);
      for (std::uint32_t off = 0; off < 0x1200; off += 8) {
        const auto p = *reinterpret_cast<const std::uintptr_t*>(a1 + off);
        if (p < 0x10000000000ULL || p > 0x800000000000ULL) {
          continue;
        }
        const std::uint32_t emb = *reinterpret_cast<const std::uint32_t*>(p);
        const std::uint32_t key = emb >> 4;
        if (key == 0 || !IsListed(g_createSources, sourceCount, key)) {
          continue;
        }
        const auto rec = *reinterpret_cast<const std::uintptr_t*>(p + 0xF8);
        if (rec < 0x10000000000ULL || rec > 0x800000000000ULL ||
            *reinterpret_cast<const std::uint32_t*>(rec + 4) != key) {
          continue;
        }
        // Evidence first: who called us + the manifest bytes the
        // orchestrator will read. Dumped on EVERY matched summon (all three
        // training bosses), independent of the rate cap, so cross-boss
        // snapshots stay comparable. The cap only gates the patch below.
        DumpSummonEvidence(a1, key, entity, arg3, arg4);
        if (!CreateRateAllow()) {
          break;
        }
        *reinterpret_cast<std::uint32_t*>(p) = target << 4;  // embedded id
        *reinterpret_cast<std::uint32_t*>(rec + 4) = target;  // record key
        g_createSwapCount.fetch_add(1, std::memory_order_relaxed);
        _MESSAGE("%s: orchestrator swap 0x%05X -> 0x%05X "
                 "(wrapper=%p entity=%p rec=%p off=+%X)",
                 kPluginName, key, target, arg1,
                 reinterpret_cast<void*>(p),
                 reinterpret_cast<void*>(rec), off);
        break;  // one entity per menu summon
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  g_entryOriginal(arg1, entity, arg3, arg4);
}

// Catalog-query hook body (reached via CaptureCatalogContext in HookStub.asm).
// Entry RVA 0x4EEF44. arg3 (R8) points at the embedded id dword (key<<4) that
// the query reads via `mov edi,[r8]; shr edi,04`. The rewrite is TEMPORARY:
// 2026-09-18 终审 evidence — a PERSISTENT rewrite lands in the roster object
// and breaks every later summon (roster validation rejects 0xA263C and the
// spawn aborts silently, leaving the stuck key behind). Restoring the
// original key right after the query returns gives: assets + params resolved
// for the TARGET during the query window, while every downstream stage
// (roster check, placement, spawn finalize) still sees the source key.
extern "C" void CatalogQueryHookBody(void* arg1, void* arg2, void* keyPtr,
                                     void* arg4) {
  (void)arg1; (void)arg2; (void)arg4;
  const auto kp = reinterpret_cast<std::uintptr_t>(keyPtr);
  std::uint32_t savedEmb = 0;
  bool restore = false;
  if (g_catalogSwap.load(std::memory_order_acquire) &&
      kp >= 0x10000 && kp < 0x7FFF00000000ULL) {
    __try {
      const auto emb = *reinterpret_cast<const std::uint32_t*>(kp);
      const auto key = emb >> 4;
      const auto target = g_createTarget.load(std::memory_order_acquire);
      const auto sourceCount =
          g_createSourceCount.load(std::memory_order_acquire);
      if (key != 0 && target != 0 &&
          IsListed(g_createSources, sourceCount, key)) {
        *reinterpret_cast<std::uint32_t*>(kp) = target << 4;
        savedEmb = emb;
        restore = true;
        g_createSwapCount.fetch_add(1, std::memory_order_relaxed);
        _MESSAGE("%s: catalog query swap 0x%05X -> 0x%05X (ptr=%p, temp)",
                 kPluginName, key, target, keyPtr);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      restore = false;
    }
  }
  g_catalogOriginal(arg1, arg2, keyPtr, arg4);
  if (restore) {
    __try {
      *reinterpret_cast<std::uint32_t*>(kp) = savedEmb;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
}


// The packed exe decrypts .text lazily; a pattern can be unreadable at
// plugin-init time even though it is correct. Retry until the region is
// committed, instead of giving up at game boot. Each hook installs at most
// once — a pattern miss retries only the missing piece (double inline hooks
// on one address corrupt each other and can leave the trampoline null).
void InstallHooksWithRetry() {
  try {
    for (int attempt = 0; attempt < 600 && !g_shutdown.load(); ++attempt) {
      bool complete = true;

      if (!g_factoryHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t factoryCore =
            HookUtils::ScanIDAPattern(kFactoryCorePattern);
        if (factoryCore == 0) {
          complete = false;
        } else {
          HookLambda(reinterpret_cast<FnFactoryCore>(factoryCore),
                     [](void* arg1, void* arg2, std::uint32_t spawnId,
                        void* paramObject) {
                       const std::uint32_t swappedId =
                           g_factorySwap.load(std::memory_order_acquire)
                               ? MaybeSwap(spawnId)
                               : spawnId;
                       original(arg1, arg2, swappedId, paramObject);
                     });
          g_factoryHooked.store(true, std::memory_order_release);
          _MESSAGE("%s: factory core hook installed at %p", kPluginName,
                   reinterpret_cast<void*>(factoryCore));
        }
      }

      // Factory REAL entry (kFactoryEntryPattern). The full-coverage creation
      // hook: unlike RVA 0x679895 it is reached for every entity that is built
      // or ensured, and RDX is the entity at that moment. Installed with
      // MapPurple (it is what finally makes the 一難 marking cover everything
      // the placement-record hook missed) and also with FactoryDiag so a plain
      // spawn can be sampled without changing any behaviour.
      if ((g_mapPurple.load(std::memory_order_acquire) ||
           g_factoryDiag.load(std::memory_order_acquire)) &&
          !g_factoryEntryHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t factoryEntry =
            HookUtils::ScanIDAPattern(kFactoryEntryPattern);
        if (factoryEntry == 0) {
          complete = false;
          _MESSAGE("%s: factory entry pattern not found (will retry)",
                   kPluginName);
        } else {
          HookLambda(reinterpret_cast<FnFactoryEntry>(factoryEntry),
                     [](void* handler, void* entity, std::uint32_t key,
                        std::uint32_t category) {
                       MapPurpleFactoryEntryBody(handler, entity, key, category);
                       original(handler, entity, key, category);
                     });
          g_factoryEntryHooked.store(true, std::memory_order_release);
          _MESSAGE("%s: factory entry hook installed at %p (full-coverage "
                   "一難 marking)",
                   kPluginName, reinterpret_cast<void*>(factoryEntry));
        }
      }

      if (g_createSwap.load(std::memory_order_acquire) &&
          !g_createHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t menuEnqueue =
            HookUtils::ScanIDAPattern(kMenuEnqueuePattern);
        if (menuEnqueue == 0) {
          complete = false;
        } else {
          // Pre-load identity swap at the menu-summon enqueue entry (see
          // kMenuEnqueuePattern). Wired through CaptureHookContext
          // (HookStub.asm). Shrine/world respawns never call this function,
          // so the mass-respawn freeze path is structurally unreachable.
          auto hook = safetyhook::create_inline(
              reinterpret_cast<void*>(menuEnqueue),
              reinterpret_cast<void*>(&CaptureHookContext));
          if (hook) {
            g_entryOriginal =
                reinterpret_cast<FnMenuEnqueue>(hook.trampoline().address());
            g_entryHook = std::move(hook);
            g_createHooked.store(true, std::memory_order_release);
            _MESSAGE("%s: menu enqueue hook installed at %p (pre-load swap)",
                     kPluginName, reinterpret_cast<void*>(menuEnqueue));
          } else {
            complete = false;
          }
        }
      }

      if (g_catalogSwap.load(std::memory_order_acquire) &&
          !g_catalogHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t catalogQuery =
            HookUtils::ScanIDAPattern(kCatalogQueryPattern);
        if (catalogQuery == 0) {
          complete = false;
        } else {
          auto hook = safetyhook::create_inline(
              reinterpret_cast<void*>(catalogQuery),
              reinterpret_cast<void*>(&CaptureCatalogContext));
          if (hook) {
            g_catalogOriginal =
                reinterpret_cast<FnCatalogQuery>(hook.trampoline().address());
            g_catalogHook = std::move(hook);
            g_catalogHooked.store(true, std::memory_order_release);
            _MESSAGE("%s: catalog query hook installed at %p (key swap)",
                     kPluginName, reinterpret_cast<void*>(catalogQuery));
          } else {
            complete = false;
          }
        }
      }

      // MapHook=0 (default): the placement table is randomised once by
      // MapSweepWorker and NO code is patched, so a pattern miss here is not a
      // failure and must not keep the installer spinning.
      if (g_mapHookEnabled.load(std::memory_order_acquire) &&
          g_mapBoss.load(std::memory_order_acquire) &&
          !g_mapHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t mapKeyRead =
            HookUtils::ScanIDAPattern(kMapPlacementKeyPattern);
        if (mapKeyRead == 0) {
          complete = false;
          _MESSAGE("%s: map placement pattern not found (will retry)",
                   kPluginName);
        } else {
          // Displaced bytes: mov r14d,[rax+04] (4) + test rbx,rbx (3).
          g_mapResume = mapKeyRead + 7;
          auto hook = safetyhook::create_inline(
              reinterpret_cast<void*>(mapKeyRead),
              reinterpret_cast<void*>(&MapBossHookStub));
          if (hook) {
            g_mapHook = std::move(hook);
            g_mapHooked.store(true, std::memory_order_release);
            _MESSAGE("%s: map placement hook installed at %p (source-level "
                     "swap, resume %p)",
                     kPluginName, reinterpret_cast<void*>(mapKeyRead),
                     reinterpret_cast<void*>(g_mapResume));
          } else {
            complete = false;
          }
        }
      }

      // MapPurple=1: hook the per-frame 一難 flag writer so EVERY entity is
      // covered, whichever path spawned it. Independent of MapHook — that one
      // decides the enemy key, this one only sets the purple bit.
      if (g_mapPurple.load(std::memory_order_acquire) &&
          !g_mapPurpleHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t flagWrite =
            HookUtils::ScanIDAPattern(kMapPurpleFlagPattern);
        if (flagWrite == 0) {
          complete = false;
          _MESSAGE("%s: purple flag pattern not found (will retry)",
                   kPluginName);
        } else {
          const std::uintptr_t site = flagWrite + kMapPurpleHookOffset;
          g_mapPurpleResume = site + kMapPurpleDisplaced;
          auto hook = safetyhook::create_inline(
              reinterpret_cast<void*>(site),
              reinterpret_cast<void*>(&MapPurpleHookStub));
          if (hook) {
            g_mapPurpleHook = std::move(hook);
            g_mapPurpleHooked.store(true, std::memory_order_release);
            _MESSAGE("%s: purple flag hook installed at %p (resume %p)",
                     kPluginName, reinterpret_cast<void*>(site),
                     reinterpret_cast<void*>(g_mapPurpleResume));
          } else {
            complete = false;
          }
        }
      }

      // MapForceEmpower=1. This is the decision that actually picks plain vs
      // purple, and the engine makes it per spawn from the record's instanceId
      // alone - see kRevivePlainBranchPattern for the CE-verified mechanism. It
      // is a two-byte code patch rather than a hook (both outcomes live inside
      // one function), so it is applied once, after the pattern resolves, and
      // retried until the bytes are validated and written.
      if (g_mapForceEmpower.load(std::memory_order_acquire) &&
          !g_reviveBranchPatched.load(std::memory_order_acquire)) {
        const std::uintptr_t branch =
            HookUtils::ScanIDAPattern(kRevivePlainBranchPattern);
        if (branch == 0) {
          complete = false;
          _MESSAGE("%s: revive/plain branch pattern not found (will retry)",
                   kPluginName);
        } else if (!ApplyRevivePlainBranchPatch(branch)) {
          _MESSAGE("%s: revive/plain branch NOT patched (see message above)",
                   kPluginName);
        }
      }

      if (g_assetSwap.load(std::memory_order_acquire) &&
          !g_getResHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t getResId = HookUtils::ScanIDAPattern(
            kGetResIdByFileKtidPattern, 0, 1, 5);
        if (getResId == 0) {
          complete = false;
          _MESSAGE("%s: GetResIdByFileKtid pattern not found (will retry)",
                   kPluginName);
        } else {
          HookLambda(reinterpret_cast<FnGetResIdByFileKtid>(getResId),
                     [](void* assetIdManager,
                        std::uint32_t ktid) -> std::uint32_t {
                       // 0xFFFFFFFF is this function's "not found" answer —
                       // verified in its miss path (CE 2026-09-19, RVA
                       // 0x609F74: `or eax,-1` on both failure exits; the hit
                       // path computes an index via the sorted-ktid binary
                       // search). A ktid the CURRENT SCENE never registered
                       // answers 0xFFFFFFFF, and handing that to the loader
                       // breaks the asset, so fall back to the original id.
                       constexpr std::uint32_t kInvalidResId = 0xFFFFFFFFu;
                       if (g_assetTrace.load(std::memory_order_relaxed)) {
                         g_assetTraceTotal.fetch_add(1,
                                                     std::memory_order_relaxed);
                         const auto slot = (ktid * 2654435761u) >> 23;
                         if (g_assetTraceHits[slot].fetch_add(
                                 1, std::memory_order_relaxed) == 0) {
                           g_assetTraceKey[slot].store(
                               ktid, std::memory_order_relaxed);
                         }
                       }
                       const auto map =
                           g_ktidSwapMap.load(std::memory_order_acquire);
                       if (map && !map->empty()) {
                         if (const auto it = map->find(ktid);
                             it != map->end()) {
                           const std::uint32_t target = it->second;
                           const std::uint32_t redirected =
                               original(assetIdManager, target);
                           if (redirected != kInvalidResId) {
                             const auto n = g_assetSwapCount.fetch_add(
                                 1, std::memory_order_relaxed);
                             if (n < 64) {
                               _MESSAGE("%s: asset swap 0x%08X -> 0x%08X "
                                        "(resId 0x%08X)",
                                        kPluginName, ktid, target, redirected);
                             }
                             return redirected;
                           }
                           const auto m = g_assetMissCount.fetch_add(
                               1, std::memory_order_relaxed);
                           if (m < 16) {
                             _MESSAGE("%s: asset swap MISS 0x%08X -> 0x%08X "
                                      "(target unregistered in this scene; "
                                      "keeping the original)",
                                      kPluginName, ktid, target);
                           }
                         }
                       }
                       return original(assetIdManager, ktid);
                     });
          g_getResHooked.store(true, std::memory_order_release);
          _MESSAGE("%s: GetResIdByFileKtid hook installed at %p", kPluginName,
                   reinterpret_cast<void*>(getResId));
        }
      }

      if (g_identitySwap.load(std::memory_order_acquire) &&
          !g_identityHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t initSlot =
            HookUtils::ScanIDAPattern(kIdentityInitPattern);
        if (initSlot == 0) {
          complete = false;
          _MESSAGE("%s: identity-init pattern not found (will retry)",
                   kPluginName);
        } else {
          // The return value MUST be forwarded: CreateSlot hands InitSlot's
          // RAX (the slot pointer) to its own caller with `mov rdx,rax`.
          using FnIdentityInit = void* (*)(void* slot, std::uint64_t identity,
                                           std::uint8_t flag);
          HookLambda(
              reinterpret_cast<FnIdentityInit>(initSlot),
              [](void* slot, std::uint64_t identity,
                 std::uint8_t flag) -> void* {
                std::uint64_t out = identity;
                const auto key = static_cast<std::uint32_t>(identity) >> 4;
                const auto target =
                    g_identityTo.load(std::memory_order_acquire);
                const auto fromCount =
                    g_identityFromCount.load(std::memory_order_acquire);
                if (key != 0 && target != 0 && key != target &&
                    IsListed(g_identityFrom, fromCount, key)) {
                  const auto tags =
                      g_identityTags.load(std::memory_order_acquire);
                  if (tags != nullptr) {
                    const auto it = tags->find(target);
                    if (it != tags->end()) {
                      out = (static_cast<std::uint64_t>(it->second) << 32) |
                            (static_cast<std::uint64_t>(target) << 4);
                      const auto n = g_identitySwapCount.fetch_add(
                          1, std::memory_order_relaxed);
                      if (n < 64) {
                        _MESSAGE("%s: identity swap 0x%05X -> 0x%05X "
                                 "(tag 0x%05X) slot=%p",
                                 kPluginName, key, target, it->second, slot);
                      }
                    }
                  }
                }
                return original(slot, out, flag);
              });
          g_identityHooked.store(true, std::memory_order_release);
          _MESSAGE("%s: identity-init hook installed at %p", kPluginName,
                   reinterpret_cast<void*>(initSlot));
        }
      }

      if (complete) {
        _MESSAGE("%s: all hooks online (attempt %d)", kPluginName,
                 attempt + 1);
        return;
      }
      Sleep(2000);
    }
    _MESSAGE("%s: hook installation gave up", kPluginName);
  } catch (const std::exception& e) {
    _MESSAGE("%s: installer exception: %s", kPluginName, e.what());
  } catch (...) {
    _MESSAGE("%s: installer unknown exception", kPluginName);
  }
}


extern "C" __declspec(dllexport) bool nioh3_plugin_initialize(
    const Nioh3PluginInitializeParam* param) {
  try {
    _MESSAGE("%s: init start", kPluginName);
    LoadConfig(param);
    try {
      std::thread(MapSweepWorker).detach();
      std::thread(InstallHooksWithRetry).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: installer thread creation failed: %s", kPluginName,
               e.what());
    }
    try {
      std::thread(LogLoop).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: log thread creation failed: %s", kPluginName, e.what());
    }
    try {
      std::thread(RosterLoop).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: roster thread creation failed: %s", kPluginName, e.what());
    }
    try {
      std::thread(PairLoop).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: pair thread creation failed: %s", kPluginName, e.what());
    }
    _MESSAGE("%s: initialized (hooks pending)", kPluginName);
    return true;
  } catch (const std::exception& e) {
    _MESSAGE("%s: init exception: %s", kPluginName, e.what());
    return false;
  } catch (...) {
    _MESSAGE("%s: init unknown exception", kPluginName);
    return false;
  }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    _MESSAGE("Initializing plugin: %s", kPluginName);
  } else if (reason == DLL_PROCESS_DETACH) {
    g_shutdown.store(true, std::memory_order_release);
  }
  return TRUE;
}
