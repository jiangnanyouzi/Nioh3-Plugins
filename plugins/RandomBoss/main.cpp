#define NOMINMAX

// Shared declarations and engine signatures live in their own headers.
#include "core.h"
#include "patterns.h"
#include "state.h"   // runtime state owned by this file / the split-out modules
#include "config.h"  // LoadConfig + the ini-derived knobs it publishes

// NOTE: these definitions used to sit in a single anonymous namespace wrapping
// the whole file. That namespace has been opened so the definitions have
// external linkage and the split-out modules (src/*.cpp) can reference them.

#include "src/patterns.h"

// The remaining ini fallback value (kDefaultBlacklist) lives in src/core.h, and
// the ini reader that consumes it is src/config.cpp. The map SOURCE / TARGET
// lists are NOT here: they are config-only (RandomBoss.ini -> MapSources /
// MapPool) since 2026-09-21, and MapPool is also the only definition of which
// enemies count as ours - there is no separate target id any more.

// Factory REAL entry. RCX = handler object, RDX = spawn entity, R8D = catalog
// key, R9D = category. Per-frame "ensure" passes pass RDX = 0, so a non-null
// entity argument isolates the actual creation/ensure moment. Unlike the
// placement-record hook (RVA 0x679895), which only sees the subset of spawns
// that read a placement record, this entry is reached for every entity that is
// built or ensured - which is what the 一難 marking needs for full coverage.
using FnFactoryEntry = void (*)(void* handler, void* entity, std::uint32_t key,
                                std::uint32_t category);

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
// Container sizes (kMapMemoSlots) and kFepLogMax now come from src/state.h as
// inline constexpr so every module shares one object each.

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

// Variant flags the placement record carries. This word's bit24 is NOT the
// purple switch - it is the engine's "ichi-nan / one-time placement" flag, and
// setting it is what stopped swapped enemies from ever coming back.
//
// MEASURED 2026-09-22 on a live map (pid 3352, base 0x7FF70CF10000), all 38
// placement objects, zero exceptions:
//
//   rec+0x08 bit24 == 1   <=>   placement +0x0C0 == 0   (engine builds a SHELL:
//                               container + actor exist, but no components)
//   rec+0x08 bit24 == 0   <=>   placement +0x0C0 != 0   (normal, populated)
//
// Every key-0xA263C (Gozuki) record read 0x011E3701 - bit24 set - and every one
// of them was a shell. Every other enemy's record read 0x001E3701 / 0x001F3701
// - bit24 clear - and every one of them had components.
//
// That is the mechanism behind the long-standing report "an ordinary Gozuki
// respawns after being killed, a purple one never does": the engine rebuilds a
// placement on the next load, but a bit24 (ichi-nan) placement is rebuilt as an
// empty shell and never re-enters the world. The old value here forced bit24 on
// every record the plugin touched, so the plugin itself was creating one-time
// placements - and no amount of "+0x8D4 / +0xAE14 / +0x8A4" patching downstream
// could undo that, because those fields are consequences, not causes.
//
// Verified in game the same day: with bit24 cleared on the live records, a
// Gozuki placement rebuilt WITH components, appeared - and still came out
// purple, because the visible purple is MapPurpleMark's runtime write to
// entity+0xE9, not this record bit. So the two properties are independent:
// bit24 = "one-time", entity+0xE9 = "looks powered-up".
//
// 0x001E3701 = ordinary, respawnable, purple via MapPurpleMark.  <<< CURRENT
// 0x011E3701 = engine's own one-time ichi-nan placement (previous value).
std::atomic<std::uint32_t> g_targetFlags{0x001E3701u};
// Counts record-flag rewrites (ApplyTargetFlags); capped logging uses it.
std::atomic<std::uint32_t> g_mapPurpleFlagWrites{0};
std::atomic<std::uint32_t> g_blacklist[kMaxListEntries];
std::atomic<std::size_t> g_blacklistCount{0};
// FactoryDiag: installs the factory-entry hook for sampling only (see
// kConfigKeyFactoryDiag). Independent of MapPurple so a load can be sampled
// without enabling the marking behaviour.
std::atomic_bool g_factoryDiag{false};
std::filesystem::path g_configPath;
FILETIME g_configMtime{};

// Implemented in src/purple.cpp; the placement-record sweep and the
// instantiation hook below both call them.
void ApplyTargetFlags(std::uintptr_t record);
void MapPurpleOnInstantiate(void* entity, std::uint32_t key);
// Implemented in src/purple.cpp. True when a key is one of the enemies this
// plugin can have placed (a member of MapPool or of any MapPool_<SRC> pool).
// This is the only definition of "ours": purple marking is gated on it, so a
// multi-target MapPool marks every one of its targets.
bool IsMapTargetKey(std::uint32_t key);
// Implemented in src/purple.cpp. MapForceEmpower: NOP the two-byte `jne` that
// sends an "already killed" placement down the plain branch (see
// kRevivePlainBranchPattern). Takes the pattern's address, returns true when the
// patch is in place.
bool ApplyRevivePlainBranchPatch(std::uintptr_t patternAddress);
// Same shape, for kBlockedPlacementPattern (MapIgnoreBlocked): NOPs the six-byte
// `jnl` that keeps a "placement+0x8D4 == 3" placement disabled. Returns true when
// the patch is in place.
bool ApplyBlockedPlacementPatch(std::uintptr_t patternAddress);

// Implemented in src/maps.cpp; the map hook and the record sweep below call
// into them.
bool IsMapSource(std::uint32_t key);
std::uint32_t MapPickTarget(std::uint32_t sourceKey, std::uint32_t instanceId);
void ApplyMapRank(std::uintptr_t record);
void MapSweepWorker();
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
    // A record that ALREADY holds one of our target keys never reaches the swap
    // code below: IsMapSource is false for a target, and the `target ==
    // sourceKey` guard would bail out too. Both of those early returns used to
    // skip ApplyTargetFlags as well, so an engine-authored placement of a target
    // enemy that carries the ORDINARY variant word (0x011F3701) kept it: the
    // entity came out plain on every single load, forever. Fix the record here,
    // for every path that is about to hand the game one of our keys - membership
    // rather than equality, so every entry of a multi-target MapPool is covered.
    if (key != 0 && IsMapTargetKey(key)) {
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
      // Hot reload: re-read the ini whenever its mtime changes so the map and
      // purple keys can be tuned without restarting the game.
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
    }
  } catch (...) {
  }
}

std::atomic_bool g_shutdown{false};
std::atomic_bool g_factoryEntryHooked{false};

// The packed exe decrypts .text lazily; a pattern can be unreadable at
// plugin-init time even though it is correct. Retry until the region is
// committed, instead of giving up at game boot. Each hook installs at most
// once — a pattern miss retries only the missing piece (double inline hooks
// on one address corrupt each other and can leave the trampoline null).
void InstallHooksWithRetry() {
  try {
    for (int attempt = 0; attempt < 600 && !g_shutdown.load(); ++attempt) {
      bool complete = true;

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

      // MapIgnoreBlocked=1. Separate gate from the one above: this is the one
      // that decides whether a placement produces a live enemy at all. See
      // kBlockedPlacementPattern. Has to be in place before a load, because the
      // placement's component sub-objects are only built by the load-time path.
      if (g_mapIgnoreBlocked.load(std::memory_order_acquire) &&
          !g_blockedPlacementPatched.load(std::memory_order_acquire)) {
        const std::uintptr_t blocked =
            HookUtils::ScanIDAPattern(kBlockedPlacementPattern);
        if (blocked == 0) {
          complete = false;
          _MESSAGE("%s: blocked-placement pattern not found (will retry)",
                   kPluginName);
        } else if (!ApplyBlockedPlacementPatch(blocked)) {
          _MESSAGE("%s: blocked-placement jump NOT patched (see message above)",
                   kPluginName);
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
