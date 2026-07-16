#include <mutex>
#define NOMINMAX
#include <Windows.h>
#include <cstdint>
#include <PluginAPI.h>
#include <LogUtils.h>
#include <FileUtils.h>
#include <HookUtils.h>
#include <GameType.h>
#include <CommonUtils.h>

#define PLUGIN_NAME "GameDumper"
#define PLUGIN_VERSION_MAJOR 1
#define PLUGIN_VERSION_MINOR 0
#define PLUGIN_VERSION_PATCH 0

namespace {

    // cooldown for triggering DumpAllItemData (milliseconds)
    static uint64_t s_lastDumpTime = 0;
    constexpr uint64_t kDumpCooldownMs = 5000;

    // 语义：近战武器 group -> type
    // 对应 IDA: sub_142393D40
    int32_t GetMeleeWeaponDisplayType(int weaponGroup) {
        switch (weaponGroup) {
            case 6409:  return 0;
            case 24575: return 1;
            case 28275: return 2;
            case 21589: return 3;
            case 20629: return 4;
            case 7191:  return 5;
            case 11583: return 6;
            case 29361: return 7;
            case 24091: return 8;
            case 636:   return 9;
            case 3375:  return 10;
            case 6102:  return 11;
            case 1254:  return 12;
            case 9554:  return 13;
            default:    return -1;
        }
    }

    // 语义：远程武器 group -> type
    // 对应 IDA: sub_142393E0C
    int32_t GetRangedWeaponDisplayType(int rangedGroup) {
        switch (rangedGroup) {
            case 59886: return 14;
            case 49224: return 15;
            case 51013: return 16;
            default:    return -1;
        }
    }

    // 语义：防具 group -> type
    // 这是 ConvertSlotTypeToArmorType_1423939DC 的逆向映射
    int32_t GetArmorDisplayType(int armorGroup) {
        switch (armorGroup) {
            case 3577:  return 17; // head
            case 11055: return 18; // chest
            case 1975:  return 19; // arms
            case 16443: return 20; // knee/waist
            case 2473:  return 21; // legs
            default:    return -1;
        }
    }

    // 可选：统一入口（按 item category 取 type）
    int32_t GetItemDisplayType(const ItemData* item) {
        if (!item) return -1;
        switch (item->category) {
            case ITEM_CATEGORY_WEAPON: return GetMeleeWeaponDisplayType(static_cast<int>(item->weaponType));
            case ITEM_CATEGORY_GUN: return GetRangedWeaponDisplayType(static_cast<int>(item->gunType));
            case ITEM_CATEGORY_ARMOR: return GetArmorDisplayType(static_cast<int>(item->armorType));
            default: return -1;
        }
    }

    typedef struct ResourceKey {
        uint32_t id;         // +0
        uint32_t subId;      // +4
        uint64_t packedTag;  // +8
        uint32_t extra;      // +16
        uint32_t field_14;   // +20
        uint32_t field_18;   // +24
        uint32_t field_1C;   // +28 (count field used for allocation sizing)
    } ResourceKey;
    static_assert(sizeof(ResourceKey) == 0x20);

    // 使用 DataArrayManager::GetAt() 按索引安全遍历
    void DumpItemData(const ItemData* itemData) {
        if (!itemData) {
            return;
        }
        if (itemData->itemId == 0) {
            return;
        }
        if (itemData->category != ITEM_CATEGORY_ARMOR && itemData->category != ITEM_CATEGORY_WEAPON && itemData->category != ITEM_CATEGORY_GUN) {
            return;
        }

        // GetName() 直接读 nameHash 查本地化表，线程安全
        _MESSAGE("%p,%u,%u,%u,%s,%s,%s",
            itemData,
            static_cast<uint32_t>(itemData->itemId),
            static_cast<uint32_t>(itemData->category),
            static_cast<uint32_t>(itemData->rarity),
            itemData->IsSamuraiItem() ? "Samurai" : "",
            itemData->IsNinjaItem() ? "Ninja" : "",
            itemData->GetName().c_str()
        );
    }

    void DumpAllItemData() {
        if (!(*g_resManager) || !(*g_resManager)->itemData) {
            _MESSAGE("DumpAllItemData: ResourceManager not ready, aborting");
            return;
        }
        auto* itemMgr = (*g_resManager)->itemData;
        uint32_t count = itemMgr->GetDataCount();
        if (count == 0) {
            _MESSAGE("DumpAllItemData: dataCount is 0, aborting");
            return;
        }
        _MESSAGE("DumpAllItemData starting, dataCount=%u...", count);
        for (uint32_t i = 0; i < count; i++) {
            DumpItemData(itemMgr->GetAt(i));
        }
        _MESSAGE("DumpAllItemData finished, iterated %u items", count);
    }


    void HookPlayerAppearance_EnqueueModelLoadByDesc() {
        //        auto entryAddr = HookUtils::ScanIDAPattern(
        //			"41 55 41 56 41 57 "
        //			"48 81 EC 18 01 00 00 48 8D 6C 24 20 "
        //			"48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 E0 00 00 00 4C 8B E9");
        //		if (entryAddr == 0) {
        //			_MESSAGE("PlayerAppearance_EnqueueModelLoadByDesc entry not found — enqueue capture disabled");
        //			return false;
        //		}
        using FnPlayerAppearance_EnqueueModelLoadByDesc = void* (*)(void*, void*, void*, ResourceKey*);
        REL::Relocation<FnPlayerAppearance_EnqueueModelLoadByDesc> PlayerAppearance_EnqueueModelLoadByDesc(REL::Pattern(0x4E908, "55 53 56 57 41 54 41 55 41 56 41 57 48 81 EC 18 01 00 00 48 8D 6C 24 20", 0, 0, 0));
        HookLambda(PlayerAppearance_EnqueueModelLoadByDesc.get(), [](void* a_this, void* a_param2, void* a_param3, ResourceKey* a_param4)->void* {
            // cooldown 防止频繁触发
            uint64_t now = GetTickCount64();
            if (now - s_lastDumpTime > kDumpCooldownMs) {
                s_lastDumpTime = now;
                if ((*g_resManager) && (*g_resManager)->itemData) {
                    DumpAllItemData();
                }
            }
            return original(a_this, a_param2, a_param3, a_param4);
        });
    }

    /*
     * Hook LookupEventHandler (nioh3.exe+0x820D8) — 事件哈希表查找函数
     */
    bool HookDispatchGameEvent() {
        HookLambda(LookupEventHandler.get(), [](void* a_gameEventManager, uint32_t a_eventId)->void* {
            if (a_eventId == 0xFFFFFFFF) {
                return original(a_gameEventManager, a_eventId);
            }
            constexpr uint32_t EVENT_ID_OPEN_EQUIPMENT_MENU = 285;
            if (a_eventId == EVENT_ID_OPEN_EQUIPMENT_MENU) {
                DumpAllItemData();
            }
            return original(a_gameEventManager, a_eventId);
        });
        return true;
    }

}

extern "C" __declspec(dllexport) bool nioh3_plugin_initialize(const Nioh3PluginInitializeParam * param) {
    _MESSAGE("Plugin initialized");
    _MESSAGE("Game version: %s", param->game_version_string);
    _MESSAGE("Plugin dir: %s", param->plugins_dir);

    HookDispatchGameEvent();
    HookPlayerAppearance_EnqueueModelLoadByDesc();

    _MESSAGE("GameDumper initialized, waiting for PlayerAppearance trigger...");
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        _MESSAGE("Initializing plugin: %s, version: %d.%d.%d", PLUGIN_NAME, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH);
    }
    return TRUE;
}
