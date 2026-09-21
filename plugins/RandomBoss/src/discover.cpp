// discover.cpp - enemy-id discovery mode (RandomBoss).
//
// Split out of main.cpp. Discover mode is opt-in (Discover=1) and exists to
// calibrate the identity field offset: it scans the spawn-request object for
// dwords inside the plausible enemy-id range and dumps (offset, value) pairs to
// CSV, which is how the identity slot was originally located.
//
// Self-contained apart from the shared constants in core.h.
#include "core.h"

// DiscoverHit itself is declared in core.h; only the storage is defined here.
DiscoverHit g_discoverHits[kDiscoverMaxHits];
std::atomic<std::uint32_t> g_discoverCount{0};
std::atomic_bool g_discoverOverflow{false};
std::atomic_bool g_discoverMode{false};
std::filesystem::path g_discoverPath = L"RandomBoss_discover.csv";

void DiscoverScan(void* requestObject) {
  const auto base = reinterpret_cast<std::uintptr_t>(requestObject);
  if (base < 0x10000000000ULL || base >= 0x400000000000ULL) {
    return;
  }
  __try {
    for (std::uint32_t offset = 0; offset < kDiscoverScanBytes; offset += 4) {
      const std::uint32_t value =
          *reinterpret_cast<const std::uint32_t*>(base + offset);
      if (value < kIdRangeMin || value > kIdRangeMax) {
        continue;
      }
      // Ticket-based recording: storm volume makes per-hit dedup too costly;
      // duplicates are trivially collapsed in offline analysis.
      const auto index =
          g_discoverCount.fetch_add(1, std::memory_order_relaxed);
      if (index < kDiscoverMaxHits) {
        g_discoverHits[index] = DiscoverHit{offset, value};
      } else {
        g_discoverOverflow.store(true, std::memory_order_release);
        return;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void WriteDiscoverCensus() {
  try {
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_discoverPath.c_str(), L"w") != 0 ||
        file == nullptr) {
      return;
    }
    std::fputs("\xEF\xBB\xBF", file);
    std::fprintf(file, "# RandomBoss discover: (offset,value) dwords in the enemy-id range\n");
    if (g_discoverOverflow.load(std::memory_order_acquire)) {
      std::fprintf(file, "# WARNING: hit cap reached, output truncated\n");
    }
    std::fprintf(file, "# offset_hex,value_hex\n");
    const auto count =
        (std::min)(g_discoverCount.load(std::memory_order_acquire),
                   kDiscoverMaxHits);
    for (std::uint32_t i = 0; i < count; ++i) {
      std::fprintf(file, "0x%03X,0x%05X\n", g_discoverHits[i].offset,
                   g_discoverHits[i].value);
    }
    std::fclose(file);
  } catch (...) {
  }
}
