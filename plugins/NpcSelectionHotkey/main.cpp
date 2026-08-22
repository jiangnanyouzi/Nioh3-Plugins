#define NOMINMAX
#include <Windows.h>

#include <HookUtils.h>
#include <LogUtils.h>
#include <PluginAPI.h>
#include <Relocation.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

constexpr const char* kPluginName = "NpcSelectionHotkey";

// ---- Verified on game 2.0.0.2, session base 0x7FF6EF090000 (2026-08-21) ----

// Update dispatcher: called by UpdateContextThunk every frame on the player
// update thread. 0x8437C/0x84554 are occupied by AppearanceRefreshHotkey,
// so this plugin hooks the dispatcher instead (not occupied).
constexpr std::uintptr_t kUpdateDispatcherRva = 0x84394;
constexpr std::uintptr_t kCharacterParentOffset = 0x3A0;
constexpr std::uintptr_t kPlayerObjectTableGlobalRva = 0x473D318;
constexpr std::uintptr_t kPlayerObjectTableEntryOffset = 0x1338;

// NPC copy selection byte: PlayerData + 0x248D (equipment group 0),
// 0x248E (group 1, always written 0x00 by the game).
//   0x00 = original appearance, 0x67 = npc1, 0x81 = npc2, 0x9F = npc3.
constexpr std::uintptr_t kNpcSelectionByteOffset = 0x248D;

// Upstream live table holding the active selection value:
//   [[Nioh3.exe+0x4749830]] + 0x71AC + groupIndex
// (0x4749830 is the 2.0.0.2 relocation of the documented 0x4748820.)
constexpr std::uintptr_t kNpcSelectionTableGlobalRva = 0x4749830;
constexpr std::uintptr_t kNpcSelectionTableOffset = 0x71AC;

// Appearance state synchronizer (the UI copy transaction kernel):
//   +0x5725B0(PlayerData+0x590, groupSelector)
// Copies liveState records, 4 global flag bytes and the selection byte.
// It does NOT rebuild models by itself; the UI caller then drives the
// model request built at +0x2AA968.
constexpr std::uintptr_t kAppearanceSyncRva = 0x5725B0;
constexpr std::uintptr_t kAppearanceSyncPlayerDataOffset = 0x590;

// Argument-free refresh routine that runs on the player update thread.
constexpr std::uintptr_t kRefreshPlayerAppearanceRva = 0x2351450;

// ---- Hotkey ----

constexpr int kDefaultHotkey = VK_F6;
constexpr UINT_PTR kSelectionCycleSize = 4;
// Cycle: original -> npc1 -> npc2 -> npc3 (index 0 = original).
constexpr UINT8 kSelectionCycleValues[kSelectionCycleSize] = {0x00, 0x67, 0x81, 0x9F};
constexpr ULONGLONG kHotkeyDebounceMs = 250;

using FnUpdateDispatcher = void (*)(void* context, void* ctx2);
using FnAppearanceSync = void (*)(void* playerDataBase, std::uint32_t groupSelector);
using FnRefreshPlayerAppearance = void (*)();

std::atomic<FnAppearanceSync> g_appearanceSync{};
std::atomic<FnRefreshPlayerAppearance> g_refreshPlayerAppearance{};
std::atomic<int> g_hotkey{kDefaultHotkey};
std::atomic_bool g_hotkeyDown{};
std::atomic<ULONGLONG> g_lastHotkeyTime{};
std::atomic<UINT8> g_nextSelectionIndex{1};

void LoadConfig(const Nioh3PluginInitializeParam* param) {
  char value[32]{};
  const std::filesystem::path pluginsDirectory =
      (param != nullptr && param->plugins_dir != nullptr)
          ? std::filesystem::path(param->plugins_dir)
          : std::filesystem::path{};
  const std::filesystem::path configPath =
      pluginsDirectory / (std::string(kPluginName) + ".ini");
  const DWORD length = GetPrivateProfileStringA(
      kPluginName, "Hotkey", "", value,
      static_cast<DWORD>(std::size(value)), configPath.string().c_str());
  if (length == 0) {
    WritePrivateProfileStringA(kPluginName, "Hotkey", "F6",
                               configPath.string().c_str());
    _MESSAGE("%s: created Hotkey=F6 in %s", kPluginName,
             configPath.string().c_str());
    return;
  }
  if (value[0] == 'F') {
    const int functionNumber = value[1] >= '1' && value[1] <= '9'
                                   ? value[1] - '0'
                                   : 0;
    if (functionNumber >= 1 && functionNumber <= 9 && value[2] == '\0') {
      g_hotkey.store(VK_F1 + functionNumber - 1, std::memory_order_release);
    }
  } else if (value[0] != '\0') {
    const unsigned int vk = std::strtoul(value, nullptr, 0);
    if (vk > 0 && vk <= 0xFF) {
      g_hotkey.store(static_cast<int>(vk), std::memory_order_release);
    }
  }
  _MESSAGE("%s: Hotkey=%s (VK=0x%02X)", kPluginName, value,
           g_hotkey.load(std::memory_order_acquire));
}

void* ResolveLivePlayerData() {
  __try {
    const auto moduleBase =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
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
    void* const characterParent = *reinterpret_cast<void* const*>(
        reinterpret_cast<std::uintptr_t>(playerObject) +
        kCharacterParentOffset);
    return characterParent;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// Resolves [[Nioh3.exe+0x4749830]] + 0x71AC -> pointer to the selection byte.
void* ResolveLiveSelectionTable() {
  __try {
    const auto moduleBase =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* const holder = *reinterpret_cast<void* const*>(
        moduleBase + kNpcSelectionTableGlobalRva);
    if (holder == nullptr) {
      return nullptr;
    }
    void* const table = *reinterpret_cast<void* const*>(holder);
    if (table == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(table) + kNpcSelectionTableOffset);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

int LogNpcHotkeyException(EXCEPTION_POINTERS* exceptionPointers) {
  const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
  _MESSAGE("%s: SEH code=0x%08lX address=%p", kPluginName,
           record->ExceptionCode, record->ExceptionAddress);
  return EXCEPTION_EXECUTE_HANDLER;
}

void TryStepNpcSelection() {
  const auto sync = g_appearanceSync.load(std::memory_order_acquire);
  const auto refresh = g_refreshPlayerAppearance.load(std::memory_order_acquire);
  if (sync == nullptr || refresh == nullptr) {
    _MESSAGE("%s: functions not resolved", kPluginName);
    return;
  }

  const int hotkey = g_hotkey.load(std::memory_order_acquire);
  const bool keyDown = (GetAsyncKeyState(hotkey) & 0x8000) != 0;
  const bool wasDown = g_hotkeyDown.exchange(keyDown, std::memory_order_acq_rel);
  if (!keyDown || wasDown) {
    return;
  }
  const ULONGLONG now = GetTickCount64();
  ULONGLONG last = g_lastHotkeyTime.load(std::memory_order_acquire);
  do {
    if (now - last < kHotkeyDebounceMs) {
      return;
    }
  } while (!g_lastHotkeyTime.compare_exchange_weak(
      last, now, std::memory_order_acq_rel, std::memory_order_acquire));

  void* const playerData = ResolveLivePlayerData();
  void* const selectionTable = ResolveLiveSelectionTable();
  if (playerData == nullptr) {
    _MESSAGE("%s: player data unavailable", kPluginName);
    return;
  }
  if (selectionTable == nullptr) {
    _MESSAGE("%s: selection table unavailable", kPluginName);
    return;
  }

  const UINT8 target = kSelectionCycleValues[g_nextSelectionIndex.load(
      std::memory_order_acquire)];
  g_nextSelectionIndex.store(
      (g_nextSelectionIndex.load(std::memory_order_acquire) + 1) %
          kSelectionCycleSize,
      std::memory_order_release);

  _MESSAGE("%s: stepping to selection 0x%02X (playerData=%p, table=%p)", kPluginName,
           target, playerData, selectionTable);

  __try {
    // 1. Upstream table holds the active selection value.
    *reinterpret_cast<UINT8*>(selectionTable) = target;
    _MESSAGE("%s: wrote upstream selection byte 0x%02X", kPluginName, target);

    // 2. PlayerData selection byte (equipment group 0).
    *reinterpret_cast<UINT8*>(reinterpret_cast<std::uintptr_t>(playerData) +
                              kNpcSelectionByteOffset) = target;
    _MESSAGE("%s: wrote playerData selection byte 0x%02X", kPluginName, target);

    // 3. Run the appearance state synchronizer used by the UI copy
    //    transaction. Runs inside the player update thread.
    sync(reinterpret_cast<void*>(
             reinterpret_cast<std::uintptr_t>(playerData) +
             kAppearanceSyncPlayerDataOffset),
         0);
    _MESSAGE("%s: appearance sync returned", kPluginName);

    // 4. Argument-free refresh (must run on the player update thread).
    refresh();
    _MESSAGE("%s: player appearance refresh returned", kPluginName);
  } __except (LogNpcHotkeyException(GetExceptionInformation())) {
  }
}

bool InstallHooks() {
  // Appearance state synchronizer entry.
  REL::Relocation<FnAppearanceSync> appearanceSync(REL::Pattern(
      kAppearanceSyncRva,
      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 "
      "41 57 48 81 EC 10 02 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 "
      "00 02 00 00 45 33 E4",
      0, 0, 0));
  // Argument-free refresh routine.
  REL::Relocation<FnRefreshPlayerAppearance> refreshPlayerAppearance(
      REL::Pattern(kRefreshPlayerAppearanceRva,
                   "40 53 48 81 EC B0 00 00 00 33 C9 E8 ? ? ? ? 48 85 C0 74 3D "
                   "48 8B 98 A0 03 00 00 48 85 DB 74 31 33 D2 48 8D 4C 24 20 "
                   "41 B8 88 00 00 00 E8 ? ? ? ? 48 8D 4C 24 20 E8 ? ? ? ? "
                   "48 8B D0 48 8B CB E8 ? ? ? ? 48 8D 4C 24 20 E8 ? ? ? ? "
                   "48 81 C4 B0 00 00 00 5B C3 CC 48 8B C4 48 89 58 08",
                   0, 0, 0));
  // Update dispatcher: called by UpdateContextThunk every frame on the
  // player update thread. Not occupied by other plugins.
  REL::Relocation<FnUpdateDispatcher> updateDispatcher(REL::Pattern(
      kUpdateDispatcherRva,
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 4C 8B 01 48 8B F2 "
      "4D 85 C0 0F 84 90 01 00 00",
      0, 0, 0));
  if (appearanceSync.get() == nullptr || refreshPlayerAppearance.get() == nullptr ||
      updateDispatcher.get() == nullptr) {
    _MESSAGE("%s: required functions were not found", kPluginName);
    return false;
  }

  g_appearanceSync.store(appearanceSync.get(), std::memory_order_release);
  g_refreshPlayerAppearance.store(refreshPlayerAppearance.get(),
                                  std::memory_order_release);

  HookLambda(updateDispatcher.get(), [](void* context, void* ctx2) {
    original(context, ctx2);
    TryStepNpcSelection();
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