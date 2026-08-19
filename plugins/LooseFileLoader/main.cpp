#define NOMINMAX
#include <Windows.h>

#include <PluginAPI.h>
#include <BranchTrampoline.h>
#include <LogUtils.h>
#include <FileUtils.h>
#include <ConfigUtils.h>
#include <CommonUtils.h>

#include "Common.h"
#include "ModHooks.h"
#include "ModAssetManager.h"



using namespace LooseFileLoader;
// cmake --build build --config Release --target LooseFileLoader

extern "C" __declspec(dllexport) bool nioh3_plugin_initialize(const Nioh3PluginInitializeParam* param) {

    _MESSAGE("Plugin initialized");
    _MESSAGE("Game version: %s", param->game_version_string);
    _MESSAGE("Plugin dir: %s", param->plugins_dir);

    g_enableAssetLoadingLog = ConfigUtils::ReadInt(PLUGIN_NAME, "EnableAssetLoadingLog", 0) != 0;
    _MESSAGE("EnableAssetLoadingLog: %d", g_enableAssetLoadingLog ? 1 : 0);
    g_disableStreamingLoading = ConfigUtils::ReadInt(PLUGIN_NAME, "DisableStreamingLoading", 0) != 0;
    _MESSAGE("DisableStreamingLoading: %d", g_disableStreamingLoading ? 1 : 0);
    std::string filtersStr = ConfigUtils::ReadString(PLUGIN_NAME, "AssetLoggerFilters", "");
    if (!filtersStr.empty()) {
        const auto filters = CommonUtils::Split(filtersStr, ',');
        g_logFilters.clear();
        g_logFilters.insert(filters.begin(), filters.end());
    }
    _MESSAGE("AssetLoggerFilters: %s", filtersStr.c_str());

    g_modAssetManager.Build(param->game_root_dir);
    if (!InstallHooks()) {
        _MESSAGE("Failed to install LooseFileLoader hooks");
    }
    else {
        _MESSAGE("All hooks installed successfully.");
    }
    return true;
}

// Used by AppearanceRefreshHotkey immediately before it asks the game to
// recreate the player's appearance resources.
extern "C" __declspec(dllexport) void nioh3_loose_file_loader_rescan() {
    g_modAssetManager.Refresh();
}

// Used by AppearanceRefreshHotkey's toggle hotkey to enable/disable all mod
// overrides at runtime. Returns the new state: 1 = enabled, 0 = disabled.
extern "C" __declspec(dllexport) int nioh3_loose_file_loader_toggle() {
    const bool enabled = !g_modsEnabled.load(std::memory_order_acquire);
    g_modsEnabled.store(enabled, std::memory_order_release);
    _MESSAGE("Mod overrides %s", enabled ? "enabled" : "disabled");
    return enabled ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    // not compatible with asi loader
    if (reason == DLL_PROCESS_ATTACH) {
        _MESSAGE("Initializing plugin: %s, version: %d.%d.%d",
            PLUGIN_NAME, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH);
    }
    return TRUE;
}

