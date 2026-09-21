// roster.cpp - training-room roster key injection for RandomBoss.
//
// Split out of main.cpp. The summon pipeline validates a requested key against
// the room's roster, so swapping the key anywhere downstream makes the spawn
// abort silently. This module sweeps writable memory for the roster trio and
// rewrites the configured slot in place, which makes the game itself summon the
// target: menu, assets, params and validation all stay self-consistent.
#include "core.h"
#include "state.h"

// Implemented in main.cpp (shared by several modules).
bool IsListed(const std::atomic<std::uint32_t>* list, std::size_t count,
              std::uint32_t id);

// Roster injection: sweep private RW memory for the training-room trio
// {武者,忍者,狱卒鬼} packed within kRosterWindowBytes, then rewrite the 武者
// slot to the target key. Chunked memcpy with one __try per 64KB keeps the
// sweep fast (a per-dword SEH would take tens of seconds over a 2GB heap).
// The window constraint excludes the catalog slot table, which holds the
// same keys thousands of bytes apart (verified by CE scans 2026-09-18).
// 21:59 post-mortem: the first sweep took 33s over 7.6GB of cold pages, the
// menu cached the roster before the inject landed, and the mismatch
// (request=old key vs roster=new key) aborted the spawn into a per-frame
// retry loop — so the sweep now runs on 4 parallel threads back-to-back.
void RosterScanShard(const RosterShard& shard, std::uint32_t target,
                     std::atomic<std::uint64_t>* logTick) {
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
    for (std::size_t off = 0; off + 8 <= n; off += 4) {
      std::uint32_t v = 0;
      std::memcpy(&v, buf + off, 4);
      if (v != kRosterPatchSlot) {
        continue;
      }
      // 22:12 post-mortem: the menu reads yet another unpacked copy of the
      // key, unreachable by the trio-window rule. Nuclear rule: patch EVERY
      // standalone occurrence of the 武者 key in writable memory. The
      // catalog slot table packs [key|idx] qwords with a nonzero high dword,
      // so the zero-high-dword guard skips it (a patched slot would shadow
      // 忍者's entry and break the spawn's param lookups).
      std::uint32_t high = 0;
      std::memcpy(&high, buf + off + 4, 4);
      if (high != 0) {
        continue;
      }
      const auto patchAddr = reinterpret_cast<std::uintptr_t>(
          shard.base + chunk + off);
      __try {
        auto* slot = reinterpret_cast<std::uint32_t*>(patchAddr);
        if (*slot == kRosterPatchSlot) {
          *slot = target;
          const auto now = GetTickCount64();
          std::uint64_t last = logTick->load(std::memory_order_relaxed);
          if (now - last > 10000 ||
              logTick->compare_exchange_weak(last, now)) {
            _MESSAGE("%s: key inject 0x%05X -> 0x%05X at %p",
                     kPluginName, kRosterPatchSlot, target,
                     reinterpret_cast<void*>(patchAddr));
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
  }
}

void RosterSweepOnce(std::atomic<std::uint64_t>* logTick) {
  if (!g_rosterSwap.load(std::memory_order_acquire)) {
    return;
  }
  const auto target = g_createTarget.load(std::memory_order_acquire);
  if (target == 0) {
    return;
  }
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
    // MEM_PRIVATE: heap copies. MEM_IMAGE: static instances in module .data
    // (the roster source may live there — 22:12 evidence shows heap caches
    // being refreshed from somewhere our MEM_PRIVATE-only sweep never saw).
    if (mbi.State != MEM_COMMIT ||
        (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_IMAGE) ||
        mbi.RegionSize < 0x10000 ||
        (kRosterScanMaxRegion != 0 &&
         mbi.RegionSize > kRosterScanMaxRegion)) {
      continue;
    }
    if (mbi.Protect != PAGE_READWRITE && mbi.Protect != PAGE_WRITECOPY &&
        mbi.Protect != PAGE_EXECUTE_READWRITE) {
      continue;
    }
    shards.push_back(
        {reinterpret_cast<const std::uint8_t*>(mbi.BaseAddress),
         static_cast<std::size_t>(mbi.RegionSize)});
  }
  constexpr std::size_t kThreads = 4;
  if (shards.size() < kThreads * 2) {
    for (const auto& s : shards) {
      RosterScanShard(s, target, logTick);
    }
    return;
  }
  std::vector<std::thread> pool;
  std::atomic<std::size_t> next{0};
  for (std::size_t t = 0; t < kThreads; ++t) {
    pool.emplace_back([&shards, &next, target, logTick]() {
      for (;;) {
        const auto i = next.fetch_add(1);
        if (i >= shards.size() || g_shutdown.load()) {
          break;
        }
        RosterScanShard(shards[i], target, logTick);
      }
    });
  }
  for (auto& th : pool) {
    th.join();
  }
}

void RosterLoop() {
  try {
    std::atomic<std::uint64_t> logTick{0};
    while (!g_shutdown.load(std::memory_order_acquire)) {
      RosterSweepOnce(&logTick);
      Sleep(250);
    }
  } catch (...) {
  }
}
