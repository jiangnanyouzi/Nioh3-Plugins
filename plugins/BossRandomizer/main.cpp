// BossRandomizer - 修炼房任意 boss / 按敌种替换（大地图同机制）
//
// 机制（2026-09-19 实测确认，见 plugins/RandomBoss/analysis/experiments/README.md 第 16-22 轮）:
//   * 敌人身份 = {key<<4, typeTag}；**key 决定是哪只敌人（也决定资产）**，
//     tag 只需是本场景已注册的类型 tag。
//   * 每个 tag 在全局敌种表里只有一条记录：
//         tag @ +0x00 | key @ +0x04 | 0x1F3F0x @ +0x08
//     把这条记录的 key 指向目标 => 该**类型的每一次出生**都变成目标敌人；
//     之后游戏自己按 key 解析并加载目标的资产 —— 所以本插件**从不碰资产层**
//     （不 hook GetResIdByFileKtid，也就没有那条线的崩溃风险）。
//   * 请求侧描述符（key @ +0x0C, tag @ +0x10, 1.0f @ -0x08）在你拜神社时会按记录重建，
//     通常无需改；看到时我们也会一并改成一致，避免失配。
//   * **硬约束**：补丁必须早于"构建出生名单的那个场景"加载（即进入修炼房之前）。
//     本插件在找到记录后立即打上（游戏启动约 10 s 内），所以流程是:
//         启动游戏 -> 等 10 s -> 拜神社 -> 进修炼房 -> 按按钮
//   * 一个 tag 的记录是**全局**的：映射生效期间该敌种在所有场景都会被替换。
//     关闭插件（Enabled=0 热重载）或重启游戏会把每一处还原成它自己的原始 key。

#define NOMINMAX
#include <Windows.h>

#include <HookUtils.h>
#include <LogUtils.h>
#include <PluginAPI.h>
#include <Relocation.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr const char* kPluginName = "BossRandomizer";
constexpr const char* kConfigSection = "BossRandomizer";
constexpr std::size_t kMaxList = 64;
constexpr std::size_t kMaxPatchSites = 512;
constexpr std::size_t kThreads = 4;

// Component factory core (v2.0.2.0). At entry RCX = handler object, RDX = spawn
// entity, R8D = the enemy KEY being created, R9D = category. EVERY enemy -
// scripted map spawns, nests, native bosses - is created through here, which is
// exactly why EnemyIdCollector can build a 689-entry key dictionary by hooking
// it. Rewriting R8D is therefore the one lever that also reaches enemy types
// with no entry in the summon record table (e.g. Shunobon) and map-only fodder,
// where patching the record table does nothing (measured 2026-09-19: patched
// records + shrine respawn left world-map 亡骸武者 unchanged).
// NOTE: the pattern starts at function+6, not at the entry: the entry bytes are
// single-use (the first inline hook overwrites them), while the +6 tail is still
// unique and survives cohabitation with other plugins.
constexpr const char* kFactoryCorePattern =
    "48 8B D9 4D 85 C9 74 22 41 8B 41 68 D1 E8 24 01";
using FnFactoryCore = void (*)(void* arg1, void* arg2, std::uint32_t spawnId,
                               void* paramObject);

struct Mapping {
  std::uint32_t tag;
  std::uint32_t key;
};

std::atomic_bool g_enabled{false};
std::atomic_bool g_shutdownRequested{false};
std::atomic_bool g_discover{false};
// Discover mode: distinct (tag, key) record pairs seen so far, so the user can
// learn a scene's enemy-type table (needed to extend this to the world map).
constexpr std::size_t kMaxSeenPairs = 96;
std::atomic<std::uint32_t> g_seenTag[kMaxSeenPairs];
std::atomic<std::uint32_t> g_seenKey[kMaxSeenPairs];
std::atomic<std::size_t> g_seenCount{0};
std::atomic_bool g_dryRun{false};
std::atomic_bool g_revertOnDisable{true};
std::atomic_bool g_allowDuplicates{false};
std::atomic<std::uint32_t> g_sourceTags[kMaxList];
std::atomic<std::size_t> g_sourceTagCount{0};
std::atomic<std::uint32_t> g_bossPool[kMaxList];
std::atomic<std::size_t> g_bossPoolCount{0};
// tag -> target key, resolved once per config load (no locks on the scan path).
std::atomic<std::shared_ptr<const std::vector<Mapping>>> g_mappings{};
std::atomic<std::uint64_t> g_configGeneration{0};
std::atomic<std::uint64_t> g_patchedCount{0};

// Factory-level replacement (reaches every enemy type, incl. ones with no
// record). SourceKeys empty = every non-blacklisted spawn is replaced.
std::atomic_bool g_factoryReplace{false};
std::atomic_bool g_factoryHooked{false};
std::atomic<std::uint32_t> g_factoryTarget{0};  // 0 = random pick from the pool
std::atomic<std::uint32_t> g_factorySource[kMaxList];
std::atomic<std::size_t> g_factorySourceCount{0};
std::atomic<std::uint32_t> g_factoryBlacklist[kMaxList];
std::atomic<std::size_t> g_factoryBlacklistCount{0};
std::atomic<std::uint64_t> g_factorySwapCount{0};
std::atomic<std::uint32_t> g_factoryLastFrom{0};
std::atomic<std::uint32_t> g_factoryLastTo{0};
std::atomic<std::uint32_t> g_randomState{0};

// Patch registry: each site with its own original key, so a revert is exact even
// when several different targets are in play.
std::atomic<std::uintptr_t> g_patchAddr[kMaxPatchSites];
std::atomic<std::uint32_t> g_patchOrig[kMaxPatchSites];
std::atomic<std::size_t> g_patchCount{0};

std::filesystem::path g_configPath;
FILETIME g_configMtime{};

std::size_t ParseIdList(std::string_view text, std::uint32_t* out,
                        std::size_t max) {
  std::size_t n = 0;
  std::size_t pos = 0;
  while (pos < text.size() && n < max) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == ',' || text[pos] == ';')) {
      ++pos;
    }
    if (pos >= text.size()) {
      break;
    }
    const std::string rest(text.substr(pos));
    char* end = nullptr;
    const auto value = std::strtoul(rest.c_str(), &end, 0);
    if (end == nullptr || end == rest.c_str()) {
      break;
    }
    if (value != 0) {
      out[n++] = static_cast<std::uint32_t>(value);
    }
    const auto consumed = static_cast<std::size_t>(end - rest.c_str());
    pos += consumed > 0 ? consumed : 1;
  }
  return n;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> ParseAssign(
    std::string_view text) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> result;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto comma = text.find(',', pos);
    const auto item = text.substr(
        pos, comma == std::string_view::npos ? std::string_view::npos
                                            : comma - pos);
    const auto eq = item.find('=');
    if (eq != std::string_view::npos) {
      const auto tag =
          std::strtoul(std::string(item.substr(0, eq)).c_str(), nullptr, 0);
      const auto key =
          std::strtoul(std::string(item.substr(eq + 1)).c_str(), nullptr, 0);
      if (tag != 0 && key != 0) {
        result.emplace_back(static_cast<std::uint32_t>(tag),
                            static_cast<std::uint32_t>(key));
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    pos = comma + 1;
  }
  return result;
}

std::string FormatKey(std::uint32_t key) {
  char buffer[16]{};
  std::snprintf(buffer, sizeof(buffer), "0x%05X", key);
  return buffer;
}

// Random mode: shuffle the pool over the source tags.
std::vector<Mapping> RollMappings(const std::vector<std::uint32_t>& tags,
                                  const std::vector<std::uint32_t>& pool,
                                  bool allowDuplicates,
                                  std::uint32_t seed) {
  std::vector<Mapping> out;
  if (tags.empty() || pool.empty()) {
    return out;
  }
  std::mt19937 rng(seed != 0 ? seed
                             : static_cast<std::uint32_t>(GetTickCount64()));
  std::vector<std::uint32_t> shuffled = pool;
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  for (std::size_t i = 0; i < tags.size(); ++i) {
    if (!allowDuplicates && i < shuffled.size()) {
      out.push_back({tags[i], shuffled[i]});  // distinct boss per tag
    } else {
      out.push_back({tags[i], shuffled[i % shuffled.size()]});
    }
  }
  return out;
}

void LoadConfig(const Nioh3PluginInitializeParam* param) {
  const std::filesystem::path pluginsDirectory =
      (param != nullptr && param->plugins_dir != nullptr)
          ? std::filesystem::path(param->plugins_dir)
          : (g_configPath.empty() ? std::filesystem::path{}
                                  : g_configPath.parent_path());
  const std::filesystem::path configPath =
      pluginsDirectory / (std::string(kPluginName) + ".ini");
  g_configPath = configPath;
  if (const HANDLE file = CreateFileW(
          configPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      file != INVALID_HANDLE_VALUE) {
    GetFileTime(file, nullptr, nullptr, &g_configMtime);
    CloseHandle(file);
  }

  char value[4096]{};
  auto readKey = [&](const char* key, const char* fallback) {
    const DWORD length = GetPrivateProfileStringA(
        kConfigSection, key, fallback, value, static_cast<DWORD>(sizeof(value)),
        configPath.string().c_str());
    if (length == 0) {
      WritePrivateProfileStringA(kConfigSection, key, fallback,
                                 configPath.string().c_str());
      value[0] = '\0';
    }
  };

  readKey("Enabled", "1");
  g_enabled.store(std::strtoul(value, nullptr, 0) != 0,
                  std::memory_order_release);
  readKey("DryRun", "0");
  g_dryRun.store(std::strtoul(value, nullptr, 0) != 0,
                 std::memory_order_release);
  readKey("Discover", "0");
  g_discover.store(std::strtoul(value, nullptr, 0) != 0,
                   std::memory_order_release);
  readKey("RevertOnDisable", "1");
  g_revertOnDisable.store(std::strtoul(value, nullptr, 0) != 0,
                          std::memory_order_release);
  readKey("AllowDuplicates", "0");
  g_allowDuplicates.store(std::strtoul(value, nullptr, 0) != 0,
                          std::memory_order_release);
  readKey("Mode", "random");
  const std::string mode = value;
  readKey("RandomSeed", "0");
  const auto seed = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0));

  readKey("SourceTags", "0x946AD,0x92EA6,0x93EC7,0x93077");
  std::vector<std::uint32_t> tags;
  {
    std::uint32_t parsed[kMaxList]{};
    const auto n = ParseIdList(value, parsed, kMaxList);
    tags.assign(parsed, parsed + n);
    g_sourceTagCount.store(n, std::memory_order_release);
    for (std::size_t i = 0; i < n; ++i) {
      g_sourceTags[i].store(parsed[i], std::memory_order_relaxed);
    }
  }

  readKey("BossPool", "0xA263C,0x782F6,0x41DB6,0xC76B4");
  std::vector<std::uint32_t> pool;
  {
    std::uint32_t parsed[kMaxList]{};
    const auto n = ParseIdList(value, parsed, kMaxList);
    pool.assign(parsed, parsed + n);
    g_bossPoolCount.store(n, std::memory_order_release);
    for (std::size_t i = 0; i < n; ++i) {
      g_bossPool[i].store(parsed[i], std::memory_order_relaxed);
    }
  }

  readKey("Assign", "");
  std::vector<Mapping> mappings;
  if (mode == "explicit") {
    for (const auto& [tag, key] : ParseAssign(value)) {
      mappings.push_back({tag, key});
    }
  } else {
    mappings = RollMappings(tags, pool, g_allowDuplicates.load(), seed);
  }
  g_mappings.store(
      std::make_shared<const std::vector<Mapping>>(std::move(mappings)),
      std::memory_order_release);

  readKey("FactoryReplace", "0");
  g_factoryReplace.store(std::strtoul(value, nullptr, 0) != 0,
                         std::memory_order_release);
  readKey("FactoryTarget", "0");
  g_factoryTarget.store(
      static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)),
      std::memory_order_release);
  readKey("FactorySource", "");
  {
    std::uint32_t parsed[kMaxList]{};
    const auto n = ParseIdList(value, parsed, kMaxList);
    g_factorySourceCount.store(n, std::memory_order_release);
    for (std::size_t i = 0; i < n; ++i) {
      g_factorySource[i].store(parsed[i], std::memory_order_relaxed);
    }
  }
  readKey("FactoryBlacklist", "0x64");
  {
    std::uint32_t parsed[kMaxList]{};
    const auto n = ParseIdList(value, parsed, kMaxList);
    g_factoryBlacklistCount.store(n, std::memory_order_release);
    for (std::size_t i = 0; i < n; ++i) {
      g_factoryBlacklist[i].store(parsed[i], std::memory_order_relaxed);
    }
  }
  g_randomState.store(seed != 0 ? seed
                                : static_cast<std::uint32_t>(GetTickCount64()),
                      std::memory_order_release);

  _MESSAGE("%s: enabled=%d mode=%s dryRun=%d discover=%d revert=%d tags=%zu "
           "pool=%zu mappings=%zu",
           kPluginName, g_enabled.load(std::memory_order_acquire) ? 1 : 0,
           mode.c_str(), g_dryRun.load(std::memory_order_acquire) ? 1 : 0,
           g_discover.load(std::memory_order_acquire) ? 1 : 0,
           g_revertOnDisable.load(std::memory_order_acquire) ? 1 : 0, tags.size(),
           pool.size(), g_mappings.load()->size());
  _MESSAGE("%s: factoryReplace=%d factoryTarget=%s factorySources=%zu "
           "factoryBlacklist=%zu",
           kPluginName, g_factoryReplace.load(std::memory_order_acquire) ? 1 : 0,
           FormatKey(g_factoryTarget.load(std::memory_order_acquire)).c_str(),
           g_factorySourceCount.load(std::memory_order_acquire),
           g_factoryBlacklistCount.load(std::memory_order_acquire));
  for (const auto& mapping : *g_mappings.load()) {
    _MESSAGE("%s:   tag %s -> key %s", kPluginName,
             FormatKey(mapping.tag).c_str(), FormatKey(mapping.key).c_str());
  }

  g_configGeneration.fetch_add(1, std::memory_order_acq_rel);

  // Re-stat AFTER readKey() may have written fallbacks back into the ini: our
  // own write must not look like a user edit, or the watcher reloads every 10 s
  // and every reload would revert the patches (bug found 2026-09-19).
  if (const HANDLE file = CreateFileW(
          configPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      file != INVALID_HANDLE_VALUE) {
    GetFileTime(file, nullptr, nullptr, &g_configMtime);
    CloseHandle(file);
  }
}

std::uint32_t TargetForTag(std::uint32_t tag) {
  const auto mappings = g_mappings.load(std::memory_order_acquire);
  if (mappings == nullptr) {
    return 0;
  }
  for (const auto& mapping : *mappings) {
    if (mapping.tag == tag) {
      return mapping.key;
    }
  }
  return 0;
}

bool IsOurTargetKey(std::uint32_t value) {
  const auto mappings = g_mappings.load(std::memory_order_acquire);
  if (mappings == nullptr) {
    return false;
  }
  for (const auto& mapping : *mappings) {
    if (mapping.key == value) {
      return true;
    }
  }
  return false;
}

// ---- factory-level replacement --------------------------------------------
// Allocation-free and log-free on purpose: this runs inside the game's own
// entity-creation call, and calling spdlog / building a std::string there
// crashed the game at launch (0xc0000005, faulting module "unknown", ~13 s
// after the hook was installed - i.e. on the first spawns). All reporting is
// done by FactoryHookLoop on our own thread.
std::uint32_t FactoryMapId(std::uint32_t spawnId) {
  if (!g_factoryReplace.load(std::memory_order_relaxed) || spawnId == 0) {
    return spawnId;
  }
  const auto blackCount =
      g_factoryBlacklistCount.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < blackCount && i < kMaxList; ++i) {
    if (g_factoryBlacklist[i].load(std::memory_order_relaxed) == spawnId) {
      return spawnId;
    }
  }
  const auto sourceCount = g_factorySourceCount.load(std::memory_order_relaxed);
  if (sourceCount != 0) {
    bool listed = false;
    for (std::size_t i = 0; i < sourceCount && i < kMaxList; ++i) {
      if (g_factorySource[i].load(std::memory_order_relaxed) == spawnId) {
        listed = true;
        break;
      }
    }
    if (!listed) {
      return spawnId;
    }
  }
  std::uint32_t target = g_factoryTarget.load(std::memory_order_relaxed);
  if (target == 0) {
    const auto poolCount = g_bossPoolCount.load(std::memory_order_relaxed);
    if (poolCount == 0) {
      return spawnId;
    }
    const auto state =
        g_randomState.fetch_add(0x9E3779B9u, std::memory_order_relaxed);
    target = g_bossPool[(state >> 8) % poolCount].load(
        std::memory_order_relaxed);
  }
  if (target == 0 || target == spawnId) {
    return spawnId;
  }
  g_factoryLastFrom.store(spawnId, std::memory_order_relaxed);
  g_factoryLastTo.store(target, std::memory_order_relaxed);
  g_factorySwapCount.fetch_add(1, std::memory_order_relaxed);
  return target;
}

void InstallFactoryHook() {
  if (g_factoryHooked.load(std::memory_order_acquire)) {
    return;
  }
  const std::uintptr_t factoryCore =
      HookUtils::ScanIDAPattern(kFactoryCorePattern);
  if (factoryCore == 0) {
    _MESSAGE("%s: factory core pattern not found (will retry)", kPluginName);
    return;
  }
  HookLambda(reinterpret_cast<FnFactoryCore>(factoryCore),
             [](void* arg1, void* arg2, std::uint32_t spawnId,
                void* paramObject) {
               original(arg1, arg2, FactoryMapId(spawnId), paramObject);
             });
  g_factoryHooked.store(true, std::memory_order_release);
  _MESSAGE("%s: factory core hook installed at %p", kPluginName,
           reinterpret_cast<void*>(factoryCore));
}

void FactoryHookLoop() {
  std::uint64_t reported = 0;
  while (!g_shutdownRequested.load(std::memory_order_acquire)) {
    if (g_factoryReplace.load(std::memory_order_acquire)) {
      InstallFactoryHook();
    }
    // Report from OUR thread, never from the hook itself.
    const auto total = g_factorySwapCount.load(std::memory_order_relaxed);
    if (total != reported) {
      if (reported == 0 || total - reported >= 1) {
        _MESSAGE("%s: factory swap total=%llu last %s -> %s", kPluginName,
                 static_cast<unsigned long long>(total),
                 FormatKey(g_factoryLastFrom.load(std::memory_order_relaxed))
                     .c_str(),
                 FormatKey(g_factoryLastTo.load(std::memory_order_relaxed))
                     .c_str());
      }
      reported = total;
    }
    Sleep(2000);
  }
}

// SEH must live in POD-only helpers: MSVC rejects __try in any function that
// also contains objects requiring unwinding (C2712). Keeping the fault-guarded
// primitives separate lets the scan loop use std::string for logging.
bool TryCopyChunk(const std::uint8_t* source, std::uint8_t* destination,
                  std::size_t size) {
  __try {
    std::memcpy(destination, source, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

std::uint32_t TryReadU32(std::uintptr_t addr, bool* ok) {
  __try {
    *ok = true;
    return *reinterpret_cast<const std::uint32_t*>(addr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  *ok = false;
  return 0;
}

bool TryWriteU32(std::uintptr_t addr, std::uint32_t expected,
                 std::uint32_t value) {
  __try {
    auto* slot = reinterpret_cast<std::uint32_t*>(addr);
    if (*slot == expected) {
      *slot = value;
      return true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

void Remember(std::uintptr_t addr, std::uint32_t original) {
  const auto n = g_patchCount.load(std::memory_order_relaxed);
  if (n >= kMaxPatchSites) {
    return;
  }
  g_patchAddr[n].store(addr, std::memory_order_relaxed);
  g_patchOrig[n].store(original, std::memory_order_relaxed);
  g_patchCount.store(n + 1, std::memory_order_release);
}

void RevertAll() {
  const auto n = g_patchCount.exchange(0, std::memory_order_acq_rel);
  std::size_t restored = 0;
  for (std::size_t i = 0; i < n && i < kMaxPatchSites; ++i) {
    const auto addr = g_patchAddr[i].load(std::memory_order_relaxed);
    const auto original = g_patchOrig[i].load(std::memory_order_relaxed);
    if (addr == 0 || original == 0) {
      continue;
    }
    bool ok = false;
    const auto current = TryReadU32(addr, &ok);
    if (!ok || current == original || !IsOurTargetKey(current)) {
      continue;
    }
    if (TryWriteU32(addr, current, original)) {
      ++restored;
    }
  }
  _MESSAGE("%s: revert: restored %zu of %zu site(s)", kPluginName, restored, n);
}

struct Shard {
  const std::uint8_t* base;
  std::size_t size;
};

void ScanShard(const Shard& shard) {
  constexpr std::size_t kChunk = 0x10000;
  static std::atomic<std::uint64_t> logTick{0};
  std::uint8_t buffer[kChunk];
  for (std::size_t chunk = 0; chunk < shard.size; chunk += kChunk) {
    const auto n =
        (std::min)(static_cast<std::size_t>(kChunk), shard.size - chunk);
    if (!TryCopyChunk(shard.base + chunk, buffer, n)) {
      break;
    }
    for (std::size_t off = 8; off + 8 <= n; off += 4) {
      std::uint32_t key = 0;
      std::uint32_t tagAfter = 0;
      std::uint32_t tagBefore = 0;
      std::uint32_t oneBefore = 0;
      std::memcpy(&key, buffer + off, 4);
      std::memcpy(&tagAfter, buffer + off + 4, 4);
      std::memcpy(&tagBefore, buffer + off - 4, 4);
      std::memcpy(&oneBefore, buffer + off - 8, 4);
      // Record entry: tag | key | 0x1F3F0x    (this is the one that decides)
      // Request descriptor: 1.0f | key | tag  (rebuilt by the shrine; keep in sync)
      const bool recordShape =
          (tagAfter & 0xFFFFFFF0u) == 0x1F3F00u && tagBefore != 0;
      const bool requestShape = oneBefore == 0x3F800000 && tagAfter != 0;
      if (!recordShape && !requestShape) {
        continue;
      }
      const std::uint32_t tag = recordShape ? tagBefore : tagAfter;
      // Discover mode logs every distinct record pair the scene contains, which
      // is how a scene's enemy-type table (tag -> key) can be read off. It only
      // considers plausible type tags to keep the output readable.
      if (recordShape && g_discover.load(std::memory_order_acquire) &&
          tag >= 0x1000 && tag < 0x100000 && key >= 0x1000) {
        const auto seen = g_seenCount.load(std::memory_order_acquire);
        bool known = false;
        for (std::size_t i = 0; i < seen && i < kMaxSeenPairs; ++i) {
          if (g_seenTag[i].load(std::memory_order_relaxed) == tag &&
              g_seenKey[i].load(std::memory_order_relaxed) == key) {
            known = true;
            break;
          }
        }
        if (!known && seen < kMaxSeenPairs) {
          g_seenTag[seen].store(tag, std::memory_order_relaxed);
          g_seenKey[seen].store(key, std::memory_order_relaxed);
          g_seenCount.store(seen + 1, std::memory_order_release);
          _MESSAGE("%s: record tag %s key %s", kPluginName,
                   FormatKey(tag).c_str(), FormatKey(key).c_str());
        }
      }
      const std::uint32_t target = TargetForTag(tag);
      if (target == 0 || target == key) {
        continue;
      }
      const auto addr =
          reinterpret_cast<std::uintptr_t>(shard.base + chunk + off);
      if (g_dryRun.load(std::memory_order_acquire)) {
        continue;
      }
      if (!TryWriteU32(addr, key, target)) {
        continue;
      }
      Remember(addr, key);
      const auto total = g_patchedCount.fetch_add(1, std::memory_order_relaxed);
      const auto now = GetTickCount64();
      auto last = logTick.load(std::memory_order_relaxed);
      if (total < 32 ||
          (now - last > 5000 &&
           logTick.compare_exchange_strong(last, now))) {
        _MESSAGE("%s: patch #%llu tag %s key %s -> %s (%s) at %p", kPluginName,
                 static_cast<unsigned long long>(total + 1),
                 FormatKey(tag).c_str(), FormatKey(key).c_str(),
                 FormatKey(target).c_str(),
                 recordShape ? "record" : "request-desc",
                 reinterpret_cast<void*>(addr));
      }
    }
  }
}

void SweepOnce() {
  std::vector<Shard> shards;
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  auto addr = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
  const auto maxAddr =
      reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
      break;
    }
    addr = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE ||
        mbi.RegionSize < 0x1000 ||
        (mbi.Protect != PAGE_READWRITE && mbi.Protect != PAGE_WRITECOPY &&
         mbi.Protect != PAGE_EXECUTE_READWRITE)) {
      continue;
    }
    shards.push_back({reinterpret_cast<const std::uint8_t*>(mbi.BaseAddress),
                      static_cast<std::size_t>(mbi.RegionSize)});
  }
  if (shards.empty()) {
    return;
  }
  if (shards.size() < kThreads * 2) {
    for (const auto& shard : shards) {
      ScanShard(shard);
    }
    return;
  }
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> pool;
  for (std::size_t t = 0; t < kThreads; ++t) {
    pool.emplace_back([&shards, &next]() {
      for (;;) {
        const auto i = next.fetch_add(1);
        if (i >= shards.size()) {
          break;
        }
        ScanShard(shards[i]);
      }
    });
  }
  for (auto& thread : pool) {
    thread.join();
  }
}

void WorkerLoop() {
  try {
    bool wasActive = false;
    std::uint64_t lastGeneration = 0;
    while (true) {
      if (g_shutdownRequested.load(std::memory_order_acquire)) {
        break;
      }
      // Hot reload on config change.
      if (!g_configPath.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(g_configPath.c_str(), GetFileExInfoStandard,
                                 &data) != 0) {
          if (CompareFileTime(&data.ftLastWriteTime, &g_configMtime) != 0) {
            _MESSAGE("%s: config change detected, reloading", kPluginName);
            LoadConfig(nullptr);
          }
        }
      }
      const bool enabled = g_enabled.load(std::memory_order_acquire);
      const bool haveMapping =
          g_mappings.load(std::memory_order_acquire) != nullptr &&
          !g_mappings.load(std::memory_order_acquire)->empty();
      const auto generation = g_configGeneration.load(std::memory_order_acquire);
      const bool active = enabled && haveMapping;
      if (wasActive && (!active || generation != lastGeneration)) {
        // Retargeting or switching off: undo everything we wrote first, because
        // a tag's record is global and would otherwise stay redirected.
        if (g_revertOnDisable.load(std::memory_order_acquire)) {
          RevertAll();
        } else {
          g_patchCount.store(0, std::memory_order_release);
        }
        wasActive = false;
      }
      if (active) {
        SweepOnce();
        wasActive = true;
        lastGeneration = generation;
      }
      Sleep(2000);
    }
    if (wasActive && g_revertOnDisable.load(std::memory_order_acquire)) {
      RevertAll();
    }
  } catch (const std::exception& e) {
    _MESSAGE("%s: worker exception: %s", kPluginName, e.what());
  } catch (...) {
    _MESSAGE("%s: worker unknown exception", kPluginName);
  }
}

}  // namespace

extern "C" __declspec(dllexport) bool nioh3_plugin_initialize(
    const Nioh3PluginInitializeParam* param) {
  try {
    _MESSAGE("%s: init start", kPluginName);
    LoadConfig(param);
    try {
      std::thread(WorkerLoop).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: worker thread creation failed: %s", kPluginName, e.what());
    }
    try {
      std::thread(FactoryHookLoop).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: factory thread creation failed: %s", kPluginName, e.what());
    }
    _MESSAGE("%s: initialized (%s)", kPluginName,
             g_enabled.load(std::memory_order_acquire) ? "enabled" : "disabled");
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
    g_shutdownRequested.store(true, std::memory_order_release);
  }
  return TRUE;
}
