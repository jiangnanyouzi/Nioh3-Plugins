// maps.cpp - placement-table swapping for RandomBoss.
//
// Split out of main.cpp. Covers the whole source-level map swap: picking a
// target per placement, rewriting the placement records (key + variant flags +
// optional rank), and the one-time sweep that walks committed writable memory
// to find the placement table in the first place.
//
// The sweep is the only part that walks memory, and it runs ONCE at startup
// (see MapSweepWorker). Everything else is per-placement or per-record.
#include "core.h"
#include "state.h"
#include "patterns.h"

// Shared helper defined in main.cpp: linear search over an atomic id list.
// It is used by several modules, so it was left where it was rather than moved
// into one of them.
// Implemented in src/purple.cpp: writes the variant bits alongside the key.
void ApplyTargetFlags(std::uintptr_t record);

bool IsListed(const std::atomic<std::uint32_t>* list, std::size_t count,
              std::uint32_t id);

// Implemented in src/purple.cpp. True when a key belongs to MapPool or to one of
// the MapPool_<SRC> pools. The sweep's "this record already holds one of ours"
// test uses it, so every entry of a multi-target pool is treated as ours and an
// engine-authored placement of any of them gets its variant word fixed too.
bool IsMapTargetKey(std::uint32_t key);

// ---------------------------------------------------------------------------
// Map placement-record sweep.
//
// The read hook can only fire when a record is already being USED, which is too
// late for that first instantiation: the model/asset decision for the placement
// was taken earlier, so the enemy comes up empty until the placement re-streams.
// (User-verified 2026-09-19: first entry after a restart was empty, leaving and
// re-entering the map spawned the target. The CE A/B worked because the records
// were rewritten BEFORE they were used.)
//
// This worker does the same thing automatically: it walks committed writable
// private memory looking for the 12-byte placement-record signature
//     { +0x00 instanceId != 0, +0x04 key in MapSources, +0x08 flags&0xFFFF == 0x3701 }
// and writes the chosen target into +0x04. Activation is distance based, so the
// records are rewritten seconds before the player can get close to them.
// ---------------------------------------------------------------------------
extern std::atomic_bool g_shutdown;  // defined in main.cpp

// Placement records carry a flags word whose low byte is 0x01 and whose second
// byte is a small class value (observed 0x3701 on Jailer Oni records and 0x3601
// on others). Requiring exactly 0x3701 would silently skip every other enemy —
// e.g. Shunobon (朱盆) — so the filter is a range now. It still matters: the
// 0x398 definition records also embed a source key (+0x64) and matching by key
// alone would have written the target into them.
constexpr bool MapRecordFlagsLookLikePlacement(std::uint32_t flags) {
  const std::uint32_t low = flags & 0xFFu;
  const std::uint32_t kind = (flags >> 8) & 0xFFu;
  if (low != 0x01u) {
    return false;
  }
  switch (kind) {
    case 0x36u:
    case 0x37u:
    case 0x3Bu:
      return flags < 0x01200000u;  // the verified placement-flag family
    default:
      return false;
  }
}

// A key is a source when it is listed in MapSources OR when the ini gave it a
// per-key pool (MapPool_<SRC>) — defining a pool for an enemy is itself the
// statement "randomise this one".
bool IsMapSource(std::uint32_t key) {
  const auto slots = g_mapKeyPoolCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < slots; ++i) {
    if (g_mapKeyPools[i].source.load(std::memory_order_acquire) == key) {
      return true;
    }
  }
  const auto sourceCount = g_mapSourceCount.load(std::memory_order_acquire);
  return sourceCount != 0 && IsListed(g_mapSources, sourceCount, key);
}

// Picks the replacement key for one placement. sourceKey first selects the
// per-key pool (MapPool_<SRC>), so different enemies can randomise into
// different sets; a source with no own pool falls back to the global MapPool.
// An explicit empty per-key pool means "leave this enemy exactly as it is", and
// so does an empty global MapPool when the source has no per-key pool either
// (there is no built-in target list any more - see core.h - so the answer is
// simply "no swap").
std::uint32_t MapPickTarget(std::uint32_t sourceKey, std::uint32_t instanceId) {
  const std::atomic<std::uint32_t>* pool = g_mapPool;
  std::size_t poolCount = g_mapPoolCount.load(std::memory_order_acquire);
  const auto slots = g_mapKeyPoolCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < slots; ++i) {
    if (g_mapKeyPools[i].source.load(std::memory_order_acquire) == sourceKey) {
      const std::size_t count =
          g_mapKeyPools[i].count.load(std::memory_order_acquire);
      if (count == 0) {
        return sourceKey;
      }
      pool = g_mapKeyPools[i].keys;
      poolCount = count;
      break;
    }
  }
  if (poolCount == 0) {
    return 0;
  }
  const std::uint32_t mode = g_mapRandomMode.load(std::memory_order_acquire);
  std::uint32_t pick = 0;
  if (mode == 2) {
    // Re-roll on every use (a boss changes each time it streams in).
    pick = static_cast<std::uint32_t>(
        g_mapSwapCount.fetch_add(1, std::memory_order_relaxed));
  } else {
    // Stable per placement. The source key is mixed in as well, otherwise two
    // different enemies that share a pool would always land on the same target.
    pick = ((instanceId ^ (sourceKey * 2654435761u)) * 2654435761u) >> 16;
  }
  return pool[pick % poolCount].load(std::memory_order_acquire);
}

// +0x58 = the per-placement strength/variant scalar, and the ONLY field that
// separates the two populations the CE A/B had told apart by eye. A differential
// over the whole placement table (2026-09-20, 10 samples from each side) found
// every other offset in 0x0C..0x134 identical: just 500.0f vs 1000.0f, 10/10 on
// each side. Two earlier candidates are now falsified in game: the flags word
// (MapRank=3, plus a live rewrite of all 309 source records to 0x011F3701 that
// changed nothing on screen) and +0x5C — that was simply the wrong offset, and
// writing it is what made an enemy come up EMPTY.
// 0 = follow the source placement (default), 1 = force 500.0f (POWERED, i.e.
// the purple variant), 2 = force 1000.0f (ORDINARY/plain).
// POLARITY CORRECTED 2026-09-21: the labels here used to be the other way
// round, which is why the 2026-09-20 experiment that forced 1000.0f and saw
// only plain enemies was read as "this field is not the switch". It is the
// switch - 1000.0f IS the plain value. Pinned on a key the plugin never
// touches: live entities D06B (key 0x82783, 500.0f, entity+0xE9 bit0 set) and
// D39D (same key, 1000.0f, bit clear).
std::atomic<std::uint32_t> g_mapRankMode{0};
// MapRankEvery: apply the forced rank to only every Nth randomised placement.
// 1 (default) = all of them; 2 = alternate records keep their original +0x58, so
// one game launch shows the forced and the untouched variant side by side — the
// A/B that decided this field in the first place, but without a second restart.
std::atomic<std::uint32_t> g_mapRankEvery{1};
std::atomic<std::uint32_t> g_mapRankStride{0};

void ApplyMapRank(std::uintptr_t record) {
  const std::uint32_t mode = g_mapRankMode.load(std::memory_order_acquire);
  if (mode == 0) {
    return;
  }
  const float desired = (mode >= 2) ? 1000.0f : 500.0f;
  auto* rank = reinterpret_cast<volatile float*>(record + 0x58);
  const float before = *rank;
  if (before == desired) {
    return;
  }
  *rank = desired;
  // One capped line per REAL change. This write is the fix for the Gozuki that
  // never went purple, so it has to be visible in the log and not only on
  // screen: MapLogRecord prints f58 as it was found (before any write), so
  // without this line a successful fix and a no-op look identical in the log.
  static std::atomic<std::uint32_t> logged{0};
  const std::uint32_t n = logged.fetch_add(1, std::memory_order_relaxed);
  if (n < 48) {
    const auto* d = reinterpret_cast<const std::uint32_t*>(record);
    _MESSAGE("%s: map rank rec=%p id=%X key=%X f58 %g -> %g",
             kPluginName, reinterpret_cast<void*>(record), d[0], d[1],
             static_cast<double>(before), static_cast<double>(desired));
  }
}


// ---------------------------------------------------------------------------
// Purple variant ("一难" / ichi-nan), SOLVED 2026-09-20 with CE + the live game.
//
// The switch is bit 0x100 of the dword at entity+0xE8. The engine's own
// powered-up placement reads 0x101 where every ordinary enemy built from the
// SAME key reads 0x1 — 20/20 entities on a live map, nothing else in the first
// 0x800 bytes separates them except the placement id — and writing 0x101 into
// 19 live plain entities turned one of them purple on screen.
//
// The placement RECORD is not the source and is now fully excluded: +0x58
// (MapRank), +0x80 (the only field that differed across the whole population)
// and a byte-identical clone of the purple record were each written in game and
// changed nothing. The table is also built once at process start and never
// rebuilt (a data breakpoint on a record's key never fired again across area
// loads), so difficulty cannot be rewriting it either. That leaves instantiation
// itself — which is exactly where MapBossHookBody runs; HookStub.asm passes the
// entity in RDX (= RDI, the object whose [+0x20] selected the record).
//
// The bit is OR-ed, never assigned. Coverage does NOT come from the placement
// hook's callers: that hook misses most spawn paths (a measured map had 67 live
// target entities and the hook had seen 8 of them). It comes from
// MapPurpleHookBody, on the per-frame flag writer every entity passes through.



// Purple-variant hunt (2026-09-20). In game the user sees exactly one purple
// Gozuki: the ENGINE's own placement of 0xA263C. Every placement we swapped to
// that same id comes up plain, so the two populations share the enemy, the key
// and the area — the only difference left is in the record itself. Logging both
// populations (ours with the original key, the engine's as found) is what makes
// that differential possible offline. Capped: these are startup-only lines.
std::atomic<std::uint32_t> g_mapSwapLogs{0};
std::atomic<std::uint32_t> g_mapNativeLogs{0};
constexpr std::uint32_t kMapSwapLogMax = 512;
constexpr std::uint32_t kMapNativeLogMax = 64;

void MapLogRecord(const char* tag, std::uintptr_t record, std::uint32_t originalKey) {
  const auto* d = reinterpret_cast<const std::uint32_t*>(record);
  const auto* fl = reinterpret_cast<const float*>(record);
  _MESSAGE("%s: map %s rec=%p orig=%X id=%X flags=%X f50=%g f58=%g "
           "f5C=%X f60=%X f64=%X f68=%X f6C=%X f70=%X f9C=%X fA0=%X fA4=%X "
           "fB0=%X fB8=%X fC0=%X fD8=%X fE0=%X fF0=%X fF8=%X pos=%g,%g,%g",
           kPluginName, tag, reinterpret_cast<void*>(record), originalKey, d[0],
           d[2], static_cast<double>(fl[0x50 / 4]),
           static_cast<double>(fl[0x58 / 4]), d[0x5C / 4], d[0x60 / 4],
           d[0x64 / 4], d[0x68 / 4], d[0x6C / 4], d[0x70 / 4], d[0x9C / 4],
           d[0xA0 / 4], d[0xA4 / 4], d[0xB0 / 4], d[0xB8 / 4], d[0xC0 / 4],
           d[0xD8 / 4], d[0xE0 / 4], d[0xF0 / 4], d[0xF8 / 4],
           static_cast<double>(fl[0x40 / 4]), static_cast<double>(fl[0x44 / 4]),
           static_cast<double>(fl[0x48 / 4]));
}

// Structure check used before writing into a candidate placement record.
//
// The baseline rule is "the 12 dwords after the flag word are all zero", and it
// is what keeps the sweep from writing into coincidental flag-word matches (a
// loose filter once produced 472 "records" against a handful of real ones and
// corrupted memory). But the all-zero rule is WRONG for a minority of genuine
// records: measured on a live map 2026-09-21, four placements in one cluster
// carry 0x0000E2B1 at +0x18 and are otherwise indistinguishable from their
// all-zero siblings. The sweep therefore dropped them, so their variant word was
// still the ORDINARY one when the game consumed the record - the plugin's own
// log shows exactly that ("map inst ... key=A697F flags=1F3701" immediately
// followed by "map source swap A697F -> A263C"). Those placements are the Gozuki
// that "never goes purple": by the time the instantiation hook rewrote the flags
// the variant decision had already been taken.
//
// So a non-zero tail is accepted only when the slot one record-stride away is
// itself an enemy placement. A real table has neighbours; an accidental match in
// unrelated data does not. The neighbour must pass the same flag-family test and
// carry a plausible enemy key, so this stays far stricter than dropping the tail
// requirement outright.
bool MapTailLooksLikePlacement(std::uintptr_t record,
                               const std::uint32_t* f,
                               std::uintptr_t regionBase,
                               std::size_t regionSize) {
  bool zeroTail = true;
  for (int k = 3; k < 15; ++k) {
    if (f[k] != 0) {
      zeroTail = false;
      break;
    }
  }
  if (zeroTail) {
    return true;
  }
  constexpr std::uintptr_t kRecordStride = 0x138;  // observed spacing
  for (int dir = -1; dir <= 1; dir += 2) {
    const auto neighbour =
        static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(record) +
            static_cast<std::intptr_t>(kRecordStride) * dir);
    // Never read outside the region we already know is committed: a fault here
    // would be caught by the caller's __except and throw the whole region away.
    if (neighbour + 64 > regionBase + regionSize ||
        neighbour < regionBase) {
      continue;
    }
    const auto* n = reinterpret_cast<const std::uint32_t*>(neighbour);
    if (!MapRecordFlagsLookLikePlacement(n[2]) || n[0] == 0) {
      continue;
    }
    if (n[1] == 0 || n[1] > 0x00FFFFFFu) {
      continue;  // not an enemy key: a different record family
    }
    return true;
  }
  return false;
}

// Scans one committed writable region. Returns false when the region vanished
// mid-walk (the game freed it between VirtualQuery and the read) — in that case
// nothing was written. SEH rather than a pre-check: there is no reliable
// "still mapped" test from user mode.
bool MapSweepRegion(std::uintptr_t base, std::size_t size) {
  if (g_mapKeyPoolCount.load(std::memory_order_acquire) == 0 &&
      g_mapSourceCount.load(std::memory_order_acquire) == 0) {
    return true;
  }
  // The zero-tail structure check below reads 15 dwords, and the loop used to
  // bound itself by 12 bytes — the last iterations of every region therefore
  // read past its end (one of the two ways the old sweep could fault).
  __try {
    for (std::uintptr_t p = base; p + 64 <= base + size; p += 4) {
      const auto* f = reinterpret_cast<const std::uint32_t*>(p);
      if (!MapRecordFlagsLookLikePlacement(f[2]) || f[0] == 0) {
        continue;  // cheap reject first; the flag word is rare
      }
      // A key from MapPool is what we write, so a record that already holds one
      // was never ours: it is the engine's own placement. Logging it here
      // (instead of a second walk) is safe because every address is visited once
      // per pass, and a swapped record carries the target only from the NEXT
      // pass. Membership, not equality with one id: MapPool may list several
      // targets and each of them needs this branch.
      if (IsMapTargetKey(f[1])) {
        bool nativeTail = true;
        for (int k = 3; k < 15; ++k) {
          if (f[k] != 0) {
            nativeTail = false;
            break;
          }
        }
        if (nativeTail &&
            g_mapNativeLogs.fetch_add(1, std::memory_order_relaxed) <
                kMapNativeLogMax) {
          MapLogRecord("native", p, 0);
        }
        // DO NOT just skip this record. "Already holds the target key" does NOT
        // mean "already correct": the engine authors its own placements of the
        // target enemy with EITHER variant word, and the ORDINARY one
        // (0x011F3701, bit16 set) spawns a plain enemy. This branch used to
        // `continue` here, so such a record was never rewritten by the sweep -
        // and MapBossHookBody bails out on !IsMapSource for the same key, so
        // nothing else ever fixed it either. Measured on a live map 2026-09-21:
        // 314 records carry key 0xA263C, two full sweep passes left exactly 2 of
        // them reading 0x011F3701 (the plugin's own log prints one of them:
        // "map native ... id=3D3DB7 flags=11F3701").
        //
        // NOTE 2026-09-21 (final): this branch alone does NOT decide plain vs
        // purple. The engine re-decides that per spawn, from the record's
        // instanceId, in a lookup that runs AFTER every field here has been
        // read - see kRevivePlainBranchPattern / MapForceEmpower. Fixing the
        // variant word here is still correct (it is what the engine writes for
        // its own powered-up placements) but it cannot turn a "you already
        // killed this one" placement purple on its own.
        {
          const std::uint32_t before = f[2];
          ApplyTargetFlags(p);
          if (f[2] != before) {
            static std::atomic<std::uint32_t> nativeFixed{0};
            const std::uint32_t n =
                nativeFixed.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
              _MESSAGE("%s: native target placement fixed rec=%p %X -> %X "
                       "(engine-authored ordinary variant)",
                       kPluginName, reinterpret_cast<void*>(p), before, f[2]);
            }
          }
        }
        continue;
      }
      if (!IsMapSource(f[1])) {
        continue;
      }
      const std::uint32_t target = MapPickTarget(f[1], f[0]);
      if (target == 0 || target == f[1]) {
        continue;
      }
      // Structure check before writing (see MapTailLooksLikePlacement): normally
      // "12 zero dwords after the flag word", relaxed only for a record that has
      // a real placement record one stride away.
      if (!MapTailLooksLikePlacement(p, f, base, size)) {
        continue;
      }
      if (g_mapSwapLogs.fetch_add(1, std::memory_order_relaxed) <
          kMapSwapLogMax) {
        MapLogRecord("swap", p, f[1]);
      }
      reinterpret_cast<volatile std::uint32_t*>(p)[1] = target;
      // Variant bits alongside the key, before the entity is built. This is NOT
      // what decides plain vs purple - see the note in the already-target branch
      // above and kRevivePlainBranchPattern.
      ApplyTargetFlags(p);
      // +0x58 (MapRank): 0 = follow the placement, 1 = force 500.0f, 2 = force
      // 1000.0f. MapRankEvery > 1 forces only every Nth placement. NOTE: this
      // field is NOT the plain/purple switch either (measured 2026-09-21 - the
      // one plain Gozuki's record was forced from 1000.0f to 500.0f before it
      // spawned and it still came up plain), so leave MapRank at 0 unless you
      // actually want the strength scalar rewritten.
      {
        const std::uint32_t every =
            g_mapRankEvery.load(std::memory_order_acquire);
        const std::uint32_t slot =
            g_mapRankStride.fetch_add(1, std::memory_order_relaxed);
        if (every <= 1 || (slot % every) == 0) {
          ApplyMapRank(p);
        }
      }
      g_mapSweepHits.fetch_add(1, std::memory_order_relaxed);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // The game freed this region between VirtualQuery and the read (or it was
    // never fully committed). Losing the region is fine — nothing was written.
    return false;
  }
  return true;
}

// One pass step over committed writable private memory. The cursor is KEPT
// BETWEEN CALLS: restarting from the minimum address every tick — the first
// version of this — only ever re-scanned the first 128 MB while the game heaps
// (where the placement records live) sit terabytes up, so the sweep never
// reached them and the near placements were still built before any rewrite
// (observed in game 2026-09-20 00:05: first entry, the two close Jailer Oni
// were empty while a distant one had re-streamed correctly).
std::atomic<std::uintptr_t> g_mapSweepCursor{0};
std::atomic<std::uint32_t> g_mapSweepPasses{0};

// Returns true once the whole address space has been walked (cursor wrapped).
bool MapSweepOnce() {
  std::atomic<std::uint32_t> skipped{0};  // regions lost to a mid-walk free
  SYSTEM_INFO systemInfo{};
  GetSystemInfo(&systemInfo);
  const std::uintptr_t minimum =
      reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
  const std::uintptr_t maximum =
      reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);

  std::uintptr_t address = g_mapSweepCursor.load(std::memory_order_relaxed);
  if (address < minimum || address >= maximum) {
    address = minimum;
  }
  std::size_t budget = 192u * 1024u * 1024u;
  MEMORY_BASIC_INFORMATION info{};
  bool completed = false;
  while (address < maximum && budget != 0 && !g_shutdown.load()) {
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) !=
        sizeof(info)) {
      address += 0x1000;
      continue;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto size = static_cast<std::size_t>(info.RegionSize);
    if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
        (info.Protect == PAGE_READWRITE || info.Protect == PAGE_WRITECOPY)) {
      if (!MapSweepRegion(base, size)) {
        skipped.fetch_add(1, std::memory_order_relaxed);
      }
      budget = size < budget ? budget - size : 0;
    }
    address = base + size;
  }
  if (address >= maximum) {
    address = minimum;
    completed = true;
  }
  g_mapSweepCursor.store(address, std::memory_order_relaxed);
  if (completed) {
    g_mapSweepPasses.fetch_add(1, std::memory_order_relaxed);
    const auto lost = skipped.load(std::memory_order_relaxed);
    if (lost != 0) {
      _MESSAGE("%s: map table scan skipped %llu region(s) freed mid-walk",
               kPluginName, lost);
    }
  }
  return completed;
}

// ONE-SHOT table randomiser (2026-09-20, second attempt). The placement records
// are built once at process start and never rebuilt afterwards — a data
// breakpoint on a record's own key field never fired again across area loads —
// so the correct level to patch is the record table itself, ONCE, instead of
// every instantiation. Consequence: with MapHook=0 (the default) the plugin
// installs no code hook at all and the game runs untouched after this pass,
// which is what removes the freeze risk the continuously running sweep had.
// The read/write loop itself is SEH-guarded and strictly bounded (see
// MapSweepRegion), so a region the game frees mid-walk costs one region, not
// the process.
void MapSweepWorker() {
  for (int tick = 0; tick < 20 && !g_shutdown.load(); ++tick) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (g_mapBoss.load(std::memory_order_acquire)) {
      break;  // the world has had ~30 s to build its placement table
    }
  }
  if (g_shutdown.load() || !g_mapBoss.load(std::memory_order_acquire)) {
    return;
  }
  // The table exists from process start, but if the walk finds nothing we are
  // simply too early (or the player has not loaded a save yet): wait and walk
  // again. A second walk can no longer see a source key once it was rewritten,
  // so "hits == 0" is the only thing that re-arms this loop.
  for (int attempt = 0; attempt < 20 && !g_shutdown.load(); ++attempt) {
    for (int step = 0; step < 64 && !g_shutdown.load(); ++step) {
      if (MapSweepOnce()) {
        break;
      }
    }
    if (g_mapSweepHits.load(std::memory_order_relaxed) != 0 ||
        g_mapKeyPoolCount.load(std::memory_order_acquire) == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20000));
    g_mapSweepCursor.store(0, std::memory_order_relaxed);
  }
  g_mapTableDone.store(true, std::memory_order_release);
  _MESSAGE("%s: map table randomise done: %llu record(s) rewritten, %u full "
           "pass(es), tableDone=1",
           kPluginName, g_mapSweepHits.load(std::memory_order_relaxed),
           g_mapSweepPasses.load(std::memory_order_relaxed));
}
