// pair.cpp - PairSwap for RandomBoss.
//
// Split out of main.cpp. A summon's enemy is decided by the PAIR of a
// request-side descriptor and a parameter-table entry; rewriting only one side
// aborts the spawn silently, so both must be rewritten together. This module
// finds the pairs, patches both, remembers every rewritten site and can restore
// them exactly (see RevertPairPatches).
#include "core.h"
#include "state.h"

// Implemented in main.cpp (shared by several modules).
bool IsListed(const std::atomic<std::uint32_t>* list, std::size_t count,
              std::uint32_t id);

// PairSwap sweep (2026-09-18 CE 终局结论, 实测可复现).
//
// A summon's enemy is decided by the PAIR of two small structures, both keyed
// by the enemy key:
//   request-side descriptor : key @ +0x0C, tag @ +0x10, 1.0f @ -0x08
//   parameter-table entry   : tag @ +0x00, key @ +0x04, 0x1F3F0x @ +0x08
// Rewriting only ONE side aborts the spawn silently (controlled A/B); rewriting
// the key on BOTH sides makes that summon button produce the target enemy —
// verified live twice (jailer button -> ninja appeared, with the tag left
// deliberately mismatched). The tag therefore only has to be a tag registered
// in this scene, so it is left untouched on both sides. Live entity identities
// store key<<4 in the 0xA0 slot pool, so a bare-key sweep can never hit them.
// True when a tag may take part in a pair match. With no whitelist configured
// the historical 0x90000..0x9FFFF band is used (the range the training room's
// roster tags live in). A configured whitelist replaces the band entirely, so
// tags outside it (e.g. Gozuki's world-map tag 0xCC15) can be targeted too.
bool PairTagAllowed(std::uint32_t tag) {
  const auto count = g_pairTagCount.load(std::memory_order_acquire);
  if (count == 0) {
    return tag >= 0x90000 && tag <= 0x9FFFF;
  }
  for (std::size_t i = 0; i < count && i < kMaxListEntries; ++i) {
    if (g_pairTags[i].load(std::memory_order_relaxed) == tag) {
      return true;
    }
  }
  return false;
}

// Target key configured for a summon button identified by its tag, or 0.
std::uint32_t PairMapLookup(std::uint32_t tag) {
  const auto count = g_pairMapCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count && i < kPairMapMax; ++i) {
    if (g_pairMapTag[i].load(std::memory_order_relaxed) == tag) {
      return g_pairMapKey[i].load(std::memory_order_relaxed);
    }
  }
  return 0;
}

// True when `value` is a key this plugin writes (used to decide whether a
// remembered site still holds our write before restoring it).
bool IsOurTargetKey(std::uint32_t value) {
  if (value == g_pairToKey.load(std::memory_order_relaxed)) {
    return true;
  }
  const auto count = g_pairMapCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count && i < kPairMapMax; ++i) {
    if (g_pairMapKey[i].load(std::memory_order_relaxed) == value) {
      return true;
    }
  }
  return false;
}

// Records a site this plugin rewrote together with its original value, so it can
// be restored exactly later. The stored address is always the 4-byte KEY field
// (both pair roles keep the key there).
void RememberPairPatch(std::uintptr_t addr, std::uint32_t original) {
  const auto n = g_pairPatchedCount.load(std::memory_order_relaxed);
  if (n >= kPairPatchSlots) {
    return;
  }
  g_pairPatchedAddr[n].store(addr, std::memory_order_relaxed);
  g_pairPatchedOrig[n].store(original, std::memory_order_relaxed);
  g_pairPatchedCount.store(n + 1, std::memory_order_release);
}

// Restores every remembered site that still holds one of our target keys.
// Faults are swallowed and slots that were changed by the game are left alone.
void RevertPairPatches() {
  const auto n = g_pairPatchedCount.exchange(0, std::memory_order_acq_rel);
  std::size_t restored = 0;
  for (std::size_t i = 0; i < n && i < kPairPatchSlots; ++i) {
    const auto addr = g_pairPatchedAddr[i].load(std::memory_order_relaxed);
    const auto original = g_pairPatchedOrig[i].load(std::memory_order_relaxed);
    if (addr == 0 || original == 0) {
      continue;
    }
    __try {
      auto* slot = reinterpret_cast<std::uint32_t*>(addr);
      if (*slot != original && IsOurTargetKey(*slot)) {
        *slot = original;
        ++restored;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  _MESSAGE("%s: pair revert: restored %zu of %zu site(s)", kPluginName,
           restored, n);
}

void PairScanShard(const RosterShard& shard, std::uint32_t fromKey,
                   std::uint32_t toKey, std::atomic<std::uint64_t>* logTick) {
  constexpr std::size_t kChunk = 0x10000;
  std::uint8_t buf[kChunk];
  for (std::size_t chunk = 0; chunk < shard.size; chunk += kChunk) {
    const auto n =
        (std::min)(static_cast<std::size_t>(kChunk), shard.size - chunk);
    __try {
      std::memcpy(buf, shard.base + chunk, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      break;
    }
    for (std::size_t off = 8; off + 8 <= n; off += 4) {
      std::uint32_t key = 0;
      std::memcpy(&key, buf + off, 4);
      std::uint32_t tagAfter = 0;
      std::uint32_t tagBefore = 0;
      std::uint32_t oneBefore = 0;
      std::memcpy(&tagAfter, buf + off + 4, 4);
      std::memcpy(&tagBefore, buf + off - 4, 4);
      std::memcpy(&oneBefore, buf + off - 8, 4);
      // Two shapes carry the key (see the header comment): the parameter-table
      // entry (tag before, 0x1F3F0x after) and the request-side descriptor
      // (tag after, 1.0f before). The tag identifies the summon button.
      const bool requestShape = oneBefore == 0x3F800000 && tagAfter != 0;
      const bool paramShape = (tagAfter & 0xFFFFFFF0u) == 0x1F3F00u &&
                              tagBefore != 0;
      if (!requestShape && !paramShape) {
        continue;
      }
      const std::uint32_t tag = paramShape ? tagBefore : tagAfter;
      // PairMap (tag -> key) wins; the legacy PairFromKey -> PairToKey rule is
      // kept as a fallback and additionally needs an allowed tag.
      std::uint32_t target = PairMapLookup(tag);
      if (target == 0) {
        if (key == fromKey && PairTagAllowed(tag)) {
          target = toKey;
        } else {
          continue;
        }
      }
      if (target == key) {
        continue;
      }
      const auto addr =
          reinterpret_cast<std::uintptr_t>(shard.base + chunk + off);
      __try {
        auto* slot = reinterpret_cast<std::uint32_t*>(addr);
        if (*slot == key) {
          *slot = target;
          if (g_pairRevert.load(std::memory_order_acquire)) {
            RememberPairPatch(addr, key);
          }
          const auto total =
              g_pairSwapCount.fetch_add(1, std::memory_order_relaxed);
          const auto now = GetTickCount64();
          auto last = logTick->load(std::memory_order_relaxed);
          // The first patches are always logged: every role of every mapping
          // must be visible (a rate limit once hid the request-desc line).
          // After that the log is throttled to one line per 5s.
          if (total < 32 ||
              (now - last > 5000 &&
               logTick->compare_exchange_strong(last, now))) {
            _MESSAGE("%s: pair swap #%llu tag 0x%05X key 0x%05X -> 0x%05X "
                     "(%s) at %p",
                     kPluginName, static_cast<unsigned long long>(total + 1),
                     tag, key, target, paramShape ? "param-entry"
                                                  : "request-desc",
                     reinterpret_cast<void*>(addr));
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
  }
}

void PairSweepOnce(std::uint32_t fromKey, std::uint32_t toKey,
                   std::atomic<std::uint64_t>* logTick) {
  std::vector<RosterShard> shards;
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  std::uintptr_t addr =
      reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
  const auto maxAddr =
      reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
      break;
    }
    addr = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (mbi.State != MEM_COMMIT ||
        (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_IMAGE) ||
        mbi.RegionSize < 0x10000 ||
        (mbi.Protect != PAGE_READWRITE && mbi.Protect != PAGE_WRITECOPY &&
         mbi.Protect != PAGE_EXECUTE_READWRITE)) {
      continue;
    }
    shards.push_back(
        {reinterpret_cast<const std::uint8_t*>(mbi.BaseAddress),
         static_cast<std::size_t>(mbi.RegionSize)});
  }
  constexpr std::size_t kThreads = 4;
  if (shards.size() < kThreads * 2) {
    for (const auto& s : shards) {
      PairScanShard(s, fromKey, toKey, logTick);
    }
    return;
  }
  std::vector<std::thread> pool;
  std::atomic<std::size_t> next{0};
  for (std::size_t t = 0; t < kThreads; ++t) {
    pool.emplace_back([&shards, &next, fromKey, toKey, logTick]() {
      for (;;) {
        const auto i = next.fetch_add(1);
        if (i >= shards.size() || g_shutdown.load()) {
          break;
        }
        PairScanShard(shards[i], fromKey, toKey, logTick);
      }
    });
  }
  for (auto& th : pool) {
    th.join();
  }
}

void PairLoop() {
  try {
    std::atomic<std::uint64_t> logTick{0};
    bool wasActive = false;
    std::uint64_t lastGeneration = 0;
    while (!g_shutdown.load(std::memory_order_acquire)) {
      const auto from = g_pairFromKey.load(std::memory_order_acquire);
      const auto to = g_pairToKey.load(std::memory_order_acquire);
      const bool haveLegacy = from != 0 && to != 0 && from != to;
      const bool haveMap =
          g_pairMapCount.load(std::memory_order_acquire) != 0;
      const bool active = g_pairSwap.load(std::memory_order_acquire) &&
                          (haveLegacy || haveMap);
      const auto generation = g_configGeneration.load(std::memory_order_acquire);
      // Leaving the active state - or reloading the config, which may retarget
      // the mapping - restores everything we wrote first. A tag's key lives in
      // ONE global record, so a stale patch would be visible in every scene.
      // PairRevert=0 keeps the old "permanent rewrite" behaviour by never
      // remembering the sites.
      if (wasActive && (!active || generation != lastGeneration)) {
        RevertPairPatches();
        wasActive = false;
      }
      if (active) {
        PairSweepOnce(from, to, &logTick);
        wasActive = true;
        lastGeneration = generation;
      }
      Sleep(2000);
    }
    if (wasActive) {
      RevertPairPatches();
    }
  } catch (...) {
  }
}
