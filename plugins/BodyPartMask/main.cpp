#define NOMINMAX
#include <Windows.h>

#include <ConfigUtils.h>
#include <HookUtils.h>
#include <LogUtils.h>
#include <PluginAPI.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// ---------------------------------------------------------------------------
// BodyPartMask
//
// Nioh 3 draws body sub-mesh i only while bit i of a 32-bit "hide mask" is 0.
// Bit i is row i of the body part table 0x5291EA5D (29 rows, 0-28).
//
// The mask that drives rendering is assembled every frame from the equipped
// equipment slots:
//
//   7FF6604EEB9F  or   esi,[rax+08]   ; one slot's mask, accumulated
//   7FF6604EEC48  mov  rcx,r12        ; esi now final
//   7FF6604EEC59  bt   esi,ebx        ; test bit i
//   7FF6604EEC61  setae r8b           ; mode = !bit
//   7FF6604EEC65  call 0x7FF6604EEC90 ; ApplyPart(obj, i+1, mode)
//
// Each slot's mask comes from a container record:
//   container = [[module + 0x45B9E30] + 0x60]
//   arrayBase = [container]
//   mask      = *(u32*)(arrayBase + index*20 + 0x10)
//
// This plugin patches no code.  A worker thread writes the configured bits into
// the container records named by the configured indices, which is exactly what
// the game itself reads.  Because a record belongs to the equipment, the effect
// follows that equipment.
//
// Configuration comes from the plugin's own ini plus every
// "*-BodyPartMask.ini" found in <game>\mods (root and ONE level of
// subdirectories, the same convention LooseFileLoader uses), so a mod can ship
// its own rules.
// ---------------------------------------------------------------------------

namespace fs = std::filesystem;

namespace {

constexpr const char* kPluginName = "BodyPartMask";
constexpr const char* kConfigSection = "BodyPartMask";
constexpr const char* kModConfigSuffix = "-BodyPartMask.ini";

// --- resolved at load: signature first, hardcoded RVA as fallback ------------
constexpr const char* kPlayerTablePattern =
    "48 8B 15 ? ? ? ? 33 C0 48 85 D2 74 ? 83 F9 03 77 ? 48 63 C1";
constexpr std::uintptr_t kPlayerTableRvaFallback = 0x4745348;

constexpr const char* kGetBasePattern =
    "48 8B 81 60 9F 00 00 48 85 C0 75 05 48 8D 41 20 C3";
constexpr std::uintptr_t kGetBaseRvaFallback = 0xF5358;

constexpr const char* kLookupPattern =
    "48 8D 04 80 48 8D 40 02 48 8D 04 82 48 83 C4 20 5B C3";
constexpr std::int32_t kLookupTailToEntry = -0x1C;
constexpr std::uintptr_t kLookupRvaFallback = 0x3FEDAC;

constexpr const char* kContainerGlobalPattern =
    "4C 8B 3D ? ? ? ? 41 8B 0E 83 E9 04 74";
constexpr std::uintptr_t kContainerGlobalRvaFallback = 0x45B9E30;

std::uintptr_t g_playerTableGlobal = 0;
std::uintptr_t g_getBase = 0;
std::uintptr_t g_lookup = 0;
std::uintptr_t g_containerGlobal = 0;

// --- structural offsets (data layout, not code layout) ----------------------
constexpr std::uintptr_t kPlayerObjectOffset = 0x1338;
constexpr std::uintptr_t kWrapperOffset = 0x9F0;
constexpr std::uintptr_t kContainerListOffset = 0x60;
constexpr std::uintptr_t kSlotArrayOffset = 0x57C0;
constexpr std::uint32_t kSlotStride = 0x340;
constexpr std::uint32_t kSlotCount = 7;
constexpr std::uint32_t kSlotObjectOffset = 0x10;
constexpr std::uint32_t kSlotKeyOffset = 0x1F8;

constexpr std::uint32_t kRecordStride = 20;
constexpr std::uint32_t kRecordMaskOffset = 0x10;
constexpr int kMaxTracked = 128;
constexpr DWORD kPollIntervalMs = 200;

// --- runtime state ----------------------------------------------------------
std::atomic_bool g_enabled{true};
std::atomic_bool g_applyOn{true};
std::atomic_bool g_logIndices{false};
std::atomic<int> g_toggleKey{VK_F8};
std::atomic<int> g_captureKey{VK_F9};
std::atomic_bool g_keyWasDown{};
std::atomic_bool g_captureWasDown{};
std::atomic_bool g_captureRequested{false};
std::atomic_bool g_threadStarted{false};
std::string g_gameRootDir;
std::string g_pluginsDir;
// The DLL's own module handle.  GetModuleHandleW(nullptr) returns the EXE, so
// it must not be used to locate this plugin's ini file.
HMODULE g_selfModule = nullptr;

// One rule per container index, merged from every configuration file.
struct MergedRule {
  std::uint32_t index = 0;
  std::uint32_t hide = 0;
  std::uint32_t show = 0;
  std::string sources;
};
std::vector<MergedRule> g_rules;

// What this plugin changed on a record, so the edit can be undone exactly.
// Keyed by container index, not by rule position, so a reload that reorders or
// drops rules still restores the game's own value.
struct TrackedIndex {
  std::uint32_t index = 0;
  std::uint32_t added = 0;
  std::uint32_t cleared = 0;
};
TrackedIndex g_tracked[kMaxTracked];
int g_trackedCount = 0;

std::uintptr_t ModuleBase() {
  return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

// --- config helpers ---------------------------------------------------------

std::uint32_t ParseRowList(const std::string& text) {
  std::uint32_t mask = 0;
  const char* p = text.c_str();
  while (*p != '\0') {
    while (*p == ',' || *p == ' ' || *p == '\t' || *p == ';') ++p;
    if (*p == '\0') break;
    char* end = nullptr;
    const long value = std::strtol(p, &end, 10);
    if (end == p) break;
    if (value >= 0 && value < 32) mask |= (1u << value);
    p = end;
  }
  return mask;
}

int ParseIndexList(const std::string& text, std::uint32_t* out, int capacity) {
  int count = 0;
  const char* p = text.c_str();
  while (*p != '\0' && count < capacity) {
    while (*p == ',' || *p == ' ' || *p == '\t' || *p == ';') ++p;
    if (*p == '\0') break;
    char* end = nullptr;
    const unsigned long value = std::strtoul(p, &end, 16);
    if (end == p) break;
    out[count++] = static_cast<std::uint32_t>(value);
    p = end;
  }
  return count;
}

int ParseHotkey(const std::string& text) {
  if (text.size() >= 2 && (text[0] == 'F' || text[0] == 'f')) {
    const int n = std::atoi(text.c_str() + 1);
    if (n >= 1 && n <= 12) return VK_F1 + n - 1;
  }
  if (text.size() == 1) {
    const char c =
        static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
  }
  return VK_F9;
}

// Same section/key names as the plugin's own ini, but read from an explicit
// path so mod files can be parsed.
std::string ReadIniString(const fs::path& ini, const char* key,
                          const char* fallback) {
  char buffer[1024] = {0};
  GetPrivateProfileStringA(kConfigSection, key, fallback, buffer,
                           static_cast<DWORD>(sizeof(buffer)),
                           ini.string().c_str());
  return std::string(buffer);
}

std::int32_t ReadIniInt(const fs::path& ini, const char* key,
                        std::int32_t fallback) {
  return static_cast<std::int32_t>(GetPrivateProfileIntA(
      kConfigSection, key, fallback, ini.string().c_str()));
}

void AddRule(std::uint32_t index, std::uint32_t hide, std::uint32_t show,
             const std::string& source) {
  for (MergedRule& rule : g_rules) {
    if (rule.index != index) continue;
    rule.hide |= hide;
    rule.show |= show;
    if (rule.sources.find(source) == std::string::npos) {
      rule.sources += ", " + source;
    }
    return;
  }
  MergedRule rule;
  rule.index = index;
  rule.hide = hide;
  rule.show = show;
  rule.sources = source;
  g_rules.push_back(std::move(rule));
}

// Load one ini (the plugin's own or a mod's) and fold it into g_rules.
//
// Only "Indices" is mandatory.  When a mod file leaves HideRows/ShowRows empty
// it inherits the plugin's own values, so a mod can simply declare which
// container indices it owns and let the main configuration decide what is
// hidden.
int LoadRuleFile(const fs::path& ini, const char* label, bool inheritWhenEmpty,
                 std::uint32_t inheritedHide, std::uint32_t inheritedShow) {
  const std::string indices = ReadIniString(ini, "Indices", "");
  std::uint32_t parsed[64] = {0};
  const int count = ParseIndexList(indices, parsed, 64);
  if (count == 0) {
    if (!inheritWhenEmpty) {
      // The plugin's own file may deliberately carry no rules at all, leaving
      // everything to the mod files.
      _MESSAGE("%s: %s -> no Indices; all rules will come from mod files",
               kPluginName, label);
    }
    return 0;
  }

  std::uint32_t hide = ParseRowList(ReadIniString(ini, "HideRows", ""));
  std::uint32_t show = ParseRowList(ReadIniString(ini, "ShowRows", ""));
  const char* inherited = "";
  if (hide == 0 && show == 0 && inheritWhenEmpty) {
    hide = inheritedHide;
    show = inheritedShow;
    inherited = " (inherited)";
  }
  if (hide == 0 && show == 0) {
    _MESSAGE("%s: %s -> %d index(es) but nothing to hide or show; skipped",
             kPluginName, label, count);
    return 0;
  }

  for (int i = 0; i < count; ++i) {
    AddRule(parsed[i], hide, show, label);
  }
  _MESSAGE("%s: %s -> %d index(es), hide=0x%08X show=0x%08X%s", kPluginName,
           label, count, hide, show, inherited);
  return count;
}

void CollectModConfigs(const fs::path& dir, std::vector<fs::path>& out) {
  std::error_code ec;
  if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;
  for (const auto& entry :
       fs::directory_iterator(dir, fs::directory_options::skip_permission_denied,
                              ec)) {
    if (ec) break;
    std::error_code fileEc;
    if (!entry.is_regular_file(fileEc)) continue;
    std::wstring name = entry.path().filename().wstring();
    if (name.size() < std::wcslen(L"-BodyPartMask.ini")) continue;
    std::wstring tail = name.substr(name.size() - std::wcslen(L"-BodyPartMask.ini"));
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (tail == L"-bodypartmask.ini") out.push_back(entry.path());
  }
}

// Locate this plugin's own ini.  The loader hands us the plugins directory;
// falling back to the DLL's own path keeps it working either way.
fs::path ResolveOwnIni() {
  if (!g_pluginsDir.empty()) {
    return fs::path(g_pluginsDir) / (std::string(kPluginName) + ".ini");
  }
  wchar_t selfPath[MAX_PATH] = {0};
  GetModuleFileNameW(g_selfModule, selfPath, MAX_PATH);
  return fs::path(selfPath).replace_extension(L".ini");
}

// Rebuild every rule from the plugin ini plus the mods directory.  Called at
// startup and again on every F9.
void LoadConfig() {
  g_rules.clear();

  const fs::path ownIni = ResolveOwnIni();
  const std::uint32_t ownHide =
      ParseRowList(ReadIniString(ownIni, "HideRows", ""));
  const std::uint32_t ownShow =
      ParseRowList(ReadIniString(ownIni, "ShowRows", ""));
  LoadRuleFile(ownIni, "BodyPartMask.ini", false, 0, 0);

  if (!g_gameRootDir.empty()) {
    const fs::path modsDir = fs::path(g_gameRootDir) / "mods";
    std::vector<fs::path> files;
    CollectModConfigs(modsDir, files);

    std::error_code ec;
    std::vector<fs::path> subDirs;
    if (fs::exists(modsDir, ec) && fs::is_directory(modsDir, ec)) {
      for (const auto& entry : fs::directory_iterator(
               modsDir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::error_code dirEc;
        if (entry.is_directory(dirEc)) subDirs.push_back(entry.path());
      }
    }
    // LooseFileLoader order: mods root first, then the subdirectories sorted by
    // name, so conflicts resolve the same way.
    std::sort(subDirs.begin(), subDirs.end(),
              [](const fs::path& a, const fs::path& b) {
                return a.filename().wstring() < b.filename().wstring();
              });
    for (const fs::path& dir : subDirs) {
      CollectModConfigs(dir, files);
    }

    for (const fs::path& file : files) {
      LoadRuleFile(file, file.filename().string().c_str(), true, ownHide,
                   ownShow);
    }
  }

  g_enabled.store(
      ReadIniInt(ownIni, "Enabled", 1) != 0, std::memory_order_release);
  g_logIndices.store(
      ReadIniInt(ownIni, "LogIndices", 0) != 0, std::memory_order_release);
  g_toggleKey.store(
      ParseHotkey(ReadIniString(ownIni, "ToggleKey", "F8")),
      std::memory_order_release);
  g_captureKey.store(
      ParseHotkey(ReadIniString(ownIni, "CaptureKey", "F9")),
      std::memory_order_release);

  _MESSAGE("%s: config loaded - %zu rule(s), enabled=%d", kPluginName,
           g_rules.size(), g_enabled.load() ? 1 : 0);
  for (const MergedRule& rule : g_rules) {
    _MESSAGE("%s:   index 0x%X hide=0x%08X show=0x%08X  [%s]", kPluginName,
             rule.index, rule.hide, rule.show, rule.sources.c_str());
  }
}

// --- address resolution -----------------------------------------------------

bool InMainModule(std::uintptr_t address) {
  const HMODULE module = GetModuleHandleW(nullptr);
  if (module == nullptr || address == 0) return false;
  const std::optional<size_t> size = HookUtils::GetModuleSize(module);
  if (!size.has_value()) return false;
  const auto base = reinterpret_cast<std::uintptr_t>(module);
  return address >= base && address < base + *size;
}

void ResolveAddresses() {
  const std::uintptr_t module = ModuleBase();

  g_playerTableGlobal = 0;
  if (const std::uintptr_t match =
          HookUtils::ScanIDAPattern(kPlayerTablePattern)) {
    const std::uintptr_t resolved = HookUtils::ReadOffsetData(match, 3, 7);
    if (InMainModule(resolved)) g_playerTableGlobal = resolved;
  }
  if (g_playerTableGlobal == 0) {
    g_playerTableGlobal = module + kPlayerTableRvaFallback;
    _MESSAGE("%s: player table global from fallback RVA", kPluginName);
  }

  g_getBase = HookUtils::ScanIDAPattern(kGetBasePattern);
  if (!InMainModule(g_getBase)) {
    g_getBase = module + kGetBaseRvaFallback;
    _MESSAGE("%s: GetBase from fallback RVA", kPluginName);
  }

  g_lookup = 0;
  if (const std::uintptr_t match = HookUtils::ScanIDAPattern(kLookupPattern)) {
    const std::uintptr_t resolved =
        static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) +
                                    kLookupTailToEntry);
    if (InMainModule(resolved)) g_lookup = resolved;
  }
  if (g_lookup == 0) {
    g_lookup = module + kLookupRvaFallback;
    _MESSAGE("%s: record lookup from fallback RVA", kPluginName);
  }

  g_containerGlobal = 0;
  if (const std::uintptr_t match =
          HookUtils::ScanIDAPattern(kContainerGlobalPattern)) {
    const std::uintptr_t resolved = HookUtils::ReadOffsetData(match, 3, 7);
    if (InMainModule(resolved)) g_containerGlobal = resolved;
  }
  if (g_containerGlobal == 0) {
    g_containerGlobal = module + kContainerGlobalRvaFallback;
    _MESSAGE("%s: container global from fallback RVA", kPluginName);
  }

  _MESSAGE("%s: playerTable=%p (rva 0x%llX) getBase=%p (rva 0x%llX) "
           "lookup=%p (rva 0x%llX) containerGlobal=%p (rva 0x%llX)",
           kPluginName, reinterpret_cast<void*>(g_playerTableGlobal),
           static_cast<unsigned long long>(g_playerTableGlobal - module),
           reinterpret_cast<void*>(g_getBase),
           static_cast<unsigned long long>(g_getBase - module),
           reinterpret_cast<void*>(g_lookup),
           static_cast<unsigned long long>(g_lookup - module),
           reinterpret_cast<void*>(g_containerGlobal),
           static_cast<unsigned long long>(g_containerGlobal - module));
}

// --- container access -------------------------------------------------------

bool ResolveContainer(std::uint8_t** base, std::uint32_t* count) {
  *base = nullptr;
  *count = 0;
  if (g_containerGlobal == 0) return false;
  __try {
    const void* global = *reinterpret_cast<void* const*>(g_containerGlobal);
    if (global == nullptr) return false;
    const void* container = *reinterpret_cast<void* const*>(
        reinterpret_cast<const std::uint8_t*>(global) + kContainerListOffset);
    if (container == nullptr) return false;
    auto* records = *reinterpret_cast<std::uint8_t* const*>(container);
    if (records == nullptr) return false;
    const std::uint32_t n =
        *reinterpret_cast<const std::uint32_t*>(records + 4);
    if (n == 0 || n > 100000) return false;
    *base = records;
    *count = n;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::uint32_t* RecordMask(std::uint8_t* base, std::uint32_t index) {
  return reinterpret_cast<std::uint32_t*>(base + index * kRecordStride +
                                          kRecordMaskOffset);
}

TrackedIndex* FindTracked(std::uint32_t index) {
  for (int i = 0; i < g_trackedCount; ++i) {
    if (g_tracked[i].index == index) return &g_tracked[i];
  }
  if (g_trackedCount >= kMaxTracked) return nullptr;
  g_tracked[g_trackedCount].index = index;
  g_tracked[g_trackedCount].added = 0;
  g_tracked[g_trackedCount].cleared = 0;
  return &g_tracked[g_trackedCount++];
}

// Apply (or, with hide==show==0, undo) one index's mask.  Undoing the previous
// edit first is what makes REMOVING a row from HideRows take effect.
void ApplyOne(std::uint8_t* base, std::uint32_t index, std::uint32_t hide,
              std::uint32_t show) {
  TrackedIndex* tracked = FindTracked(index);
  if (tracked == nullptr) return;
  std::uint32_t* const slot = RecordMask(base, index);
  const std::uint32_t before = *slot;
  const std::uint32_t original = (before | tracked->cleared) & ~tracked->added;
  const std::uint32_t after = (original | hide) & ~show;
  tracked->added = after & ~original;
  tracked->cleared = original & ~after;
  if (after != before) {
    *slot = after;
  }
}

void ApplyMask(std::uint8_t* base) {
  for (const MergedRule& rule : g_rules) {
    ApplyOne(base, rule.index, rule.hide, rule.show);
  }
}

void RestoreMask(std::uint8_t* base) {
  for (const MergedRule& rule : g_rules) {
    ApplyOne(base, rule.index, 0, 0);
  }
}

void LogAllRecords(std::uint8_t* base, std::uint32_t count) {
  int reported = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t mask = *reinterpret_cast<const std::uint32_t*>(
        base + i * kRecordStride + kRecordMaskOffset);
    if (mask == 0) continue;
    if (reported < 512) {
      _MESSAGE("%s: index 0x%X mask=0x%08X", kPluginName, i, mask);
    }
    ++reported;
  }
  _MESSAGE("%s: startup scan - %d non-zero mask records of %u", kPluginName,
           reported, count);
}

// F9: report the container indices the PLAYER's equipment slots currently use,
// by walking the very structures the mask loop walks:
//   wrapper   = playerObject + 0x9F0
//   slotBase  = GetBase(wrapper) + 0x57C0        (7 records, stride 0x340)
//   slotObj   = [slotBase + i*0x340 + 0x10]
//   key       = [slotObj + 0x1F8]
//   record    = Lookup(container, key)           -> arrayBase + index*20 + 8
void CaptureSlots() {
  if (g_playerTableGlobal == 0 || g_getBase == 0 || g_lookup == 0 ||
      g_containerGlobal == 0) {
    _MESSAGE("%s: slot capture - addresses unresolved", kPluginName);
    return;
  }

  __try {
    const void* table = *reinterpret_cast<void* const*>(g_playerTableGlobal);
    if (table == nullptr) {
      _MESSAGE("%s: slot capture - player table not ready", kPluginName);
      return;
    }
    auto* playerObject = *reinterpret_cast<std::uint8_t* const*>(
        reinterpret_cast<const std::uint8_t*>(table) + kPlayerObjectOffset);
    if (playerObject == nullptr) {
      _MESSAGE("%s: slot capture - player object not ready", kPluginName);
      return;
    }

    const void* global = *reinterpret_cast<void* const*>(g_containerGlobal);
    if (global == nullptr) return;
    const void* container = *reinterpret_cast<void* const*>(
        reinterpret_cast<const std::uint8_t*>(global) + kContainerListOffset);
    if (container == nullptr) return;
    auto* records = *reinterpret_cast<std::uint8_t* const*>(container);
    if (records == nullptr) return;

    const auto getBase = reinterpret_cast<void* (*)(void*)>(g_getBase);
    const auto lookup =
        reinterpret_cast<void* (*)(void*, std::uint32_t)>(g_lookup);

    auto* slotBase =
        static_cast<std::uint8_t*>(getBase(playerObject + kWrapperOffset));
    if (slotBase == nullptr) {
      _MESSAGE("%s: slot capture - slot base not ready", kPluginName);
      return;
    }

    _MESSAGE("%s: ---- current equipment slot indices ----", kPluginName);
    // Mirror the mask loop's own skip test exactly.  It switches on the slot
    // type and, for types 4..8, skips the slot when the matching bit of the
    // wrapper's flag byte is set:
    //   type 4 -> bit0, 5 -> bit1, 6 -> bit2, 7 -> bit3, 8 -> bit4
    // Any other type always takes the normal path.
    const std::uint8_t wrapperFlags =
        *reinterpret_cast<const std::uint8_t*>(playerObject + kWrapperOffset + 8);
    _MESSAGE("%s: wrapper flags = 0x%02X", kPluginName, wrapperFlags);

    int found = 0;
    char pasteList[512] = {0};
    for (std::uint32_t i = 0; i < kSlotCount; ++i) {
      auto* slot = slotBase + kSlotArrayOffset + i * kSlotStride;
      auto* slotObject =
          *reinterpret_cast<std::uint8_t* const*>(slot + kSlotObjectOffset);
      if (slotObject == nullptr) continue;
      const std::uint32_t slotType =
          *reinterpret_cast<const std::uint32_t*>(slot);
      bool skipped = false;
      if (slotType >= 4 && slotType <= 8) {
        const auto bit = static_cast<std::uint8_t>(1u << (slotType - 4));
        skipped = (wrapperFlags & bit) != 0;
      }

      const std::uint32_t key =
          *reinterpret_cast<const std::uint32_t*>(slotObject + kSlotKeyOffset);
      auto* record =
          static_cast<std::uint8_t*>(lookup(const_cast<void*>(container), key));
      if (record == nullptr) {
        _MESSAGE("%s: slot %u key 0x%X -> no record", kPluginName, i, key);
        continue;
      }
      const std::ptrdiff_t index =
          ((record - 8) - records) / static_cast<std::ptrdiff_t>(kRecordStride);
      _MESSAGE("%s: slot %u  type %u  key 0x%X  ->  index 0x%llX%s", kPluginName,
               i, slotType, key, static_cast<unsigned long long>(index),
               skipped ? "   (skipped right now)" : "");
      if (skipped) {
        continue;  // keep it out of the paste list
      }
      // Also emit the bare hex form, which is exactly what goes into Indices.
      if (pasteList[0] != '\0') {
        strncat_s(pasteList, sizeof(pasteList), ",", _TRUNCATE);
      }
      char one[32] = {0};
      sprintf_s(one, sizeof(one), "%llX",
                static_cast<unsigned long long>(index));
      strncat_s(pasteList, sizeof(pasteList), one, _TRUNCATE);
      ++found;
    }
    if (found > 0) {
      _MESSAGE("%s: paste into Indices: %s", kPluginName, pasteList);
    } else {
      _MESSAGE("%s: no usable slot right now", kPluginName);
    }
    _MESSAGE("%s: ---- %d usable slot(s) ----", kPluginName, found);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    _MESSAGE("%s: slot capture faulted", kPluginName);
  }
}

DWORD WINAPI WorkerThread(LPVOID) {
  _MESSAGE("%s: worker thread started", kPluginName);
  while (true) {
    Sleep(kPollIntervalMs);

    const int key = g_toggleKey.load(std::memory_order_acquire);
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool wasDown = g_keyWasDown.exchange(down, std::memory_order_acq_rel);
    if (down && !wasDown) {
      const bool next = !g_applyOn.load(std::memory_order_acquire);
      g_applyOn.store(next, std::memory_order_release);
      _MESSAGE("%s: mask %s", kPluginName, next ? "ON" : "OFF");
    }

    const int captureKey = g_captureKey.load(std::memory_order_acquire);
    const bool captureDown = (GetAsyncKeyState(captureKey) & 0x8000) != 0;
    const bool captureWasDown =
        g_captureWasDown.exchange(captureDown, std::memory_order_acq_rel);
    if (captureDown && !captureWasDown) {
      g_captureRequested.store(true, std::memory_order_release);
    }

    if (!g_enabled.load(std::memory_order_acquire)) continue;

    std::uint8_t* base = nullptr;
    std::uint32_t count = 0;
    if (!ResolveContainer(&base, &count)) continue;

    if (g_captureRequested.exchange(false, std::memory_order_acq_rel)) {
      // F9 reloads every configuration file and applies it immediately.
      LoadConfig();
      CaptureSlots();
    }

    if (g_logIndices.exchange(false, std::memory_order_acq_rel)) {
      LogAllRecords(base, count);
    }

    if (g_applyOn.load(std::memory_order_acquire)) {
      ApplyMask(base);
    } else {
      RestoreMask(base);
    }
  }
  return 0;
}

bool StartWorker() {
  bool expected = false;
  if (!g_threadStarted.compare_exchange_strong(expected, true)) return true;
  HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
  if (thread == nullptr) {
    g_threadStarted.store(false, std::memory_order_release);
    _MESSAGE("%s: failed to start worker thread (%lu)", kPluginName,
             GetLastError());
    return false;
  }
  CloseHandle(thread);
  return true;
}

}  // namespace

extern "C" __declspec(dllexport) bool nioh3_plugin_initialize(
    const Nioh3PluginInitializeParam* param) {
  if (param != nullptr && param->game_root_dir != nullptr) {
    g_gameRootDir = param->game_root_dir;
  }
  if (param != nullptr && param->plugins_dir != nullptr) {
    g_pluginsDir = param->plugins_dir;
  }
  _MESSAGE("%s: ini=%s mods=%s", kPluginName, ResolveOwnIni().string().c_str(),
           (fs::path(g_gameRootDir) / "mods").string().c_str());
  ResolveAddresses();
  LoadConfig();
  _MESSAGE("%s initialized for game version %s", kPluginName,
           param->game_version_string);
  return StartWorker();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_selfModule = module;
    _MESSAGE("Initializing plugin: %s", kPluginName);
  }
  return TRUE;
}
