// config.cpp - the ini contract of RandomBoss.
//
// Split out of main.cpp. Everything that turns RandomBoss.ini into the shared
// atomics lives here: the two raw-file readers, the id/tag parsers, and
// LoadConfig itself. The globals below are the ones LoadConfig publishes - they
// are DEFINED here (not in main.cpp) so that "who writes this value" and "who
// owns this value" are the same file.
//
// Two details in here are load-bearing and were each diagnosed from a live
// failure; see the comments at their sites:
//   * raw-file reads for list values (GetPrivateProfileStringA truncates at the
//     first comma on this system),
//   * the mtime re-stat at the end of LoadConfig (otherwise the plugin's own
//     fallback writes look like a user edit and trigger a reload loop).
#include "core.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

bool ParseIdList(std::string_view text, std::uint32_t* out,
                 std::size_t capacity) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while (pos <= text.size() && count < capacity) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == ',' || text[pos] == ';' ||
            text[pos] == '\t')) {
      ++pos;
    }
    if (pos >= text.size()) {
      break;
    }
    std::uint32_t value = 0;
    bool hex = false;
    if (pos + 1 < text.size() && text[pos] == '0' &&
        (text[pos + 1] == 'x' || text[pos + 1] == 'X')) {
      hex = true;
      pos += 2;
    }
    std::size_t digits = 0;
    while (pos < text.size()) {
      const char ch = text[pos];
      int digit = -1;
      if (ch >= '0' && ch <= '9') {
        digit = ch - '0';
      } else if (ch >= 'a' && ch <= 'f') {
        digit = ch - 'a' + 10;
      } else if (ch >= 'A' && ch <= 'F') {
        digit = ch - 'A' + 10;
      }
      if (digit < 0 || (!hex && digit > 9) ||
          (hex && digit > 15)) {
        break;
      }
      value = value * (hex ? 16u : 10u) + static_cast<std::uint32_t>(digit);
      ++pos;
      ++digits;
    }
    if (digits > 0) {
      out[count++] = value;
    } else {
      ++pos;
    }
  }
  return count;
}

// GetPrivateProfileStringA truncates a comma-separated value at its first entry
// on this system — the quirk already documented for CreateSources. For the map
// lists that silently reduced MapPool to a single key (every Jailer Oni came out
// as the same boss) and MapSources to a single variant (Shunobon "did not
// react"), both reported by the user on 2026-09-20. So the list values are read
// from the raw file instead; the LAST occurrence wins, so the stale duplicate
// keys this plugin writes cannot shadow the live value either.
std::string ReadIniListValue(const std::filesystem::path& path, const char* key) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {};
  }
  std::string text;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != 0 &&
         read != 0) {
    text.append(buffer, read);
  }
  CloseHandle(file);

  const std::string prefix = std::string(key) + "=";
  std::string found;
  for (std::size_t position = text.find(prefix); position != std::string::npos;
       position = text.find(prefix, position + prefix.size())) {
    if (position != 0 && text[position - 1] != '\n') {
      continue;  // not at the start of a line
    }
    const std::size_t end = text.find('\n', position);
    std::string line = text.substr(
        position + prefix.size(),
        end == std::string::npos ? std::string::npos
                                 : end - position - prefix.size());
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    found = line;  // last occurrence wins
  }
  return found;
}

// "MapPool_<SOURCEHEX>=<list>" gives one source key its own target pool, e.g.
//   MapPool_300B8=0xA263C,0x782F6     ; 朱盆 -> Gozuki / Mezuki
//   MapPool_BC496=0x1B6CC,0x93457     ; 狱卒鬼 -> something else again
// A source with its own pool is swapped only into that pool; everything in
// MapSources without an entry here falls back to the global MapPool. Parsed from
// the raw file for the same comma reason as ReadIniListValue.
void LoadMapKeyPools(const std::filesystem::path& path) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  std::string text;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != 0 &&
         read != 0) {
    text.append(buffer, read);
  }
  CloseHandle(file);

  constexpr std::size_t kPrefixLength = 8;  // "MapPool_"
  std::size_t slots = 0;
  std::size_t position = 0;
  while (position < text.size() && slots < kMapPoolSlots) {
    const std::size_t lineEnd = text.find('\n', position);
    const std::size_t stop =
        lineEnd == std::string::npos ? text.size() : lineEnd;
    std::string line = text.substr(position, stop - position);
    position = stop + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.compare(0, kPrefixLength, "MapPool_") != 0) {
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos || equals <= kPrefixLength) {
      continue;
    }
    const std::uint32_t source = static_cast<std::uint32_t>(
        std::strtoul(line.substr(kPrefixLength, equals - kPrefixLength).c_str(),
                     nullptr, 16));
    if (source == 0) {
      continue;
    }
    std::uint32_t keys[kMapPoolMaxKeys] = {};
    const std::size_t count = ParseIdList(
        std::string_view(line).substr(equals + 1), keys, kMapPoolMaxKeys);
    std::size_t slot = slots;
    for (std::size_t i = 0; i < slots; ++i) {
      if (g_mapKeyPools[i].source.load(std::memory_order_relaxed) == source) {
        slot = i;  // last occurrence wins, like ReadIniListValue
        break;
      }
    }
    if (slot == slots) {
      ++slots;
    }
    g_mapKeyPools[slot].source.store(source, std::memory_order_release);
    for (std::size_t i = 0; i < count; ++i) {
      g_mapKeyPools[slot].keys[i].store(keys[i], std::memory_order_release);
    }
    g_mapKeyPools[slot].count.store(count, std::memory_order_release);
  }
  // The loop above stops the moment the slot array is full, which silently
  // ignores every remaining MapPool_ line. Report it: silent truncation is
  // exactly the failure mode that made "why did adding a variant change
  // nothing?" unanswerable from the log before (see ReadIniListValue's
  // history). Self-correcting: if the file simply ended, `position` is at the
  // end and nothing is counted, so a file with exactly kMapPoolSlots entries
  // does not warn.
  if (slots >= kMapPoolSlots) {
    std::size_t ignored = 0;
    for (std::size_t p = position; p < text.size();) {
      const std::size_t e = text.find('\n', p);
      const std::size_t stop = e == std::string::npos ? text.size() : e;
      std::string line = text.substr(p, stop - p);
      p = stop + 1;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.compare(0, kPrefixLength, "MapPool_") == 0) {
        ++ignored;
      }
    }
    if (ignored != 0) {
      _MESSAGE("%s: WARNING: MapPool_ slots are full (%zu distinct source "
               "keys); %zu remaining MapPool_ line(s) IGNORED. Merge or remove "
               "them - they are NOT in effect.",
               kPluginName, kMapPoolSlots, ignored);
    }
  }
  g_mapKeyPoolCount.store(slots, std::memory_order_release);
}

// Parses "0xSRC=0xDST,0xSRC2=0xDST2" into a map. Whitespace tolerant.
std::map<std::uint32_t, std::uint32_t> ParseKtidPairs(std::string_view text) {
  std::map<std::uint32_t, std::uint32_t> result;
  std::size_t pos = 0;
  auto skipSeparators = [&]() {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == ',' || text[pos] == ';')) {
      ++pos;
    }
  };
  auto parseHex = [&](std::uint32_t& out) -> bool {
    skipSeparators();
    if (pos + 1 < text.size() && text[pos] == '0' &&
        (text[pos + 1] == 'x' || text[pos + 1] == 'X')) {
      pos += 2;
    }
    std::uint32_t value = 0;
    std::size_t digits = 0;
    while (pos < text.size()) {
      const char ch = text[pos];
      int digit = -1;
      if (ch >= '0' && ch <= '9') digit = ch - '0';
      else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
      if (digit < 0) break;
      value = value * 16u + static_cast<std::uint32_t>(digit);
      ++pos;
      ++digits;
    }
    if (digits == 0) return false;
    out = value;
    return true;
  };
  while (pos < text.size()) {
    std::uint32_t src = 0;
    std::uint32_t dst = 0;
    if (!parseHex(src)) break;
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size() || (text[pos] != '=' && text[pos] != '>')) {
      ++pos;
      continue;
    }
    ++pos;
    if (!parseHex(dst)) break;
    result[src] = dst;
  }
  return result;
}

// ---------------------------------------------------------------------------
// LoadConfig
// ---------------------------------------------------------------------------

void LoadConfig(const Nioh3PluginInitializeParam* param) {
  // Hot reload passes nullptr — reuse the init-time config path's directory,
  // otherwise GetPrivateProfileString reads whatever RandomBoss.ini sits in
  // the process CWD and silently resets every key to its fallback (observed
  // 11:01:43: createSwap flipped to 0, assetSwap to 1, ktidPairs to 0).
  const std::filesystem::path pluginsDirectory =
      (param != nullptr && param->plugins_dir != nullptr)
          ? std::filesystem::path(param->plugins_dir)
          : (g_configPath.empty()
                 ? std::filesystem::path{}
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

  auto readKey = [&](const char* key, const char* fallback, char* buffer,
                     std::size_t size) {
    const DWORD length = GetPrivateProfileStringA(
        kConfigSection, key, fallback, buffer, static_cast<DWORD>(size),
        configPath.string().c_str());
    if (length == 0) {
      WritePrivateProfileStringA(kConfigSection, key, fallback,
                                 configPath.string().c_str());
    }
  };

  char value[2048]{};
  readKey(kConfigKeyTarget, "0xA263C", value, std::size(value));
  g_targetId.store(static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)),
                   std::memory_order_release);

  readKey(kConfigKeySources, "", value, std::size(value));
  if (value[0] == '\0') {
    g_swapAll.store(true, std::memory_order_release);
  } else {
    g_swapAll.store(false, std::memory_order_release);
    g_sourceCount.store(
        ParseIdList(value, reinterpret_cast<std::uint32_t*>(g_sourceIds),
                    kMaxListEntries),
        std::memory_order_release);
  }

  readKey(kConfigKeyBlacklist, "0x64", value, std::size(value));
  g_blacklistCount.store(
      ParseIdList(value, reinterpret_cast<std::uint32_t*>(g_blacklist),
                  kMaxListEntries),
      std::memory_order_release);

  readKey(kConfigKeyDiscover, "0", value, std::size(value));
  g_discoverMode.store(std::strtoul(value, nullptr, 0) != 0,
                       std::memory_order_release);
  g_discoverPath = pluginsDirectory / L"RandomBoss_discover.csv";

  readKey("FactorySwap", "0", value, std::size(value));
  g_factorySwap.store(std::strtoul(value, nullptr, 0) != 0,
                      std::memory_order_release);
  readKey("AssetSwap", "1", value, std::size(value));
  g_assetSwap.store(std::strtoul(value, nullptr, 0) != 0,
                    std::memory_order_release);

  readKey(kConfigKeyCreateSwap, "0", value, std::size(value));
  g_createSwap.store(std::strtoul(value, nullptr, 0) != 0,
                     std::memory_order_release);
  readKey(kConfigKeyCreateSources, "", value, std::size(value));
  // NOTE: log the raw buffer — a comma-truncation quirk in
  // GetPrivateProfileStringA has been observed reducing
  // "0x93457,0x1B6CC,0xBC496" to its first entry on this system.
  _MESSAGE("%s: CreateSources raw=[%s]", kPluginName, value);
  if (value[0] == '\0') {
    constexpr std::size_t kDefaultCount = std::size(kDefaultCreateSources);
    g_createSourceCount.store(kDefaultCount, std::memory_order_release);
    for (std::size_t i = 0; i < kDefaultCount; ++i) {
      g_createSources[i].store(kDefaultCreateSources[i],
                               std::memory_order_release);
    }
  } else {
    g_createSourceCount.store(
        ParseIdList(value, reinterpret_cast<std::uint32_t*>(g_createSources),
                    kMaxListEntries),
        std::memory_order_release);
  }
  readKey(kConfigKeyCreateTarget, "0xA263C", value, std::size(value));
  g_createTarget.store(static_cast<std::uint32_t>(std::strtoul(
                           value, nullptr, 0)),
                       std::memory_order_release);
  readKey("CreateMode", "2", value, std::size(value));
  g_createMode.store(std::strtoul(value, nullptr, 0),
                     std::memory_order_release);

  // --- source-level map swap (MapBossHookStub @ RVA 0x679895) ---------------
  readKey(kConfigKeyMapBoss, "1", value, std::size(value));
  g_mapBoss.store(std::strtoul(value, nullptr, 0) != 0,
                  std::memory_order_release);

  {
    const std::string raw = ReadIniListValue(configPath, kConfigKeyMapSources);
    value[0] = '\0';
    raw.copy(value, std::size(value) - 1);
  }
  if (value[0] == '\0') {
    // There is no built-in source list any more (see core.h): MapSources is
    // config-only, so an empty value means "no source keys" and swapping simply
    // does nothing. Say so loudly instead of substituting a table the user
    // cannot see or edit - a silent fallback is what made "why did adding a
    // variant change nothing?" impossible to answer from the log.
    g_mapSourceCount.store(0, std::memory_order_release);
    _MESSAGE("%s: WARNING: MapSources is empty - NO source keys configured, so "
             "NOTHING will be swapped. List them in RandomBoss.ini "
             "(comma-separated 0x ids).",
             kPluginName);
  } else {
    g_mapSourceCount.store(
        ParseIdList(value, reinterpret_cast<std::uint32_t*>(g_mapSources),
                    kMaxListEntries),
        std::memory_order_release);
  }

  {
    const std::string raw = ReadIniListValue(configPath, kConfigKeyMapPool);
    value[0] = '\0';
    raw.copy(value, std::size(value) - 1);
  }
  if (value[0] == '\0') {
    constexpr std::size_t kCount = std::size(kDefaultMapPool);
    g_mapPoolCount.store(kCount, std::memory_order_release);
    for (std::size_t i = 0; i < kCount; ++i) {
      g_mapPool[i].store(kDefaultMapPool[i], std::memory_order_release);
    }
  } else {
    g_mapPoolCount.store(
        ParseIdList(value, reinterpret_cast<std::uint32_t*>(g_mapPool),
                    kMaxListEntries),
        std::memory_order_release);
  }

  readKey(kConfigKeyMapRandomMode, "1", value, std::size(value));
  g_mapRandomMode.store(static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)),
                        std::memory_order_release);
  readKey("MapRank", "0", value, std::size(value));
  g_mapRankMode.store(static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)),
                      std::memory_order_release);
  readKey("MapRankEvery", "1", value, std::size(value));
  g_mapRankEvery.store(
      static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)),
      std::memory_order_release);
  readKey("MapPurple", "0", value, std::size(value));
  g_mapPurple.store(std::strtoul(value, nullptr, 0) != 0,
                    std::memory_order_release);
  // MapForceEmpower (default 0 = off): NOP the two-byte `jne` in the entity
  // activation path that sends an "already killed" placement down the plain
  // branch. This is the decision the engine makes per spawn from the record's
  // instanceId, which is why no record-side fix ever changed the outcome. See
  // kRevivePlainBranchPattern. Off by default because it also affects every
  // OTHER revived enemy in the game, not only the target.
  readKey("MapForceEmpower", "0", value, std::size(value));
  g_mapForceEmpower.store(std::strtoul(value, nullptr, 0) != 0,
                          std::memory_order_release);
  LoadMapKeyPools(configPath);
  readKey("MapHook", "0", value, std::size(value));
  g_mapHookEnabled.store(std::strtoul(value, nullptr, 0) != 0,
                         std::memory_order_release);
  // Read FactoryDiag BEFORE the summary line so the summary reports the value
  // that is actually in effect (it used to print the default, which made a
  // working config look like FactoryDiag=0).
  readKey(kConfigKeyFactoryDiag, "0", value, std::size(value));
  g_factoryDiag.store(std::strtoul(value, nullptr, 0) != 0,
                      std::memory_order_release);
  _MESSAGE("%s: MapBoss=%d MapSources=%zu MapPool=%zu MapPoolKeys=%zu "
           "MapRandomMode=%u MapHook=%d MapPurple=%d ForceEmpower=%d "
           "FactoryDiag=%d",
           kPluginName, g_mapBoss.load(std::memory_order_acquire) ? 1 : 0,
           g_mapSourceCount.load(std::memory_order_acquire),
           g_mapPoolCount.load(std::memory_order_acquire),
           g_mapKeyPoolCount.load(std::memory_order_acquire),
           g_mapRandomMode.load(std::memory_order_acquire),
           g_mapHookEnabled.load(std::memory_order_acquire) ? 1 : 0,
           g_mapPurple.load(std::memory_order_acquire) ? 1 : 0,
           g_mapForceEmpower.load(std::memory_order_acquire) ? 1 : 0,
           g_factoryDiag.load(std::memory_order_acquire) ? 1 : 0);
  readKey("CreateMaxPerMinute", "4", value, std::size(value));
  g_createMaxPerMinute.store(std::strtoul(value, nullptr, 0),
                             std::memory_order_release);
  readKey("CatalogSwap", "1", value, std::size(value));
  g_catalogSwap.store(std::strtoul(value, nullptr, 0) != 0,
                      std::memory_order_release);
  readKey("RosterSwap", "1", value, std::size(value));
  g_rosterSwap.store(std::strtoul(value, nullptr, 0) != 0,
                     std::memory_order_release);

  // IdentitySwap: canonical tags are verified data (CE 2026-09-18), so the
  // default table carries all three training-room bosses.
  readKey(kConfigKeyIdentitySwap, "0", value, std::size(value));
  g_identitySwap.store(std::strtoul(value, nullptr, 0) != 0,
                       std::memory_order_release);
  readKey(kConfigKeyIdentityFrom, "", value, std::size(value));
  g_identityFromCount.store(
      ParseIdList(value, reinterpret_cast<std::uint32_t*>(g_identityFrom),
                  kMaxListEntries),
      std::memory_order_release);
  readKey(kConfigKeyIdentityTo, "0", value, std::size(value));
  g_identityTo.store(static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)),
                     std::memory_order_release);
  readKey(kConfigKeyIdentityTags,
          "0x93457=0x946AD,0x1B6CC=0x92EA6,0xBC496=0x93EC7", value,
          std::size(value));
  g_identityTags.store(
      std::make_shared<const std::map<std::uint32_t, std::uint32_t>>(
          ParseKtidPairs(value)),
      std::memory_order_release);
  _MESSAGE("%s: identitySwap=%d identityFrom=%zu identityTo=0x%05X "
           "identityTags=%zu",
           kPluginName, g_identitySwap.load(std::memory_order_acquire) ? 1 : 0,
           g_identityFromCount.load(std::memory_order_acquire),
           g_identityTo.load(std::memory_order_acquire),
           g_identityTags.load(std::memory_order_acquire)->size());

  readKey(kConfigKeyPairSwap, "0", value, std::size(value));
  g_pairSwap.store(std::strtoul(value, nullptr, 0) != 0,
                   std::memory_order_release);
  readKey(kConfigKeyPairFrom, "0", value, std::size(value));
  g_pairFromKey.store(static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)),
                      std::memory_order_release);
  readKey(kConfigKeyPairTo, "0", value, std::size(value));
  g_pairToKey.store(static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)),
                    std::memory_order_release);
  readKey(kConfigKeyPairTags, "", value, std::size(value));
  g_pairTagCount.store(
      ParseIdList(value, reinterpret_cast<std::uint32_t*>(g_pairTags),
                  kMaxListEntries),
      std::memory_order_release);
  readKey(kConfigKeyPairRevert, "1", value, std::size(value));
  g_pairRevert.store(std::strtoul(value, nullptr, 0) != 0,
                     std::memory_order_release);
  readKey(kConfigKeyPairMap, "", value, std::size(value));
  {
    const auto mapping = ParseKtidPairs(value);
    std::size_t n = 0;
    for (const auto& [tag, key] : mapping) {
      if (n >= kPairMapMax || tag == 0 || key == 0) {
        continue;
      }
      g_pairMapTag[n].store(tag, std::memory_order_relaxed);
      g_pairMapKey[n].store(key, std::memory_order_relaxed);
      ++n;
    }
    g_pairMapCount.store(n, std::memory_order_release);
  }
  _MESSAGE("%s: pairSwap=%d pairFrom=0x%05X pairTo=0x%05X pairTags=%zu "
           "pairRevert=%d pairMap=%zu",
           kPluginName, g_pairSwap.load(std::memory_order_acquire) ? 1 : 0,
           g_pairFromKey.load(std::memory_order_acquire),
           g_pairToKey.load(std::memory_order_acquire),
           g_pairTagCount.load(std::memory_order_acquire),
           g_pairRevert.load(std::memory_order_acquire) ? 1 : 0,
           g_pairMapCount.load(std::memory_order_acquire));
  for (std::size_t i = 0; i < g_pairMapCount.load(std::memory_order_acquire);
       ++i) {
    _MESSAGE("%s: pairMap[%zu] tag 0x%05X -> key 0x%05X", kPluginName, i,
             g_pairMapTag[i].load(std::memory_order_relaxed),
             g_pairMapKey[i].load(std::memory_order_relaxed));
  }

  readKey("SwapKTIDs", "", value, std::size(value));
  auto swapMap = std::make_shared<const std::map<std::uint32_t, std::uint32_t>>(
      ParseKtidPairs(value));
  g_ktidSwapMap.store(std::move(swapMap), std::memory_order_release);

  readKey(kConfigKeyAssetTrace, "0", value, std::size(value));
  g_assetTrace.store(std::strtoul(value, nullptr, 0) != 0,
                     std::memory_order_release);
  _MESSAGE("%s: assetTrace=%d", kPluginName,
           g_assetTrace.load(std::memory_order_acquire) ? 1 : 0);

  g_configGeneration.fetch_add(1, std::memory_order_acq_rel);

  // Re-stat AFTER the fallback writes above: readKey() writes a missing/empty
  // key back into the ini (e.g. "SourceIds="), which bumps the file mtime.
  // Without this refresh the watcher reads its own write as a user edit and
  // reloads every 10 s - and since a reload reverts the pair patches, that
  // silently disabled the swap entirely (diagnosed 2026-09-19 08:29).
  if (const HANDLE file = CreateFileW(
          configPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      file != INVALID_HANDLE_VALUE) {
    GetFileTime(file, nullptr, nullptr, &g_configMtime);
    CloseHandle(file);
  }

  _MESSAGE("%s: target=0x%08X mode=%s sources=%zu blacklist=%zu discover=%d "
           "factorySwap=%d assetSwap=%d ktidPairs=%zu createSwap=%d "
           "createSources=%zu createTarget=0x%05X",
           kPluginName, g_targetId.load(std::memory_order_acquire),
           g_swapAll.load(std::memory_order_acquire) ? "swap-all"
                                                     : "source-list",
           g_sourceCount.load(std::memory_order_acquire),
           g_blacklistCount.load(std::memory_order_acquire),
           g_discoverMode.load(std::memory_order_acquire) ? 1 : 0,
           g_factorySwap.load(std::memory_order_acquire) ? 1 : 0,
           g_assetSwap.load(std::memory_order_acquire) ? 1 : 0,
           g_ktidSwapMap.load()->size(),
           g_createSwap.load(std::memory_order_acquire) ? 1 : 0,
           g_createSourceCount.load(std::memory_order_acquire),
           g_createTarget.load(std::memory_order_acquire));
}
