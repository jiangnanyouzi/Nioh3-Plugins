#define NOMINMAX
#include <Windows.h>

#include <HookUtils.h>
#include <LogUtils.h>
#include <PluginAPI.h>
#include <Relocation.h>

#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

constexpr const char* kPluginName = "AppearanceRefreshHotkey";
constexpr const char* kConfigSection = "AppearanceRefreshHotkey";
constexpr const char* kConfigKeyHotkey = "Hotkey";
constexpr const char* kDefaultHotkeyName = "F10";

constexpr std::uintptr_t kUpdateContextThunkRva = 0x1F135C;
constexpr std::uintptr_t kUpdateSingleObjectRva = 0x1F1534;
constexpr std::uintptr_t kCharacterParentOffset = 0x3A0;
// +0x12ADCC, called by RefreshPlayerAppearance, reads the player object from
// [module+0x4618398] + 0x1338 when invoked with selector zero.
constexpr std::uintptr_t kPlayerObjectTableGlobalRva = 0x4618398;
constexpr std::uintptr_t kPlayerObjectTableEntryOffset = 0x1338;

constexpr std::uintptr_t kSetAppearanceStateWordRva = 0x14BCA6C;
constexpr std::uintptr_t kRefreshPlayerAppearanceRva = 0x2477990;
constexpr std::uintptr_t kAppearanceStateTableGlobalRva = 0x44734B0;
constexpr std::uintptr_t kAppearanceStateTableOffset = 0x228F28;

constexpr std::uint16_t kNoArmorAppearanceOverride = 0xFFFF;
constexpr ULONGLONG kRefreshGapMs = 1000;

struct AppearanceRefreshEntry {
  std::uint32_t stateSelector;
  std::uint32_t stateWordIndex;
  // Armor: 0xFFFF no-override value. Weapon: primary fallback appearance.
  std::uint16_t transitionValue;
  // Weapon-only alternate fallback when current already equals transitionValue.
  std::uint16_t alternateWeaponFallbackValue;
  bool isWeapon;
};

// Proven state entries. Armor briefly removes its override (0xFFFF). A weapon
// temporarily uses another real appearance of the same type. Both fallback
// IDs were captured from the game's own indexed state writer.
constexpr std::array<AppearanceRefreshEntry, 30> kRefreshEntries{{
    {0, 0x20, kNoArmorAppearanceOverride, 0, false},
    {0, 0x21, kNoArmorAppearanceOverride, 0, false},
    {0, 0x22, kNoArmorAppearanceOverride, 0, false},
    {0, 0x23, kNoArmorAppearanceOverride, 0, false},
    {0, 0x24, kNoArmorAppearanceOverride, 0, false},
    // Katana, dual swords, spear, axe, odachi, switchglaive, fist.
    {0, 0x01, 0x27BF, 0x4BF7, true}, {0, 0x02, 0x4167, 0xB446, true},
    {0, 0x03, 0xFB93, 0xE044, true}, {0, 0x04, 0x4007, 0x1C06, true},
    {0, 0x06, 0xD641, 0xCCBB, true}, {0, 0x09, 0x8A92, 0x7993, true},
    {0, 0x0B, 0xCEBB, 0x195C, true},
    // Bow, rifle, cannon.
    {0, 0x18, 0xCC31, 0xE575, true}, {0, 0x19, 0xB1CF, 0xE4F8, true},
    {0, 0x1A, 0x5718, 0xBE5B, true},
    // Kusarigama, splitstaff, hand axe, bo staff.
    {1, 0x05, 0xE75F, 0xEAE2, true}, {1, 0x07, 0x275D, 0x2742, true},
    {1, 0x08, 0x4355, 0x18BA, true}, {1, 0x0A, 0xD2FD, 0xDB2A, true},
    {1, 0x20, kNoArmorAppearanceOverride, 0, false},
    {1, 0x21, kNoArmorAppearanceOverride, 0, false},
    {1, 0x22, kNoArmorAppearanceOverride, 0, false},
    {1, 0x23, kNoArmorAppearanceOverride, 0, false},
    {1, 0x24, kNoArmorAppearanceOverride, 0, false},
    // Ninja blade, ninja dual swords, ninja claw, ninja bow/rifle/cannon.
    {1, 0x0C, 0xB9C8, 0xD2C4, true}, {1, 0x0D, 0xA3A1, 0xE31B, true},
    {1, 0x0E, 0x8C65, 0x2F00, true}, {1, 0x18, 0xCC31, 0x8E1A, true},
    {1, 0x19, 0xB1CF, 0x7C7A, true}, {1, 0x1A, 0x5718, 0x1DC8, true},
}};

using FnUpdateContextThunk = void (*)(void* updateContext);
using FnUpdateSingleObject = void (*)(void* updateObject);
using FnSetAppearanceStateWord = void (*)(void* stateTable,
                                          std::uint32_t stateSelector,
                                          std::uint32_t wordIndex,
                                          std::uint16_t value);
using FnRefreshPlayerAppearance = void (*)();

thread_local void* t_activeUpdateContext = nullptr;
std::atomic<FnSetAppearanceStateWord> g_setAppearanceStateWord{};
std::atomic<FnRefreshPlayerAppearance> g_refreshPlayerAppearance{};
std::atomic<int> g_hotkey{VK_F10};
std::atomic_bool g_hotkeyDown{};
std::atomic<ULONGLONG> g_lastHotkeyTime{};
std::atomic_bool g_refreshPending{};
std::atomic<ULONGLONG> g_restoreAfter{};
std::atomic<void*> g_refreshState{};
std::atomic<std::uint32_t> g_refreshMask{};
std::array<std::atomic<std::uint16_t>, kRefreshEntries.size()>
    g_savedValues{};
std::array<std::atomic<std::uint16_t>, kRefreshEntries.size()>
    g_temporaryValues{};

std::string NormalizeKeyName(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if (!std::isspace(ch)) {
      result.push_back(static_cast<char>(std::toupper(ch)));
    }
  }
  return result;
}

int ParseHotkey(std::string_view text) {
  const std::string key = NormalizeKeyName(text);
  if (key.size() >= 2 && key.front() == 'F') {
    int functionNumber = 0;
    const auto [end, error] = std::from_chars(
        key.data() + 1, key.data() + key.size(), functionNumber);
    if (error == std::errc{} && end == key.data() + key.size() &&
        functionNumber >= 1 && functionNumber <= 24) {
      return VK_F1 + functionNumber - 1;
    }
  }

  const bool hexadecimal = key.size() > 2 && key[0] == '0' && key[1] == 'X';
  const char* const first = key.data() + (hexadecimal ? 2 : 0);
  unsigned int virtualKey = 0;
  const auto [end, error] = std::from_chars(
      first, key.data() + key.size(), virtualKey, hexadecimal ? 16 : 10);
  if (error == std::errc{} && end == key.data() + key.size() &&
      virtualKey > 0 && virtualKey <= 0xFF) {
    return static_cast<int>(virtualKey);
  }
  return 0;
}

void LoadConfig(const Nioh3PluginInitializeParam* param) {
  const std::filesystem::path pluginsDirectory =
      (param != nullptr && param->plugins_dir != nullptr)
          ? std::filesystem::path(param->plugins_dir)
          : std::filesystem::path{};
  const std::filesystem::path configPath =
      pluginsDirectory / (std::string(kPluginName) + ".ini");

  char value[64]{};
  const DWORD length = GetPrivateProfileStringA(
      kConfigSection, kConfigKeyHotkey, "", value,
      static_cast<DWORD>(std::size(value)), configPath.string().c_str());
  if (length == 0) {
    WritePrivateProfileStringA(kConfigSection, kConfigKeyHotkey,
                               kDefaultHotkeyName,
                               configPath.string().c_str());
    g_hotkey.store(VK_F10, std::memory_order_release);
    _MESSAGE("%s: created %s (Hotkey=%s)", kPluginName,
             configPath.string().c_str(), kDefaultHotkeyName);
    return;
  }

  const int parsed = ParseHotkey(value);
  if (parsed == 0) {
    g_hotkey.store(VK_F10, std::memory_order_release);
    _MESSAGE("%s: invalid Hotkey=%s; using F10", kPluginName, value);
    return;
  }
  g_hotkey.store(parsed, std::memory_order_release);
  _MESSAGE("%s: Hotkey=%s (VK=0x%02X)", kPluginName, value, parsed);
}

void* ResolveLiveAppearanceStateTable() {
  const auto moduleBase =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  __try {
    void* const globalHolder = *reinterpret_cast<void* const*>(
        moduleBase + kAppearanceStateTableGlobalRva);
    if (globalHolder == nullptr) {
      return nullptr;
    }
    void* const tableOwner = *reinterpret_cast<void* const*>(globalHolder);
    if (tableOwner == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(tableOwner) +
                                   kAppearanceStateTableOffset);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

int LogRefreshException(EXCEPTION_POINTERS* exceptionPointers) {
  const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
  _MESSAGE("%s: refresh SEH code=0x%08lX address=%p", kPluginName,
           record->ExceptionCode, record->ExceptionAddress);
  return EXCEPTION_EXECUTE_HANDLER;
}

void TryRefreshAppearance() {
  const ULONGLONG now = GetTickCount64();
  const auto setState =
      g_setAppearanceStateWord.load(std::memory_order_acquire);
  const auto refresh =
      g_refreshPlayerAppearance.load(std::memory_order_acquire);

  if (g_refreshPending.load(std::memory_order_acquire)) {
    if (now < g_restoreAfter.load(std::memory_order_acquire)) {
      return;
    }
    const void* const state =
        g_refreshState.load(std::memory_order_acquire);
    const std::uint32_t mask = g_refreshMask.load(std::memory_order_acquire);
    if (state != nullptr && setState != nullptr && refresh != nullptr) {
      __try {
        for (std::size_t index = 0; index < kRefreshEntries.size(); ++index) {
          if ((mask & (1u << index)) != 0) {
            const auto& entry = kRefreshEntries[index];
            setState(const_cast<void*>(state), entry.stateSelector,
                     entry.stateWordIndex,
                     g_savedValues[index].load(std::memory_order_acquire));
          }
        }
        refresh();
      } __except (LogRefreshException(GetExceptionInformation())) {
      }
    }
    g_refreshPending.store(false, std::memory_order_release);
    return;
  }

  const int hotkey = g_hotkey.load(std::memory_order_acquire);
  // Do not use GetAsyncKeyState's low transition bit: another component in
  // the same process may consume it.  Track the high "currently down" bit
  // ourselves so the configured key remains reliable.
  const bool keyDown = (GetAsyncKeyState(hotkey) & 0x8000) != 0;
  const bool wasDown = g_hotkeyDown.exchange(keyDown, std::memory_order_acq_rel);
  if (!keyDown || wasDown) {
    return;
  }

  ULONGLONG last = g_lastHotkeyTime.load(std::memory_order_acquire);
  do {
    if (now - last < 250) {
      return;
    }
  } while (!g_lastHotkeyTime.compare_exchange_weak(
      last, now, std::memory_order_acq_rel, std::memory_order_acquire));

  void* const state = ResolveLiveAppearanceStateTable();
  if (state == nullptr || setState == nullptr || refresh == nullptr) {
    _MESSAGE("%s: refresh skipped; live state table is unavailable", kPluginName);
    return;
  }

  __try {
    std::uint32_t mask = 0;
    for (std::size_t index = 0; index < kRefreshEntries.size(); ++index) {
      const auto& entry = kRefreshEntries[index];
      const auto* const words = reinterpret_cast<const std::uint16_t*>(
          reinterpret_cast<std::uintptr_t>(state) + entry.stateSelector * 0x50);
      const std::uint16_t current = words[entry.stateWordIndex];
      // Weapons are always refreshed through a same-family fallback. A 0x0000
      // weapon naturally takes fallback -> 0x0000; an active transmog takes
      // fallback -> its exact saved value. Armor with no override has no work.
      const bool shouldRefresh = entry.isWeapon || current != entry.transitionValue;
      if (shouldRefresh) {
        g_savedValues[index].store(current, std::memory_order_release);
        const std::uint16_t temporary = entry.isWeapon
                                            ? (current != entry.transitionValue
                                                   ? entry.transitionValue
                                                   : entry.alternateWeaponFallbackValue)
                                            : entry.transitionValue;
        g_temporaryValues[index].store(temporary, std::memory_order_release);
        mask |= 1u << index;
      }
    }
    if (mask == 0) {
      return;
    }

    for (std::size_t index = 0; index < kRefreshEntries.size(); ++index) {
      if ((mask & (1u << index)) != 0) {
        const auto& entry = kRefreshEntries[index];
        setState(state, entry.stateSelector, entry.stateWordIndex,
                 g_temporaryValues[index].load(std::memory_order_acquire));
      }
    }
    refresh();
    g_refreshState.store(state, std::memory_order_release);
    g_refreshMask.store(mask, std::memory_order_release);
    g_restoreAfter.store(now + kRefreshGapMs, std::memory_order_release);
    g_refreshPending.store(true, std::memory_order_release);
  } __except (LogRefreshException(GetExceptionInformation())) {
  }
}

void* ResolveLivePlayerCharacterParent() {
  const auto moduleBase =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  __try {
    void* const playerTable = *reinterpret_cast<void* const*>(
        moduleBase + kPlayerObjectTableGlobalRva);
    if (playerTable == nullptr) {
      return nullptr;
    }
    void* const playerObject = *reinterpret_cast<void* const*>(
        reinterpret_cast<std::uintptr_t>(playerTable) +
        kPlayerObjectTableEntryOffset);
    if (playerObject == nullptr) {
      return nullptr;
    }
    return *reinterpret_cast<void* const*>(
        reinterpret_cast<std::uintptr_t>(playerObject) +
        kCharacterParentOffset);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool IsCurrentPlayerUpdateObject(void* updateObject) {
  if (updateObject == nullptr) {
    return false;
  }
  __try {
    void* const characterParent = *reinterpret_cast<void* const*>(
        reinterpret_cast<std::uintptr_t>(updateObject) +
        kCharacterParentOffset);
    return characterParent != nullptr &&
           characterParent == ResolveLivePlayerCharacterParent();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool InstallHooks() {
  REL::Relocation<FnSetAppearanceStateWord> setStateWord(REL::Pattern(
      kSetAppearanceStateWordRva,
      "48 63 C2 45 8B D8 45 0F B7 C1 41 8B D3 4C 8D 14 80", 0, 0, 0));
  REL::Relocation<FnRefreshPlayerAppearance> refreshPlayerAppearance(
      REL::Pattern(kRefreshPlayerAppearanceRva,
                   "40 53 48 81 EC B0 00 00 00 33 C9 E8 ? ? ? ? 48 85 C0",
                   0, 0, 0));
  REL::Relocation<FnUpdateContextThunk> updateContextThunk(REL::Pattern(
      kUpdateContextThunkRva,
      "48 83 EC 28 48 8B D1 48 8B 49 38 E8 ? ? ? ?", 0, 0, 0));
  REL::Relocation<FnUpdateSingleObject> updateSingleObject(REL::Pattern(
      kUpdateSingleObjectRva,
      "48 83 EC 28 E8 ? ? ? ? 84 C0 74 ? 48 8B 81 B0 03 00 00", 0, 0,
      0));
  if (setStateWord.get() == nullptr || refreshPlayerAppearance.get() == nullptr ||
      updateContextThunk.get() == nullptr || updateSingleObject.get() == nullptr) {
    _MESSAGE("%s: required functions were not found", kPluginName);
    return false;
  }

  g_setAppearanceStateWord.store(setStateWord.get(), std::memory_order_release);
  g_refreshPlayerAppearance.store(refreshPlayerAppearance.get(),
                                  std::memory_order_release);

  HookLambda(updateContextThunk.get(), [](void* updateContext) {
    void* const previousContext = t_activeUpdateContext;
    t_activeUpdateContext = updateContext;
    original(updateContext);
    t_activeUpdateContext = previousContext;
  });

  HookLambda(updateSingleObject.get(), [](void* updateObject) {
    if (t_activeUpdateContext != nullptr && IsCurrentPlayerUpdateObject(updateObject)) {
      TryRefreshAppearance();
    }
    original(updateObject);
  });
  return true;
}

}  // namespace

extern "C" __declspec(dllexport) bool nioh3_plugin_initialize(
    const Nioh3PluginInitializeParam* param) {
  LoadConfig(param);
  _MESSAGE("%s initialized for game version %s", kPluginName,
           param->game_version_string);
  return InstallHooks();
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    _MESSAGE("Initializing plugin: %s", kPluginName);
  }
  return TRUE;
}
