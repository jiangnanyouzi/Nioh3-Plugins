#define NOMINMAX
#include <Windows.h>

#include <HookUtils.h>
#include <LogUtils.h>
#include <PluginAPI.h>
#include <safetyhook.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

constexpr const char* kPluginName = "BodyAppearanceRefreshHotkey";
constexpr const char* kConfigSection = "BodyAppearanceRefreshHotkey";
constexpr const char* kConfigKeyHotkey = "Hotkey";
constexpr const char* kDefaultHotkeyName = "F9";

// The verified UI version-comparison site inside the Change Appearance UI
// update.  The game compares its cached UI resource version against the
// current version and, on mismatch, constructs and dispatches the ordinary
// body-appearance rebuild transaction (event 0x64 / subcode 0x33).
// Making the cache differ for exactly one comparison lets the game's own
// branch perform the refresh; this plugin never calls any game function.
// The site is resolved from a byte pattern at load time so that a game
// update which moves code does not require a rebuild.  The fallback RVA is
// the last verified one (game 2.0.0.2, 2026-08-21 build; 1.0.7.0 used
// 0x225373E).
constexpr std::uintptr_t kVersionCompareRva = 0x1C27776;
// Byte pattern of the verified site and the following branch:
//   cmp [rdi+0x8C],eax ; jne +0xA ; cmp [rdi+0x34],bpl ; je rel32
constexpr std::string_view kVersionCompareSignature =
    "39 87 8C 00 00 00 75 0A 40 38 6F 34 0F 84";
constexpr std::array<std::uint8_t, 6> kVersionComparePrefix{
    0x39, 0x87, 0x8C, 0x00, 0x00, 0x00};
// Guards verified at the comparison site (RCX = live UI object):
//   [rcx+0x4F0] == 3   (Change Appearance mode)
//   [rcx+0x10]  == 8   (page index)
constexpr std::uintptr_t kUiModeOffset = 0x4F0;
constexpr std::uintptr_t kUiPageOffset = 0x10;
constexpr std::uintptr_t kUiCachedVersionOffset = 0x8C;

constexpr ULONGLONG kMinimumRefreshGapMs = 250;

std::atomic<int> g_hotkey{VK_F9};
std::atomic_bool g_hotkeyDown{};
std::atomic<ULONGLONG> g_lastHotkeyTime{};

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
    _MESSAGE("%s: created %s (Hotkey=%s)", kPluginName,
             configPath.string().c_str(), kDefaultHotkeyName);
    return;
  }

  const int parsed = ParseHotkey(value);
  if (parsed == 0) {
    _MESSAGE("%s: invalid Hotkey=%s; using %s", kPluginName, value,
             kDefaultHotkeyName);
    return;
  }
  g_hotkey.store(parsed, std::memory_order_release);
  _MESSAGE("%s: Hotkey=%s (VK=0x%02X)", kPluginName, value, parsed);
}

bool ConsumeHotkeyPress() {
  const bool keyDown =
      (GetAsyncKeyState(g_hotkey.load(std::memory_order_acquire)) & 0x8000) != 0;
  const bool wasDown = g_hotkeyDown.exchange(keyDown, std::memory_order_acq_rel);
  if (!keyDown || wasDown) {
    return false;
  }

  const ULONGLONG now = GetTickCount64();
  ULONGLONG last = g_lastHotkeyTime.load(std::memory_order_acquire);
  do {
    if (now - last < kMinimumRefreshGapMs) {
      return false;
    }
  } while (!g_lastHotkeyTime.compare_exchange_weak(
      last, now, std::memory_order_acq_rel, std::memory_order_acquire));
  return true;
}

void OnUiVersionCompare(safetyhook::Context& ctx) {
  if (!ConsumeHotkeyPress()) {
    return;
  }

  const auto ui = reinterpret_cast<const std::uint8_t*>(ctx.rdi);
  if (ui == nullptr) {
    return;
  }

  __try {
    if (*reinterpret_cast<const std::uint32_t*>(ui + kUiModeOffset) != 3 ||
        *reinterpret_cast<const std::uint32_t*>(ui + kUiPageOffset) != 8) {
      return;
    }
    const std::uint32_t current = static_cast<std::uint32_t>(ctx.rax);
    auto* cached = reinterpret_cast<std::uint32_t*>(
        const_cast<std::uint8_t*>(ui) + kUiCachedVersionOffset);
    if (*cached == current) {
      *cached = current ^ 1;
      _MESSAGE("%s: forced body refresh (cached %lu -> %lu)", kPluginName,
               current, *cached);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

std::uintptr_t ResolveVersionCompareSite() {
  const auto moduleBase =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  if (moduleBase != 0) {
    if (const std::uintptr_t match =
            HookUtils::ScanIDAPattern(kVersionCompareSignature)) {
      _MESSAGE("%s: version-comparison site resolved by pattern at %p (RVA 0x%zX)",
               kPluginName, reinterpret_cast<void*>(match),
               match - moduleBase);
      return match;
    }
    _MESSAGE("%s: version-comparison pattern not found; using hardcoded RVA 0x%zX",
             kPluginName, kVersionCompareRva);
    return moduleBase + kVersionCompareRva;
  }
  return 0;
}

bool InstallHook() {
  const std::uintptr_t compareSite = ResolveVersionCompareSite();
  if (compareSite == 0) {
    _MESSAGE("%s: failed to resolve a version-comparison site", kPluginName);
    return false;
  }

  // Fail safe instead of mid-hooking unrelated code: the comparison prefix
  // bytes must be present at the resolved site.
  std::array<std::uint8_t, kVersionComparePrefix.size()> bytes{};
  if (!HookUtils::SafeReadBuf(compareSite, bytes.data(), bytes.size()) ||
      bytes != kVersionComparePrefix) {
    _MESSAGE("%s: unexpected bytes at %p (got %02X %02X %02X %02X %02X %02X); refusing to hook",
             kPluginName, reinterpret_cast<void*>(compareSite), bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return false;
  }

  static safetyhook::MidHook hook;
  hook = safetyhook::create_mid(reinterpret_cast<void*>(compareSite),
                                OnUiVersionCompare);
  if (!hook) {
    _MESSAGE("%s: failed to create mid hook at %p", kPluginName,
             reinterpret_cast<void*>(compareSite));
    return false;
  }
  return true;
}

}  // namespace

extern "C" __declspec(dllexport) bool nioh3_plugin_initialize(
    const Nioh3PluginInitializeParam* param) {
  LoadConfig(param);
  _MESSAGE("%s initialized for game version %s; open Grooming -> Change Appearance and press the configured hotkey to refresh the body model",
           kPluginName, param->game_version_string);
  return InstallHook();
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    _MESSAGE("Initializing plugin: %s", kPluginName);
  }
  return TRUE;
}