#define NOMINMAX
#include <Windows.h>

#include <HookUtils.h>
#include <LogUtils.h>
#include <PluginAPI.h>
#include <Relocation.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr const char* kPluginName = "RandomBoss";
constexpr const char* kConfigSection = "RandomBoss";
constexpr const char* kConfigKeyTarget = "TargetId";
constexpr const char* kConfigKeySources = "SourceIds";
constexpr const char* kConfigKeyBlacklist = "Blacklist";
constexpr const char* kConfigKeyDiscover = "Discover";
constexpr const char* kConfigKeyCreateSwap = "CreateSwap";
constexpr const char* kConfigKeyCreateSources = "CreateSources";
constexpr const char* kConfigKeyCreateTarget = "CreateTarget";
// IdentitySwap (2026-09-18 CE 结论): rewrite the packed entity identity at the
// slot-identity writer. Opt-in; the tag must come from the canonical table.
constexpr const char* kConfigKeyIdentitySwap = "IdentitySwap";
constexpr const char* kConfigKeyIdentityFrom = "IdentityFrom";
constexpr const char* kConfigKeyIdentityTo = "IdentityTo";
constexpr const char* kConfigKeyIdentityTags = "IdentityTags";
// PairSwap (2026-09-18 CE 终局结论): a summon's enemy is decided by the PAIR of
// (request descriptor, parameter-table entry); rewriting only one side aborts
// the spawn silently, rewriting the key on both sides swaps the enemy.
constexpr const char* kConfigKeyPairSwap = "PairSwap";
constexpr const char* kConfigKeyPairFrom = "PairFromKey";
constexpr const char* kConfigKeyPairTo = "PairToKey";
constexpr const char* kConfigKeyPairTags = "PairTags";
constexpr const char* kConfigKeyPairRevert = "PairRevert";
constexpr const char* kConfigKeyPairMap = "PairMap";
// AssetTrace (diagnostic, 2026-09-19). Records which ktids pass through
// AssetIdManager::GetResIdByFileKtid and reports the top-16 per 10 s window.
// It answers the two questions a file-layer (ktid) redirect depends on:
//   1. does the asset load path even go through this function?
//   2. which ids does a summon actually resolve (and when)?
// Rationale: a failed swap test proved the *mechanism* works (redirect fired,
// target resolved, 0 misses) while the target file was never read from disk —
// i.e. the redirected id was not the one that produced the visible model.
constexpr const char* kConfigKeyAssetTrace = "AssetTrace";
// FactoryDiag (diagnostic, 2026-09-21). Installs the factory-REAL-entry hook
// (kFactoryEntryPattern) on its own, without enabling any marking, and logs a
// field snapshot for the first N target-key entities that pass through it.
// Purpose: the 一難/purple variant is NOT the runtime bit at entity+0xE8 (that
// was disproven by controlled write tests) and NOT the placement record's
// flags (the same record yields purple on some loads and plain on others).
// This hook is the one point that sees every creation, so a single map load
// yields the field comparison that pins the real determinant down.
constexpr const char* kConfigKeyFactoryDiag = "FactoryDiag";
// Source-level map swap (see kMapPlacementKeyPattern / MapBossHookStub).
constexpr const char* kConfigKeyMapBoss = "MapBoss";
constexpr const char* kConfigKeyMapSources = "MapSources";
constexpr const char* kConfigKeyMapPool = "MapPool";
constexpr const char* kConfigKeyMapRandomMode = "MapRandomMode";

// Default map SOURCE set = every Jailer Oni variant (EnemyIdCollector_names.csv,
// 18 entries: 0x1361D 0x1A2E6 0x232D5 0x26C12 0x304FC 0x3CF7D 0x5D5B8 0x99943
// 0xA6DDA 0xB77A6 0xBC496 0xC0618 0xD0779 0xE1ABA 0xE6F42 0xE8E25 0xF2527
// 0xF3566). "Every Jailer Oni on the map becomes the configured boss."
constexpr std::uint32_t kDefaultMapSources[] = {
    // Jailer Oni (18 variants)
    0x01361D, 0x01A2E6, 0x0232D5, 0x026C12, 0x0304FC, 0x03CF7D,
    0x05D5B8, 0x099943, 0x0A6DDA, 0x0B77A6, 0x0BC496, 0x0C0618,
    0x0D0779, 0x0E1ABA, 0x0E6F42, 0x0E8E25, 0x0F2527, 0x0F3566,
    // Shunobon / 朱盆 (8 variants) — user 2026-09-20: "朱盆也弄成随机的"
    0x0300B8, 0x053347, 0x05E52D, 0x060E01, 0x097AD6, 0x0A697F,
    0x0DF255, 0x0E0BB4};
// Default map TARGET pool: random per placement (MapRandomMode=1 keeps each
// placement stable, so the same spot always yields the same boss). 0xA263C
// Gozuki is the only target verified end-to-end; prune any entry that crashes,
// or force the NG++ tier with MapRank.
constexpr std::uint32_t kDefaultMapPool[] = {
    0x0A263C, 0x0782F6, 0x0C76B4, 0x040A3B,
    0x05AAF9, 0x041DB6, 0x093F79, 0x0D1F46};

// Component factory core (v2.0.1.0 RVA 0x5FC500). At function entry r8d is
// the spawn parameter id; the constructor receives it as r8. Swapping r8
// here rewires the battle-data component. Whether that alone changes the
// visible model is exactly what this test plugin determines.
// NOTE: the pattern starts at function+6, NOT the entry. The entry bytes
// are single-use: whichever plugin hooks factory core first overwrites them
// (safetyhook inline patch), destroying any entry-anchored pattern for
// every other plugin. The +6 tail survives cohabitation and is still unique.
constexpr const char* kFactoryCorePattern =
    "48 8B D9 4D 85 C9 74 22 41 8B 41 68 D1 E8 24 01";
// Assembly main (v2.0.1.0 RVA 0x247960). Entry: RCX = output entity, RDX =
// source request object. Discover mode scans [rdx, rdx+0x400) for dword
// values inside the enemy-id range and logs (offset, value) pairs, so the
// identity field offset calibrates itself from a single load storm.
constexpr const char* kAssemblyMainPattern =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ? ? ? ? "
    "8B 87 70 01 00 00 89 83 70 01 00 00";
// Component factory REAL entry (v2.0.2.0 RVA 0x5FC570). At entry RCX =
// handler object, RDX = spawn entity, R8D = catalog key, R9D = category.
// Multi-capture evidence (2026-09-18): the boss-creation call arrives with
// RCX = handler and RDX = entity as two DISTINCT heap objects (e.g.
// 0x21C07903B10 / 0x21C078F4520), R8D = key; per-frame ensures pass RCX =
// stack / RDX = 0, so a both-heap check isolates the creation moment.
// Patching [entity] (embedded id = key<<4) + [handler+8] (key) + R8 here is
// consumed downstream by ensure/model pipeline — the pre-ensure identity
// swap. Verified ids: 武者 0x93457 / 忍者 0x1B6CC / 狱卒鬼 0xBC496.
constexpr const char* kFactoryEntryPattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 41 8B D9";

// Menu-summon enqueue entry (RVA 0x149AC4 in the 2026-09-18 build, located
// from the fac15 capture's outermost frame). This is THE pre-load swap point:
//   - args: RCX=arg1, RDX=entity, R8=arg3, R9=arg4
//   - at +0x57 it does  r12 = [RDX+0xF8]  -> the identity record
//   - at +0x67 it reads [r12+0x974]
//   - far below (+0xA77) it enqueues the asset-load job (call 0xDE03A4C)
// Patching [identityRecord+0x04] (the catalog key) HERE lands before the
// enqueue, so the worker loads the TARGET's assets and the whole downstream
// flow (orchestrator -> builder -> Caller A -> factory) reads one consistent
// key. Shrine/world respawns bypass this function entirely (loading stack
// diff: respawn loads enter via 0x3635F2/0x3DF407/0x3B2141, never here), so
// no scene gate and no storm risk.
constexpr const char* kMenuEnqueuePattern =
    "48 8B C4 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 A0 48 81 EC 60 01 "
    "00 00 0F 29 70 B8 48 8D B1 60 10 00 00 0F 29 78 A8";

// SummonOrchestrator (found upstream of the factory entry via the boot-5400
// frozen stack). Entry args: RCX = spawn object, DL = flag. It iterates the
// object's +0x238 array of 24-byte items ([item] = spawn-def pointer) and
// drives BOTH the asset load and the factory call — so patching the key
// inside the spawn-def HERE lands before the load decision (the factory
// entry is downstream and too late, evidenced by the double-summon test).
// Hook target = pattern match - 0x18 (the lea rbp/sub rsp/lea rsi block is
// 0x18 into the function; the entry mov rax,rsp is verified before hooking).
constexpr const char* kOrchestratorPattern =
    "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 "
    "48 8D 68 A1 48 81 EC 00 01 00 00 0F 29 70 C8 48 8D B1 38 02 00 00";

// AssetIdManager::GetResIdByFileKtid(uint32 ktid) -> uint32 resId.
// Every asset the game loads resolves through this single mapping; redirecting
// deserialize, entity build) consume the target asset. LooseFileLoader only
// CALLS this function (never hooks it), so the entry bytes are intact and the
// inline hook coexists with it.
// Pattern source: LooseFileLoader Common.h line 85 (GetResIdByFileKtid).
// NOTE: line 91's "E8 ? ? ? ? 49 8D AF ? ? ? ? 8B D0" is GetFileKtIdFromRes
// (GameAsset* arg) — hooking THAT with a u32 arg truncates the pointer and
// crashes on first call (lived experience, 22:30 boot). Do not confuse them.
constexpr const char* kGetResIdByFileKtidPattern =
    "E8 ? ? ? ? 8B D0 48 8B CB E8 ? ? ? ? BB";

// Live catalog query entry (RVA 0x4EEF44 in the 2026-09-18 build), located by
// hardware-probing the hot param catalog (2045 slots, 0x398 stride — the live
// twin of the dead 0x3A4360). Signature verified by breakpoint captures:
//   - arg1 RCX = context object (fed to 0x3A42E0 -> catalog object)
//   - arg3 R8  = &embeddedId  (key<<4, read at +0x37: mov edi,[r8]; shr edi,04)
//   - XMM1 = float default
// Called ~625/s per on-screen enemy for params AND once per summon with the
// picked boss key (0x93457 for 武者, captured live). Rewriting *[R8] BEFORE
// the query resolves makes every consumer downstream (params, and the asset
// manifest if it derives from the same embedded id) read the TARGET.
const char* kCatalogQueryPattern =
    "48 89 5C 24 10 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8B EC 48 83 "
    "EC 70";

// Entity-slot identity writer (RVA 0x49A62C in the 2026-09-18 build; 17-byte
// signature verified unique in-module by CE). ABI captured live:
//   RCX = slot (0xA0 bytes), RDX = packed identity {low32 = key<<4,
//   high32 = type tag}, R8B = flag byte; returns the slot in RAX.
// This is the last moment before the identity lands in the slot, so rewriting
// RDX makes every downstream consumer (params, assets, spawn validation) read
// one consistent value. A tag that does not belong to the key aborts the spawn
// silently (controlled A/B, 2026-09-18) — hence the canonical tag table.
constexpr const char* kIdentityInitPattern =
    "48 89 5C 24 08 57 48 83 EC 20 48 89 51 08 41 8A F8";

// Map placement-record key read (v2.0.2.0 RVA 0x679895; verified UNIQUE
// in-module with a CE AOB scan on 2026-09-20). The site sits inside the
// function at RVA 0x67985C, which instantiates ONE world entity from ONE
// per-instance placement record:
//   RAX = record { +0x00 instanceId, +0x04 key, +0x08 flags }
//   mov r14d,[rax+04]        <-- hook site (this pattern)
//   test rbx,rbx / je ...    <-- displaced bytes 5..7 (resume = site + 7)
//   ...                     0x67991B call 0x2BFA04 -> (tail) 0x2BFB88
//                           -> 0x2BFD80 -> decision fn 0x2C19C4
// Rewriting R14 here is a SOURCE-LEVEL swap: the descriptor, the params lookup,
// the asset request and the model all see the rewritten key, because they are
// all built downstream of this load. Proven 2026-09-20: rewriting the records
// of 10 Jailer Oni placements to 0xA263C made the map spawn Gozuki (CE A/B +
// user verification, and a logging breakpoint showed R14 = 0x000A263C).
// Runtime-level identity patches (factory/catalog/identity-slot) never changed
// the model — this one does, because it is upstream of construction.
constexpr const char* kMapPlacementKeyPattern =
    "44 8B 70 04 48 85 DB 74 1A 8B 03 C1 E8 04";

// Per-frame 一難 (purple) flag writer, RVA 0x27DB6A; the hook lands 14 bytes in,
// on `mov [rax+0E8h],cl`. CE-verified 2026-09-20 on a live map with a hardware
// write breakpoint (1300+ hits per entity in seconds, all from this one site):
//
//   mov cl,[rdi+0x1940] / shr cl,6 / not cl / and cl,sil
//   mov [rax+0xE8],cl          <- only the LOW BYTE of the dword
//
// RAX is the entity. The 一難 bit is 0x100, i.e. byte +0xE9, which this writer
// never touches — that is why the mark is sticky and no re-assert is needed.
// Because the instruction runs for every active entity on every frame it also
// enumerates entities through spawn paths the placement hook never sees
// (streamed-in, shrine respawn, far pre-create), which is the whole reason this
// hook exists: it replaces any form of memory scanning.
constexpr const char* kMapPurpleFlagPattern =
    "8A 8F 40 19 00 00 C0 E9 06 F6 D1 40 22 CE 88 88 E8 00 00 00";
constexpr std::uintptr_t kMapPurpleHookOffset = 14;
constexpr std::uintptr_t kMapPurpleDisplaced = 6;

// Training-room roster injection (RosterSwap, default on). Mechanism proven
// 2026-09-18: the summon pipeline validates the requested key against the
// room roster {武者,忍者,狱卒鬼}; swapping the key anywhere downstream makes
// the spawn abort silently. The roster object holds all three keys packed
// within ~0x400 bytes (verified: 0xBC496/0x93457/0x1B6CC in one object,
// while the catalog slot table spreads the same keys >0xA000 apart — the
// tight window discriminates roster from catalog). Sweeping memory for the
// trio and rewriting the 武者 slot to the target makes the game itself
// summon the target: menu/assets/params/validation all self-consistent.
constexpr std::uint32_t kRosterTrio[] = {0x93457, 0x1B6CC, 0xBC496};
constexpr std::uint32_t kRosterPatchSlot = 0x93457;  // 武者 slot -> target
// 2026-09-18 CE 实测: BC496@+0x258 与 93457@+0x458 间距正好 0x400，
// 且 roster 所在堆区常达数百 MB —— 窗口与区尺寸过滤都曾把真目标排除。
constexpr std::uint32_t kRosterWindowBytes = 0x800;
constexpr std::uintptr_t kRosterScanMaxRegion = 0;  // 0 = 不限制区大小
// Default target: Gozuki (牛头鬼) from the live-verified enemy catalog.
constexpr std::uint32_t kDefaultTargetId = 0xA263C;

// The generic battle-data component (id 100) must never be swapped: every
// entity's HP container creation flows through it.
constexpr std::uint32_t kDefaultBlacklist[] = {0x64};
// CreateSwap default sources: the three training-room bosses whose keys were
// verified by multi-capture on 2026-09-18 (武者/忍者/狱卒鬼).
constexpr std::uint32_t kDefaultCreateSources[] = {0x93457, 0x1B6CC, 0xBC496};
constexpr std::size_t kMaxListEntries = 256;

// Discover mode: plausible enemy-id dword range (catalog keys are 20-bit).
constexpr std::uint32_t kIdRangeMin = 0x1000;
constexpr std::uint32_t kIdRangeMax = 0xFFFFF;
// RDX is the big spawn-director object (fields observed up to +0x44D0);
// the identity dword is beyond the first KB, so scan the whole thing.
// Cost: 0x4800/4 dwords per assembly-main call — trivial even in a storm.
constexpr std::uint32_t kDiscoverScanBytes = 0x4800;
constexpr std::uint32_t kDiscoverMaxHits = 65536;

using FnFactoryCore = void (*)(void* arg1, void* arg2, std::uint32_t spawnId,
                               void* paramObject);
using FnOrchestrator = void (*)(void* spawnObject, unsigned char flag);
using FnMenuEnqueue = void (*)(void* arg1, void* entity, void* arg3,
                               void* arg4);
using FnCatalogQuery = void (*)(void* arg1, void* arg2, void* keyPtr,
                                void* arg4);
// Factory REAL entry. RCX = handler object, RDX = spawn entity, R8D = catalog
// key, R9D = category. Per-frame "ensure" passes pass RDX = 0, so a non-null
// entity argument isolates the actual creation/ensure moment. Unlike the
// placement-record hook (RVA 0x679895), which only sees the subset of spawns
// that read a placement record, this entry is reached for every entity that is
// built or ensured - which is what the 一難 marking needs for full coverage.
using FnFactoryEntry = void (*)(void* handler, void* entity, std::uint32_t key,
                                std::uint32_t category);

// Register-capture bridge for the menu-enqueue hook (see HookStub.asm).
// (The original RDI/R13 menu-summon gate is superseded by the pre-load hook;
// the stub still works as a plain detour and is kept.)
extern "C" std::uint64_t g_hookRdi;
extern "C" std::uint64_t g_hookR13;
extern "C" std::uint64_t g_hookRsp;
extern "C" std::uint64_t g_hookRbp;
extern "C" std::uint64_t g_hookR8;
extern "C" std::uint64_t g_hookR9;
extern "C" void CreateSwapHookBody(void* arg1, void* entity, void* arg3,
                                   void* arg4);
extern "C" void CatalogQueryHookBody(void* arg1, void* arg2, void* keyPtr,
                                     void* arg4);
std::uint64_t g_hookRdi = 0;
std::uint64_t g_hookR13 = 0;
std::uint64_t g_hookRsp = 0;
std::uint64_t g_hookRbp = 0;
std::uint64_t g_hookR8 = 0;
std::uint64_t g_hookR9 = 0;
static FnMenuEnqueue g_entryOriginal = nullptr;
static FnCatalogQuery g_catalogOriginal = nullptr;
extern "C" void CaptureHookContext();
extern "C" void CaptureCatalogContext();

// Map source-level swap (see kMapPlacementKeyPattern). MapBossHookStub is the
// asm half: it saves the volatile registers, calls MapBossHookBody with the
// placement record, writes the returned key into R14D, replays the displaced
// instructions and resumes at g_mapResume. g_mapResume is written by the
// installer (hook site + 7) and read by the stub, so it must have C linkage.
extern "C" void MapBossHookStub();
extern "C" std::uint64_t g_mapResume = 0;

// Purple (一難) flag hook, see kMapPurpleFlagPattern. Same contract as the map
// hook: the asm stub names the register the engine already has the entity in,
// MapPurpleHookBody does the work, then the displaced instruction is replayed
// and control resumes at g_mapPurpleResume (hook site + kMapPurpleDisplaced).
extern "C" void MapPurpleHookStub();
extern "C" std::uint64_t g_mapPurpleResume = 0;
std::atomic_bool g_mapPurpleHooked{false};
safetyhook::InlineHook g_mapPurpleHook;

// Factory-entry 一難 diagnostics (used by MapPurpleFactoryEntryBody below).
// Kept small: one capped log line per target entity that passes through the
// creation hook, so the in-game result ("which of these came out purple") can
// be matched against the field snapshot offline.
std::atomic<std::uint64_t> g_fepLogged{0};
std::atomic<std::uint64_t> g_fepSeen{0};
constexpr std::uint64_t kFepLogMax = 256;
// Counts successful 一難 marks so their log lines stay capped (see
// MapPurpleMark). Distinct from g_fepLogged, which counts factory-entry hits.
std::atomic<std::uint32_t> g_mapPurpleMarkLogs{0};

// Factory-REAL-entry body (full-coverage creation hook). Declared here so the
// installer, which is defined much further down, can name it. C linkage keeps
// the name stable for the hook lambda.
extern "C" void MapPurpleFactoryEntryBody(void* handler, void* entity,
                                         std::uint32_t key,
                                         std::uint32_t category);

std::atomic_bool g_mapBoss{true};   // source-level map swap (default on)
std::atomic_bool g_mapHooked{false};
std::atomic<std::uint32_t> g_mapRandomMode{1};  // 1 = stable per placement,
                                                // 2 = re-roll every spawn
extern std::atomic<std::uint32_t> g_mapRankMode;  // defined below (MapRank)
extern std::atomic<std::uint32_t> g_mapRankEvery;  // defined below (MapRank)
extern std::atomic<std::uint32_t> g_mapPurple;  // defined below (purple / 一难)
std::atomic<std::uint32_t> g_mapSources[kMaxListEntries];
std::atomic<std::size_t> g_mapSourceCount{0};
std::atomic<std::uint32_t> g_mapPool[kMaxListEntries];
std::atomic<std::size_t> g_mapPoolCount{0};
std::atomic<std::uint64_t> g_mapSwapCount{0};
// The hook writes the target key THROUGH to the placement record (see
// MapBossHookBody), so after the first swap the record no longer holds the
// source key. This tiny memo keeps the original key per record so
// MapRandomMode=2 can still re-roll on later instantiations. Keyed by the low
// 32 bits of the record address; a collision only costs one wrong re-roll.
constexpr std::size_t kMapMemoSlots = 256;
std::atomic<std::uint32_t> g_mapMemoRecord[kMapMemoSlots];
std::atomic<std::uint32_t> g_mapMemoSource[kMapMemoSlots];
std::atomic<std::uint64_t> g_mapSweepHits{0};
// Per-source-key pools: the ini line "MapPool_<SOURCEHEX>=<list>" gives that one
// enemy its own target list, so 朱盆 and 狱卒鬼 no longer have to share a pool
// (the user's 2026-09-20 request). Looked up by the ORIGINAL placement key.
constexpr std::size_t kMapPoolSlots = 32;
constexpr std::size_t kMapPoolMaxKeys = 32;
struct MapKeyPool {
  std::atomic<std::uint32_t> source{0};  // 0 = free slot
  std::atomic<std::size_t> count{0};
  std::atomic<std::uint32_t> keys[kMapPoolMaxKeys];
};
MapKeyPool g_mapKeyPools[kMapPoolSlots];
std::atomic<std::size_t> g_mapKeyPoolCount{0};
// One-shot source-table randomisation (see MapRandomizeTableOnce). MapHook=0
// (the default) means the plugin never patches code at all: the placement table
// the engine built at startup is rewritten once, then the game runs untouched.
std::atomic_bool g_mapHookEnabled{false};
std::atomic_bool g_mapTableDone{false};
std::atomic<std::uint64_t> g_mapTableHits{0};
safetyhook::InlineHook g_mapHook;
static safetyhook::InlineHook g_entryHook;
static safetyhook::InlineHook g_catalogHook;
using FnAssemblyMain = void (*)(void* arg1, void* arg2);
using FnGetResIdByFileKtid = std::uint32_t (*)(void* assetIdManager,
                                               std::uint32_t ktid);

struct DiscoverHit {
  std::uint32_t offset;
  std::uint32_t value;
};
DiscoverHit g_discoverHits[kDiscoverMaxHits];
std::atomic<std::uint32_t> g_discoverCount{0};
std::atomic_bool g_discoverOverflow{false};

std::atomic<std::uint32_t> g_targetId{kDefaultTargetId};
// Variant flags the placement record must carry for the target enemy to come
// out as the powered-up (紫皮 / 一難) variant. VERIFIED 2026-09-21 against the
// engine's own placement: the native purple Gozuki's record (id=CC15) reads
// 0x011E3701, while every ordinary placement for the SAME enemy reads
// 0x011F3701 - the single difference being bit 0x10000.
//
// Cross-checked across every flag value this project has ever observed
// (11E3701 11F3701 1F3701 1E3701 13701 13601 1E3601 11E3601 1013601):
//   bit24 set AND bit16 clear  ->  purple-capable  (11E3701, 11E3601)
//   bit16 set                  ->  ordinary        (11F3701, 1F3701, 13701)
// Bit 24 distinguished the engine's own placements in earlier rounds too
// (only 10 of 306 records carry it).
std::atomic<std::uint32_t> g_targetFlags{0x011E3701u};
// Counts record-flag rewrites (ApplyTargetFlags); capped logging uses it.
std::atomic<std::uint32_t> g_mapPurpleFlagWrites{0};
// Empty list = swap every id except the blacklist.
std::atomic<std::uint32_t> g_sourceIds[kMaxListEntries];
std::atomic<std::size_t> g_sourceCount{0};
std::atomic_bool g_swapAll{true};
std::atomic<std::uint32_t> g_blacklist[kMaxListEntries];
std::atomic<std::size_t> g_blacklistCount{0};
std::atomic<std::uint64_t> g_swapCount{0};
std::atomic<std::uint64_t> g_seenCount{0};
std::atomic<std::uint64_t> g_assetSwapCount{0};
// Redirects that could not be honoured because the TARGET ktid is not in the
// current scene's asset table (GetResIdByFileKtid answers 0xFFFFFFFF there).
std::atomic<std::uint64_t> g_assetMissCount{0};
// AssetTrace accumulator (see kConfigKeyAssetTrace). 512 slots, multiplicative
// hash; a collision only blurs the histogram, never the call total.
constexpr std::size_t kAssetTraceSlots = 512;
std::atomic<std::uint32_t> g_assetTraceKey[kAssetTraceSlots];
std::atomic<std::uint32_t> g_assetTraceHits[kAssetTraceSlots];
std::atomic<std::uint64_t> g_assetTraceTotal{0};
std::atomic_bool g_assetTrace{false};
// FactoryDiag: installs the factory-entry hook for sampling only (see
// kConfigKeyFactoryDiag). Independent of MapPurple so a load can be sampled
// without enabling the marking behaviour.
std::atomic_bool g_factoryDiag{false};
std::atomic_bool g_discoverMode{false};
std::atomic_bool g_factorySwap{false};  // r8 battle-data swap (实验性,默认关)
std::atomic_bool g_assetSwap{true};     // KTID 归档重定向(v3 主线,默认开)
std::atomic_bool g_createSwap{false};   // creation-moment identity swap (上游,默认关)
std::atomic_bool g_catalogSwap{true};   // catalog-query key rewrite (默认开)
std::atomic_bool g_rosterSwap{true};    // training-roster key injection (默认开)
std::atomic<std::uint32_t> g_createSources[kMaxListEntries];
std::atomic<std::size_t> g_createSourceCount{0};
std::atomic<std::uint32_t> g_createTarget{kDefaultTargetId};
std::atomic<std::uint64_t> g_createSwapCount{0};
// 1 = full identity swap (entity+handler+R8). 2 = entity-only: leave the
// key/descriptor path untouched (the target's descriptor may not be loaded
// in-session — a full swap aborts the spawn, observed 10:56) and patch only
// [entity+0x00]; whether the model track follows the entity id is exactly
// what mode 2 measures.
std::atomic<std::uint32_t> g_createMode{2};
// Sliding-window rate cap for createSwap (see hook body).
std::atomic_uint64_t g_createSwapTimes[8]{};
std::atomic<std::uint32_t> g_createSwapSlot{0};
std::atomic<std::uint32_t> g_createMaxPerMinute{4};
std::filesystem::path g_discoverPath = L"RandomBoss_discover.csv";
std::filesystem::path g_configPath;
FILETIME g_configMtime{};
// KTID swap map; hot-reloaded when the ini mtime changes. Readers (game
// threads) take a snapshot via atomic shared_ptr — no locks in the hook.
std::atomic<std::shared_ptr<const std::map<std::uint32_t, std::uint32_t>>>
    g_ktidSwapMap{};
// IdentitySwap state: sources whose spawns get their identity rewritten, the
// single target key, and the canonical key -> type-tag table.
std::atomic_bool g_identitySwap{false};
std::atomic<std::uint32_t> g_identityFrom[kMaxListEntries];
std::atomic<std::size_t> g_identityFromCount{0};
std::atomic<std::uint32_t> g_identityTo{0};
std::atomic<std::uint64_t> g_identitySwapCount{0};
std::atomic<std::shared_ptr<const std::map<std::uint32_t, std::uint32_t>>>
    g_identityTags{};
// PairSwap state (see PairScanShard).
std::atomic_bool g_pairSwap{false};
std::atomic<std::uint32_t> g_pairFromKey{0};
std::atomic<std::uint32_t> g_pairToKey{0};
// Optional tag whitelist: when non-empty, only these tags are matched (and they
// may lie outside the default 0x90000..0x9FFFF band). Empty = the default band.
std::atomic<std::uint32_t> g_pairTags[kMaxListEntries];
std::atomic<std::size_t> g_pairTagCount{0};
// Reversible patching (2026-09-19): a tag's key lives in ONE global record, so
// every patch is visible in every scene. Remembering the sites lets us restore
// the original key as soon as the swap is switched off, which is what makes the
// feature usable for "only in the training room" workflows.
std::atomic_bool g_pairRevert{true};
// PairMap: tag -> target key. One entry per summon button (a roster tag).
// Unlike the older PairFromKey/PairToKey form this does not need to know the
// enemy's CURRENT key: the tag identifies the button, so the same config works
// whatever the scene considers canonical. Example (training room):
//   PairMap=0x946AD=0xA263C,0x92EA6=0x782F6,0x93EC7=0x41DB6,0x93077=0xC76B4
constexpr std::size_t kPairMapMax = 16;
std::atomic<std::uint32_t> g_pairMapTag[kPairMapMax];
std::atomic<std::uint32_t> g_pairMapKey[kPairMapMax];
std::atomic<std::size_t> g_pairMapCount{0};
// Bumped on every config load so background threads can tell that the mapping
// may have changed and therefore need to undo their previous patches first.
std::atomic<std::uint64_t> g_configGeneration{0};
// Patched sites with their ORIGINAL key values, so a revert is exact even with
// several different targets in play.
constexpr std::size_t kPairPatchSlots = 256;
std::atomic<std::uintptr_t> g_pairPatchedAddr[kPairPatchSlots];
std::atomic<std::uint32_t> g_pairPatchedOrig[kPairPatchSlots];
std::atomic<std::size_t> g_pairPatchedCount{0};
std::atomic<std::uint64_t> g_pairSwapCount{0};

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

// Parses "0xSRC=0xDST,0xSRC2=0xDST2" into a map. Whitespace tolerant.
std::map<std::uint32_t, std::uint32_t> ParseKtidPairs(std::string_view text);

// GetPrivateProfileStringA truncates a comma-separated value at its first entry
// on this system — the quirk already documented for CreateSources further down.
// For the map lists that silently reduced MapPool to a single key (every Jailer
// Oni came out as the same boss) and MapSources to a single variant (Shunobon
// "did not react"), both reported by the user on 2026-09-20. Read the raw line
// from the file instead; the LAST occurrence wins, so the stale duplicate keys
// this plugin writes cannot shadow the live value either.
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
  g_mapKeyPoolCount.store(slots, std::memory_order_release);
}

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
    constexpr std::size_t kCount = std::size(kDefaultMapSources);
    g_mapSourceCount.store(kCount, std::memory_order_release);
    for (std::size_t i = 0; i < kCount; ++i) {
      g_mapSources[i].store(kDefaultMapSources[i], std::memory_order_release);
    }
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
           "MapRandomMode=%u MapHook=%d MapPurple=%d FactoryDiag=%d",
           kPluginName, g_mapBoss.load(std::memory_order_acquire) ? 1 : 0,
           g_mapSourceCount.load(std::memory_order_acquire),
           g_mapPoolCount.load(std::memory_order_acquire),
           g_mapKeyPoolCount.load(std::memory_order_acquire),
           g_mapRandomMode.load(std::memory_order_acquire),
           g_mapHookEnabled.load(std::memory_order_acquire) ? 1 : 0,
           g_mapPurple.load(std::memory_order_acquire) ? 1 : 0,
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

bool IsListed(const std::atomic<std::uint32_t>* list, std::size_t count,
              std::uint32_t id) {
  for (std::size_t i = 0; i < count; ++i) {
    if (list[i].load(std::memory_order_acquire) == id) {
      return true;
    }
  }
  return false;
}

std::uint32_t MaybeSwap(std::uint32_t id) {
  if (id == 0) {
    return id;
  }
  g_seenCount.fetch_add(1, std::memory_order_relaxed);
  const auto blacklistCount =
      g_blacklistCount.load(std::memory_order_acquire);
  if (blacklistCount != 0 &&
      IsListed(g_blacklist, blacklistCount, id)) {
    return id;
  }
  if (!g_swapAll.load(std::memory_order_acquire)) {
    const auto sourceCount = g_sourceCount.load(std::memory_order_acquire);
    if (sourceCount == 0 ||
        !IsListed(g_sourceIds, sourceCount, id)) {
      return id;
    }
  }
  const std::uint32_t target = g_targetId.load(std::memory_order_acquire);
  if (target == 0 || target == id) {
    return id;
  }
  g_swapCount.fetch_add(1, std::memory_order_relaxed);
  return target;
}

// ---------------------------------------------------------------------------
// Map placement-record sweep.
//
// The read hook can only fire when a record is already being USED, which is too
// late for that first instantiation: the model/asset decision for the placement
// was taken earlier, so the enemy comes up empty until the placement re-streams.
// (User-verified 2026-09-19: first entry after a restart was empty, leaving and
// re-entering the map spawned the target. The CE A/B worked because the records
// were rewritten BEFORE they were used.)
//
// This worker does the same thing automatically: it walks committed writable
// private memory looking for the 12-byte placement-record signature
//     { +0x00 instanceId != 0, +0x04 key in MapSources, +0x08 flags&0xFFFF == 0x3701 }
// and writes the chosen target into +0x04. Activation is distance based, so the
// records are rewritten seconds before the player can get close to them.
// ---------------------------------------------------------------------------
extern std::atomic_bool g_shutdown;  // defined later in this TU (line ~1102)

// Placement records carry a flags word whose low byte is 0x01 and whose second
// byte is a small class value (observed 0x3701 on Jailer Oni records and 0x3601
// on others). Requiring exactly 0x3701 would silently skip every other enemy —
// e.g. Shunobon (朱盆) — so the filter is a range now. It still matters: the
// 0x398 definition records also embed a source key (+0x64) and matching by key
// alone would have written the target into them.
constexpr bool MapRecordFlagsLookLikePlacement(std::uint32_t flags) {
  const std::uint32_t low = flags & 0xFFu;
  const std::uint32_t kind = (flags >> 8) & 0xFFu;
  if (low != 0x01u) {
    return false;
  }
  switch (kind) {
    case 0x36u:
    case 0x37u:
    case 0x3Bu:
      return flags < 0x01200000u;  // the verified placement-flag family
    default:
      return false;
  }
}

// A key is a source when it is listed in MapSources OR when the ini gave it a
// per-key pool (MapPool_<SRC>) — defining a pool for an enemy is itself the
// statement "randomise this one".
bool IsMapSource(std::uint32_t key) {
  const auto slots = g_mapKeyPoolCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < slots; ++i) {
    if (g_mapKeyPools[i].source.load(std::memory_order_acquire) == key) {
      return true;
    }
  }
  const auto sourceCount = g_mapSourceCount.load(std::memory_order_acquire);
  return sourceCount != 0 && IsListed(g_mapSources, sourceCount, key);
}

// Picks the replacement key for one placement. sourceKey first selects the
// per-key pool (MapPool_<SRC>), so different enemies can randomise into
// different sets; a source with no own pool falls back to the global MapPool.
// An explicit empty per-key pool means "leave this enemy exactly as it is".
std::uint32_t MapPickTarget(std::uint32_t sourceKey, std::uint32_t instanceId) {
  const std::atomic<std::uint32_t>* pool = g_mapPool;
  std::size_t poolCount = g_mapPoolCount.load(std::memory_order_acquire);
  const auto slots = g_mapKeyPoolCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < slots; ++i) {
    if (g_mapKeyPools[i].source.load(std::memory_order_acquire) == sourceKey) {
      const std::size_t count =
          g_mapKeyPools[i].count.load(std::memory_order_acquire);
      if (count == 0) {
        return sourceKey;
      }
      pool = g_mapKeyPools[i].keys;
      poolCount = count;
      break;
    }
  }
  const std::uint32_t configured = g_targetId.load(std::memory_order_acquire);
  if (poolCount == 0) {
    return configured;
  }
  const std::uint32_t mode = g_mapRandomMode.load(std::memory_order_acquire);
  std::uint32_t pick = 0;
  if (mode == 2) {
    // Re-roll on every use (a boss changes each time it streams in).
    pick = static_cast<std::uint32_t>(
        g_mapSwapCount.fetch_add(1, std::memory_order_relaxed));
  } else {
    // Stable per placement. The source key is mixed in as well, otherwise two
    // different enemies that share a pool would always land on the same target.
    pick = ((instanceId ^ (sourceKey * 2654435761u)) * 2654435761u) >> 16;
  }
  return pool[pick % poolCount].load(std::memory_order_acquire);
}

// +0x58 = the per-placement strength/variant scalar, and the ONLY field that
// separates the two populations the CE A/B had told apart by eye. A differential
// over the whole placement table (2026-09-20, 10 samples from each side) found
// every other offset in 0x0C..0x134 identical: just 500.0f vs 1000.0f, 10/10 on
// each side. Two earlier candidates are now falsified in game: the flags word
// (MapRank=3, plus a live rewrite of all 309 source records to 0x011F3701 that
// changed nothing on screen) and +0x5C — that was simply the wrong offset, and
// writing it is what made an enemy come up EMPTY.
// 0 = follow the source placement (default), 1 = force 500.0f (normal),
// 2 = force 1000.0f (the strong/purple variant).
std::atomic<std::uint32_t> g_mapRankMode{0};
// MapRankEvery: apply the forced rank to only every Nth randomised placement.
// 1 (default) = all of them; 2 = alternate records keep their original +0x58, so
// one game launch shows the forced and the untouched variant side by side — the
// A/B that decided this field in the first place, but without a second restart.
std::atomic<std::uint32_t> g_mapRankEvery{1};
std::atomic<std::uint32_t> g_mapRankStride{0};

void ApplyMapRank(std::uintptr_t record) {
  const std::uint32_t mode = g_mapRankMode.load(std::memory_order_acquire);
  if (mode == 0) {
    return;
  }
  *reinterpret_cast<volatile float*>(record + 0x58) =
      (mode >= 2) ? 1000.0f : 500.0f;
}

// Rewrites a placement record so the enemy it spawns comes out as the target
// enemy AND as the powered-up (紫皮 / 一難) variant.
//
// Why both fields, together, at this moment: rewriting only the key (which is
// all this plugin used to do) leaves the record's variant flags at whatever the
// SOURCE placement carried. The engine then spawns the target enemy in the
// source placement's variant - which is why swapping produced a mix of purple
// and plain Gozukis from a single MapPool, and why some placements (e.g. the
// 0xD2BC record) came out plain on every single load.
//
// Setting the flags only at RUNTIME does not work: bit 0x100 of entity+0xE8 is
// downstream of this decision (a controlled write of that bit left the visible
// variant unchanged), so the variant has to be right in the record before the
// entity is built.
void ApplyTargetFlags(std::uintptr_t record) {
  if (!g_mapPurple.load(std::memory_order_acquire)) {
    return;
  }
  auto* fields = reinterpret_cast<volatile std::uint32_t*>(record);
  const std::uint32_t current = fields[2];
  const std::uint32_t desired = g_targetFlags.load(std::memory_order_acquire);
  if (current == desired) {
    return;
  }
  // Keep the record's own low 16 bits (0x3701 / 0x3601 - the placement-family
  // tag the sweep validator checks) and take only the variant bits from the
  // target: bit24 clear/set as the target has it, bit16 forced clear.
  const std::uint32_t updated =
      (current & 0x0000FFFFu) | (desired & 0xFFFF0000u);
  fields[2] = updated;
  const std::uint32_t n =
      g_mapPurpleFlagWrites.fetch_add(1, std::memory_order_relaxed);
  if (n < 32) {
    _MESSAGE("%s: purple record flags %p: %X -> %X", kPluginName,
             reinterpret_cast<void*>(record), current, updated);
  }
}

// ---------------------------------------------------------------------------
// Purple variant ("一难" / ichi-nan), SOLVED 2026-09-20 with CE + the live game.
//
// The switch is bit 0x100 of the dword at entity+0xE8. The engine's own
// powered-up placement reads 0x101 where every ordinary enemy built from the
// SAME key reads 0x1 — 20/20 entities on a live map, nothing else in the first
// 0x800 bytes separates them except the placement id — and writing 0x101 into
// 19 live plain entities turned one of them purple on screen.
//
// The placement RECORD is not the source and is now fully excluded: +0x58
// (MapRank), +0x80 (the only field that differed across the whole population)
// and a byte-identical clone of the purple record were each written in game and
// changed nothing. The table is also built once at process start and never
// rebuilt (a data breakpoint on a record's key never fired again across area
// loads), so difficulty cannot be rewriting it either. That leaves instantiation
// itself — which is exactly where MapBossHookBody runs; HookStub.asm passes the
// entity in RDX (= RDI, the object whose [+0x20] selected the record).
//
// The bit is OR-ed, never assigned. Coverage does NOT come from the placement
// hook's callers: that hook misses most spawn paths (a measured map had 67 live
// target entities and the hook had seen 8 of them). It comes from
// MapPurpleHookBody, on the per-frame flag writer every entity passes through.
constexpr std::uintptr_t kEntityVariantOffset = 0xE8;
constexpr std::uintptr_t kEntityKeyOffset = 0x28;
std::atomic<std::uint32_t> g_mapPurple{0};
std::atomic<std::uint64_t> g_mapPurpleFlagged{0};

// ORs the 一難/purple bit into one entity, if that entity carries our target key.
//
// Layout, CE-verified 2026-09-20 on a live map: the dword at entity+0xE8 reads
// 0x101 for the engine's own powered-up placements and 0x1 for every ordinary
// enemy built from the SAME key. The distinguishing bit is 0x100 — byte +0xE9 —
// and the engine's only recurring writer touches byte +0xE8 alone (see
// kMapPurpleFlagPattern), so once this bit is set nothing puts it back. That is
// why the mark survives map re-entry and needs no re-assert thread.
void MapPurpleMark(std::uintptr_t entity) {
  if (entity < 0x10000000000ULL || entity >= 0x800000000000ULL) {
    return;
  }
  const std::uint32_t target = g_targetId.load(std::memory_order_relaxed);
  if (target == 0) {
    return;
  }
  __try {
    if (*reinterpret_cast<const volatile std::uint32_t*>(entity +
                                                         kEntityKeyOffset) !=
        target) {
      return;
    }
    auto* flag = reinterpret_cast<volatile std::uint8_t*>(
        entity + kEntityVariantOffset + 1);
    const std::uint8_t before = *flag;
    if ((before & 0x01) == 0) {
      *flag = static_cast<std::uint8_t>(before | 0x01);
      g_mapPurpleFlagged.fetch_add(1, std::memory_order_relaxed);
      // One capped line per SUCCESSFUL mark. Paired with the sweep counters
      // this answers "was the plain one ever offered to the marker at all?" -
      // the question that decides whether the remaining gap is a coverage bug
      // (never offered) or a write bug (offered but did not take).
      const std::uint32_t n =
          g_mapPurpleMarkLogs.fetch_add(1, std::memory_order_relaxed);
      if (n < 96) {
        const std::uint32_t id =
            *reinterpret_cast<const volatile std::uint32_t*>(entity + 0x20);
        _MESSAGE("%s: purple mark #%u ent=%p id=%X flag=%02X->%02X", kPluginName,
                 n, reinterpret_cast<void*>(entity), id, before,
                 static_cast<unsigned>(before | 0x01));
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Called from the placement-record hook on every instantiation it happens to
// see. That hook does not cover every spawn path, so MapPurpleHookBody — the
// per-frame flag writer — is what actually guarantees full coverage.
void MapPurpleOnInstantiate(void* entity, std::uint32_t key) {
  if (g_mapPurple.load(std::memory_order_relaxed) == 0 || entity == nullptr ||
      key == 0 || key != g_targetId.load(std::memory_order_relaxed)) {
    return;
  }
  MapPurpleMark(reinterpret_cast<std::uintptr_t>(entity));
}

// Factory-entry body (see FnFactoryEntry). Runs at the creation/ensure moment
// for every entity, which the placement-record hook does not cover - the
// long-standing "coverage" problem. Passes through untouched; it only marks
// and, for the first kFepLogMax distinct target entities, records a field
// snapshot so the determining field can be pinned down from real spawns.
//
// Everything here is a plain read of the entity. No allocation, no locks.
extern "C" void MapPurpleFactoryEntryBody(void* handler, void* entity,
                                          std::uint32_t key,
                                          std::uint32_t category) {
  if (g_mapPurple.load(std::memory_order_relaxed) == 0 || entity == nullptr ||
      key == 0) {
    return;
  }
  const auto ent = reinterpret_cast<std::uintptr_t>(entity);
  if (ent < 0x10000000000ULL || ent >= 0x800000000000ULL) {
    return;
  }
  if (key != g_targetId.load(std::memory_order_relaxed)) {
    return;
  }
  std::uint32_t id = 0;
  std::uint32_t d38 = 0;
  std::uint32_t d90 = 0;
  std::uint32_t d94 = 0;
  std::uint32_t dE8 = 0;
  std::uint32_t keyEcho = 0;
  __try {
    id = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0x20);
    keyEcho = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0x28);
    d38 = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0x38);
    d90 = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0x90);
    d94 = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0x94);
    dE8 = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0xE8);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
  // keyEcho is the entity's own copy of the catalogue key; a mismatch means
  // this object is not (yet) the enemy we think it is.
  if (keyEcho != key) {
    return;
  }
  g_fepSeen.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t n = g_fepLogged.fetch_add(1, std::memory_order_relaxed);
  if (n < kFepLogMax) {
    _MESSAGE("%s: fep #%llu ent=%p cat=%X id=%X d38=%X d90=%X d94=%X dE8=%X",
             kPluginName, static_cast<unsigned long long>(n),
             reinterpret_cast<void*>(entity), category, id, d38, d90, d94,
             dE8);
  }
  // Mark the 一難 bit here as well: this path is the full-coverage one.
  MapPurpleMark(ent);
}
// Purple-variant hunt (2026-09-20). In game the user sees exactly one purple
// Gozuki: the ENGINE's own placement of 0xA263C. Every placement we swapped to
// that same id comes up plain, so the two populations share the enemy, the key
// and the area — the only difference left is in the record itself. Logging both
// populations (ours with the original key, the engine's as found) is what makes
// that differential possible offline. Capped: these are startup-only lines.
std::atomic<std::uint32_t> g_mapSwapLogs{0};
std::atomic<std::uint32_t> g_mapNativeLogs{0};
constexpr std::uint32_t kMapSwapLogMax = 512;
constexpr std::uint32_t kMapNativeLogMax = 64;

void MapLogRecord(const char* tag, std::uintptr_t record, std::uint32_t originalKey) {
  const auto* d = reinterpret_cast<const std::uint32_t*>(record);
  const auto* fl = reinterpret_cast<const float*>(record);
  _MESSAGE("%s: map %s rec=%p orig=%X id=%X flags=%X f50=%g f58=%g "
           "f5C=%X f60=%X f64=%X f68=%X f6C=%X f70=%X f9C=%X fA0=%X fA4=%X "
           "fB0=%X fB8=%X fC0=%X fD8=%X fE0=%X fF0=%X fF8=%X pos=%g,%g,%g",
           kPluginName, tag, reinterpret_cast<void*>(record), originalKey, d[0],
           d[2], static_cast<double>(fl[0x50 / 4]),
           static_cast<double>(fl[0x58 / 4]), d[0x5C / 4], d[0x60 / 4],
           d[0x64 / 4], d[0x68 / 4], d[0x6C / 4], d[0x70 / 4], d[0x9C / 4],
           d[0xA0 / 4], d[0xA4 / 4], d[0xB0 / 4], d[0xB8 / 4], d[0xC0 / 4],
           d[0xD8 / 4], d[0xE0 / 4], d[0xF0 / 4], d[0xF8 / 4],
           static_cast<double>(fl[0x40 / 4]), static_cast<double>(fl[0x44 / 4]),
           static_cast<double>(fl[0x48 / 4]));
}

// Scans one committed writable region. Returns false when the region vanished
// mid-walk (the game freed it between VirtualQuery and the read) — in that case
// nothing was written. SEH rather than a pre-check: there is no reliable
// "still mapped" test from user mode.
bool MapSweepRegion(std::uintptr_t base, std::size_t size) {
  if (g_mapKeyPoolCount.load(std::memory_order_acquire) == 0 &&
      g_mapSourceCount.load(std::memory_order_acquire) == 0) {
    return true;
  }
  // The zero-tail structure check below reads 15 dwords, and the loop used to
  // bound itself by 12 bytes — the last iterations of every region therefore
  // read past its end (one of the two ways the old sweep could fault).
  __try {
    for (std::uintptr_t p = base; p + 64 <= base + size; p += 4) {
      const auto* f = reinterpret_cast<const std::uint32_t*>(p);
      if (!MapRecordFlagsLookLikePlacement(f[2]) || f[0] == 0) {
        continue;  // cheap reject first; the flag word is rare
      }
      // The target id is what we write, so a record that already holds it was
      // never ours: it is the engine's own placement. Logging it here (instead
      // of a second walk) is safe because every address is visited once per
      // pass, and a swapped record carries the target only from the NEXT pass.
      if (f[1] == g_targetId.load(std::memory_order_acquire)) {
        bool nativeTail = true;
        for (int k = 3; k < 15; ++k) {
          if (f[k] != 0) {
            nativeTail = false;
            break;
          }
        }
        if (nativeTail &&
            g_mapNativeLogs.fetch_add(1, std::memory_order_relaxed) <
                kMapNativeLogMax) {
          MapLogRecord("native", p, 0);
        }
        continue;
      }
      if (!IsMapSource(f[1])) {
        continue;
      }
      const std::uint32_t target = MapPickTarget(f[1], f[0]);
      if (target == 0 || target == f[1]) {
        continue;
      }
      // Structure check before writing: a real placement record is followed by
      // 12 zero dwords in every sample observed (2026-09-20). Together with the
      // verified flag family this excludes the false matches that corrupted
      // memory when the filter was loose (472 "records" vs a handful of real).
      bool zeroTail = true;
      for (int k = 3; k < 15; ++k) {
        if (f[k] != 0) {
          zeroTail = false;
          break;
        }
      }
      if (!zeroTail) {
        continue;
      }
      if (g_mapSwapLogs.fetch_add(1, std::memory_order_relaxed) <
          kMapSwapLogMax) {
        MapLogRecord("swap", p, f[1]);
      }
      reinterpret_cast<volatile std::uint32_t*>(p)[1] = target;
      // Variant bits must land in the record BEFORE the entity is built; a
      // runtime write to entity+0xE8 does not affect the visible variant.
      ApplyTargetFlags(p);
      // +0x58 (MapRank): 0 = follow, 1 = 500.0f, 2 = 1000.0f. MapRankEvery > 1
      // forces only every Nth placement so one launch shows both variants.
      {
        const std::uint32_t every =
            g_mapRankEvery.load(std::memory_order_acquire);
        const std::uint32_t slot =
            g_mapRankStride.fetch_add(1, std::memory_order_relaxed);
        if (every <= 1 || (slot % every) == 0) {
          ApplyMapRank(p);
        }
      }
      g_mapSweepHits.fetch_add(1, std::memory_order_relaxed);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // The game freed this region between VirtualQuery and the read (or it was
    // never fully committed). Losing the region is fine — nothing was written.
    return false;
  }
  return true;
}

// One pass step over committed writable private memory. The cursor is KEPT
// BETWEEN CALLS: restarting from the minimum address every tick — the first
// version of this — only ever re-scanned the first 128 MB while the game heaps
// (where the placement records live) sit terabytes up, so the sweep never
// reached them and the near placements were still built before any rewrite
// (observed in game 2026-09-20 00:05: first entry, the two close Jailer Oni
// were empty while a distant one had re-streamed correctly).
std::atomic<std::uintptr_t> g_mapSweepCursor{0};
std::atomic<std::uint32_t> g_mapSweepPasses{0};

// Returns true once the whole address space has been walked (cursor wrapped).
bool MapSweepOnce() {
  std::atomic<std::uint32_t> skipped{0};  // regions lost to a mid-walk free
  SYSTEM_INFO systemInfo{};
  GetSystemInfo(&systemInfo);
  const std::uintptr_t minimum =
      reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
  const std::uintptr_t maximum =
      reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);

  std::uintptr_t address = g_mapSweepCursor.load(std::memory_order_relaxed);
  if (address < minimum || address >= maximum) {
    address = minimum;
  }
  std::size_t budget = 192u * 1024u * 1024u;
  MEMORY_BASIC_INFORMATION info{};
  bool completed = false;
  while (address < maximum && budget != 0 && !g_shutdown.load()) {
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) !=
        sizeof(info)) {
      address += 0x1000;
      continue;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto size = static_cast<std::size_t>(info.RegionSize);
    if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
        (info.Protect == PAGE_READWRITE || info.Protect == PAGE_WRITECOPY)) {
      if (!MapSweepRegion(base, size)) {
        skipped.fetch_add(1, std::memory_order_relaxed);
      }
      budget = size < budget ? budget - size : 0;
    }
    address = base + size;
  }
  if (address >= maximum) {
    address = minimum;
    completed = true;
  }
  g_mapSweepCursor.store(address, std::memory_order_relaxed);
  if (completed) {
    g_mapSweepPasses.fetch_add(1, std::memory_order_relaxed);
    const auto lost = skipped.load(std::memory_order_relaxed);
    if (lost != 0) {
      _MESSAGE("%s: map table scan skipped %llu region(s) freed mid-walk",
               kPluginName, lost);
    }
  }
  return completed;
}

// ONE-SHOT table randomiser (2026-09-20, second attempt). The placement records
// are built once at process start and never rebuilt afterwards — a data
// breakpoint on a record's own key field never fired again across area loads —
// so the correct level to patch is the record table itself, ONCE, instead of
// every instantiation. Consequence: with MapHook=0 (the default) the plugin
// installs no code hook at all and the game runs untouched after this pass,
// which is what removes the freeze risk the continuously running sweep had.
// The read/write loop itself is SEH-guarded and strictly bounded (see
// MapSweepRegion), so a region the game frees mid-walk costs one region, not
// the process.
void MapSweepWorker() {
  for (int tick = 0; tick < 20 && !g_shutdown.load(); ++tick) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (g_mapBoss.load(std::memory_order_acquire)) {
      break;  // the world has had ~30 s to build its placement table
    }
  }
  if (g_shutdown.load() || !g_mapBoss.load(std::memory_order_acquire)) {
    return;
  }
  // The table exists from process start, but if the walk finds nothing we are
  // simply too early (or the player has not loaded a save yet): wait and walk
  // again. A second walk can no longer see a source key once it was rewritten,
  // so "hits == 0" is the only thing that re-arms this loop.
  for (int attempt = 0; attempt < 20 && !g_shutdown.load(); ++attempt) {
    for (int step = 0; step < 64 && !g_shutdown.load(); ++step) {
      if (MapSweepOnce()) {
        break;
      }
    }
    if (g_mapSweepHits.load(std::memory_order_relaxed) != 0 ||
        g_mapKeyPoolCount.load(std::memory_order_acquire) == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20000));
    g_mapSweepCursor.store(0, std::memory_order_relaxed);
  }
  g_mapTableDone.store(true, std::memory_order_release);
  _MESSAGE("%s: map table randomise done: %llu record(s) rewritten, %u full "
           "pass(es), tableDone=1",
           kPluginName, g_mapSweepHits.load(std::memory_order_relaxed),
           g_mapSweepPasses.load(std::memory_order_relaxed));
}

// Per-frame 一難 flag hook body. Called from MapPurpleHookStub with RCX = the
// entity the engine is about to write entity+0xE8 for. Deliberately tiny: one
// key compare plus, at most once per entity, one byte OR. No I/O, no locks, no
// allocation — this runs for every active entity on every frame.
extern "C" void MapPurpleHookBody(void* entity) {
  if (g_mapPurple.load(std::memory_order_relaxed) == 0) {
    return;
  }
  MapPurpleMark(reinterpret_cast<std::uintptr_t>(entity));
}

// Source-level map swap body. Called from MapBossHookStub with RCX = placement
// record; returns the key to use (EAX). It re-reads the record itself so the
// asm stub only has to name one register, and so the "no opinion" answer is
// always the key the game was about to load.
extern "C" std::uint32_t MapBossHookBody(void* record, void* entity) {
  const auto* fields = reinterpret_cast<const std::uint32_t*>(record);
  const std::uint32_t sourceKey = fields[1];  // +0x04 = enemy key
  // Purple-variant hunt: log the (record, entity) pair for every instantiation.
  // The record identifies WHICH placement spawned (the engine's own 0xA263C
  // placements come out purple, ours plain), the entity is the live object the
  // difference must live in — the placement record is already ruled out: a
  // byte-identical clone of the purple record still spawned a plain enemy.
  {
    static std::atomic<std::uint32_t> instLogs{0};
    const std::uint32_t index =
        instLogs.fetch_add(1, std::memory_order_relaxed);
    if (index < 2048) {
      _MESSAGE("%s: map inst #%u rec=%p ent=%p id=%X key=%X flags=%X",
               kPluginName, index, record, entity, fields[0], sourceKey,
               fields[2]);
    }
  }
  // Every exit below hands the game the key it will actually build the entity
  // from, so the purple (一难) flag is applied on the way out — see
  // MapPurpleOnInstantiate for why the entity, not the record, is the lever.
  auto finish = [&](std::uint32_t key) -> std::uint32_t {
    MapPurpleOnInstantiate(entity, key);
    return key;
  };
  if (sourceKey == 0 || !g_mapBoss.load(std::memory_order_acquire)) {
    return finish(sourceKey);
  }

  const auto blacklistCount = g_blacklistCount.load(std::memory_order_acquire);
  if (blacklistCount != 0 &&
      IsListed(g_blacklist, blacklistCount, sourceKey)) {
    return finish(sourceKey);
  }

  // The write-through below turns the record itself into the target after the
  // first call, so the ORIGINAL key has to be remembered here for the
  // re-roll mode (an evicted slot simply falls through unchanged).
  const std::uint32_t recordLow =
      static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(record));
  std::uint32_t originalKey = sourceKey;
  {
    const std::size_t base = (recordLow >> 4) % kMapMemoSlots;
    for (std::size_t probe = 0; probe < kMapMemoSlots; ++probe) {
      const std::size_t index = (base + probe) % kMapMemoSlots;
      const std::uint32_t slotRecord =
          g_mapMemoRecord[index].load(std::memory_order_acquire);
      if (slotRecord == recordLow) {
        originalKey = g_mapMemoSource[index].load(std::memory_order_acquire);
        break;
      }
      if (slotRecord == 0) {
        g_mapMemoSource[index].store(sourceKey, std::memory_order_release);
        g_mapMemoRecord[index].store(recordLow, std::memory_order_release);
        break;
      }
    }
  }

  if (!IsMapSource(originalKey)) {
    return finish(sourceKey);
  }

  const std::uint32_t target = MapPickTarget(originalKey, fields[0]);
  if (target == 0 || target == sourceKey) {
    return finish(sourceKey);
  }

  const auto index = g_mapSwapCount.fetch_add(1, std::memory_order_relaxed);
  if (index < 64) {
    _MESSAGE("%s: map source swap %X -> %X (record %p inst %X)", kPluginName,
             originalKey, target, record, fields[0]);
  }
  // Write THROUGH to the placement record, not just into R14. The CE A/B that
  // worked patched the record itself, so every downstream reader (descriptor,
  // params lookup, asset job, spawn registry) saw one consistent key. Rewriting
  // only the register left the record at the source key: the game then built
  // the entity with the target while its model/asset side still followed the
  // old key, and the enemy came up invisible/absent (observed in game
  // 2026-09-19 23:52 — the log showed 8 swaps and an empty spawn point).
  reinterpret_cast<volatile std::uint32_t*>(record)[1] = target;
  // Set the variant bits in the same breath as the key. Leaving them at the
  // source placement's value is what produced the "some purple, some plain"
  // mix from one MapPool.
  ApplyTargetFlags(reinterpret_cast<std::uintptr_t>(record));
  ApplyMapRank(reinterpret_cast<std::uintptr_t>(record));
  return finish(target);
}

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

void LogLoop() {
  try {
    while (true) {
      Sleep(10000);
      // Hot reload: re-read the ini whenever its mtime changes so KTID
      // swap pairs can be tuned without restarting the game.
      if (!g_configPath.empty()) {
        if (const HANDLE file = CreateFileW(
                g_configPath.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            file != INVALID_HANDLE_VALUE) {
          FILETIME mtime{};
          GetFileTime(file, nullptr, nullptr, &mtime);
          CloseHandle(file);
          if (CompareFileTime(&mtime, &g_configMtime) != 0) {
            g_configMtime = mtime;
            _MESSAGE("%s: config change detected, reloading", kPluginName);
            LoadConfig(nullptr);
          }
        }
      }
      const std::uint64_t seen =
          g_seenCount.exchange(0, std::memory_order_relaxed);
      const std::uint64_t swapped =
          g_swapCount.exchange(0, std::memory_order_relaxed);
      const std::uint64_t assetSwapped =
          g_assetSwapCount.exchange(0, std::memory_order_relaxed);
      const std::uint64_t createSwapped =
          g_createSwapCount.exchange(0, std::memory_order_relaxed);
      const std::uint64_t assetMissed =
          g_assetMissCount.exchange(0, std::memory_order_relaxed);
      if (seen != 0 || assetSwapped != 0 || createSwapped != 0 ||
          assetMissed != 0) {
        _MESSAGE("%s: last 10s: seen=%llu factorySwapped=%llu "
                 "assetRedirected=%llu assetMissed=%llu createSwapped=%llu "
                 "(ktidPairs=%zu)",
                 kPluginName, static_cast<unsigned long long>(seen),
                 static_cast<unsigned long long>(swapped),
                 static_cast<unsigned long long>(assetSwapped),
                 static_cast<unsigned long long>(assetMissed),
                 static_cast<unsigned long long>(createSwapped),
                 g_ktidSwapMap.load()->size());
      }
      if (g_assetTrace.load(std::memory_order_acquire)) {
        const std::uint64_t traceCalls =
            g_assetTraceTotal.exchange(0, std::memory_order_relaxed);
        if (traceCalls != 0) {
          struct TraceTop {
            std::uint32_t key;
            std::uint32_t hits;
          };
          TraceTop top[16]{};
          std::size_t distinct = 0;
          for (std::size_t i = 0; i < kAssetTraceSlots; ++i) {
            const std::uint32_t hits =
                g_assetTraceHits[i].exchange(0, std::memory_order_relaxed);
            if (hits == 0) {
              continue;
            }
            ++distinct;
            if (hits <= top[15].hits) {
              continue;
            }
            const TraceTop entry{
                g_assetTraceKey[i].load(std::memory_order_relaxed), hits};
            std::size_t position = 15;
            while (position > 0 && top[position - 1].hits < hits) {
              top[position] = top[position - 1];
              --position;
            }
            top[position] = entry;
          }
          std::string line = "assetTrace 10s: calls=" +
                             std::to_string(traceCalls) + " distinct=" +
                             std::to_string(distinct) + " top:";
          for (const auto& entry : top) {
            if (entry.hits == 0) {
              continue;
            }
            char item[32]{};
            std::snprintf(item, sizeof(item), " 0x%08X:%u", entry.key,
                          entry.hits);
            line += item;
          }
          _MESSAGE("%s: %s", kPluginName, line.c_str());
        }
      }
      if (g_discoverMode.load(std::memory_order_acquire)) {
        WriteDiscoverCensus();
      }
    }
  } catch (...) {
  }
}

std::atomic_bool g_shutdown{false};
std::atomic_bool g_factoryHooked{false};
std::atomic_bool g_factoryEntryHooked{false};
std::atomic_bool g_getResHooked{false};
std::atomic_bool g_createHooked{false};
std::atomic_bool g_createOrcheHooked{false};
std::atomic_bool g_catalogHooked{false};
std::atomic_bool g_identityHooked{false};
std::atomic<std::uint64_t> g_createDiagCount{0};

// Sliding-window rate cap shared by the factory/orchestrator hooks: the
// shrine mass-respawn funnels dozens of spawns per burst; swapping them all
// with an unloaded target aborts the storm and freezes the game. Training
// summons are 1-3/min — a small window passes them and blocks the storm.
// Returns false when the caller must pass through unpatched this minute.
bool CreateRateAllow() {
  const auto now = GetTickCount64();
  std::uint32_t usedLastMinute = 0;
  for (const auto& t : g_createSwapTimes) {
    if (now - t.load(std::memory_order_relaxed) < 60000) {
      ++usedLastMinute;
    }
  }
  if (usedLastMinute >=
      g_createMaxPerMinute.load(std::memory_order_acquire)) {
    return false;
  }
  g_createSwapTimes[g_createSwapSlot.fetch_add(1) %
                    std::size(g_createSwapTimes)]
      .store(now, std::memory_order_relaxed);
  return true;
}

// Evidence collector for the menu-summon hook: walks UP from this hook
// (return addresses near the entry RSP / the RBP chain) so the caller that
// baked the wrapper's asset manifest can be located WITHOUT the flaky CE
// bridge, and snapshots the manifest bytes the orchestrator is about to
// read (its `lea rsi,[rcx+0x1060]`). Pure observation: nothing is patched
// here. Rate-deduped: human summons are seconds apart, so one snapshot per
// summon is plenty and a stray caller flood cannot bloat the log.
void DumpSummonEvidence(std::uintptr_t wrapper, std::uint32_t key, void* arg2,
                        void* arg3, void* arg4) {
  static std::atomic<std::uint64_t> s_lastDumpTick{0};
  const auto now = GetTickCount64();
  if (now - s_lastDumpTick.load(std::memory_order_relaxed) < 2000) {
    return;
  }
  s_lastDumpTick.store(now, std::memory_order_relaxed);

  const auto exeBase =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  std::uintptr_t exeEnd = exeBase + 0x4000000;  // fallback window
  if (const auto size = HookUtils::GetModuleSize(
          reinterpret_cast<HMODULE>(exeBase))) {
    exeEnd = exeBase + *size;
  }

  _MESSAGE("%s: dump key=0x%05X args=%p %p %p wrapper=%p rsp=%p rbp=%p "
           "exe=%p..%p",
           kPluginName, key, reinterpret_cast<void*>(wrapper), arg2, arg3,
           arg4, reinterpret_cast<void*>(g_hookRsp),
           reinterpret_cast<void*>(g_hookRbp),
           reinterpret_cast<void*>(exeBase),
           reinterpret_cast<void*>(exeEnd));

  // Direct caller: [entry RSP] is its return address, always valid.
  __try {
    const auto ret0 =
        *reinterpret_cast<const std::uint64_t*>(g_hookRsp);
    if (ret0 >= exeBase + 0x1000 && ret0 < exeEnd) {
      _MESSAGE("%s:   direct caller exe+0x%07X",
               kPluginName, static_cast<std::uint32_t>(ret0 - exeBase));
    } else {
      _MESSAGE("%s:   direct caller %p (outside exe)",
               kPluginName, reinterpret_cast<void*>(ret0));
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  // Frame-chain walk: valid only while callers keep RBP frame pointers
  // (FPO frames silently break the chain; the scan below covers that).
  auto fp = g_hookRbp;
  __try {
    for (std::uint32_t i = 0; i < 8; ++i) {
      if (fp < 0x10000 || (fp & 7) != 0 || fp >= 0x7FFF00000000ULL) {
        break;
      }
      const auto ret = *reinterpret_cast<const std::uint64_t*>(fp + 8);
      const auto next = *reinterpret_cast<const std::uint64_t*>(fp);
      if (ret >= exeBase + 0x1000 && ret < exeEnd) {
        _MESSAGE("%s:   rbp[%u] exe+0x%07X", kPluginName, i,
                 static_cast<std::uint32_t>(ret - exeBase));
      }
      fp = next;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  // Raw scan of the caller's stack region: any exe-code pointer near the
  // entry RSP is a candidate return address, FPO or not. Offsets identify
  // each frame's position relative to this hook.
  std::uint32_t logged = 0;
  __try {
    for (std::uint32_t off = 0; off < 0x600 && logged < 24; off += 8) {
      const auto v =
          *reinterpret_cast<const std::uint64_t*>(g_hookRsp + off);
      if (v >= exeBase + 0x1000 && v < exeEnd) {
        _MESSAGE("%s:   scan[rsp+0x%03X] exe+0x%07X", kPluginName, off,
                 static_cast<std::uint32_t>(v - exeBase));
        ++logged;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  // Manifest snapshot: the region the orchestrator addresses via
  // [rcx+0x1060], plus the wrapper area around the entity-pointer slot.
  char hex[160];
  auto hexDump = [&](std::uintptr_t addr, std::uint32_t size) {
    hex[0] = '\0';
    __try {
      for (std::uint32_t i = 0; i < size; ++i) {
        std::sprintf(hex + i * 2, "%02X",
                     *reinterpret_cast<const std::uint8_t*>(addr + i));
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      hex[0] = '\0';
    }
  };
  hexDump(wrapper + 0xC0, 0x40);
  _MESSAGE("%s: wrapper  +0x00C0: %s", kPluginName, hex);
  for (std::uint32_t off = 0x1000; off < 0x1200; off += 0x40) {
    hexDump(wrapper + off, 0x40);
    _MESSAGE("%s: manifest +0x%04X: %s", kPluginName, off, hex);
  }
}

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
namespace {

struct RosterShard {
  const std::uint8_t* base;
  std::size_t size;
};

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

}  // namespace

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

// PairSwap sweep (2026-09-18 CE 终局结论, 实测可复现).
//
// A summon's enemy is decided by the PAIR of two small structures, both keyed
// by the enemy key:
//   request-side descriptor : key @ +0x0C, tag @ +0x10, 1.0f @ -0x08
//   parameter-table entry   : tag @ +0x00, key @ +0x04, 0x1F3F0x @ +0x08
// Rewriting only ONE side aborts the spawn silently (controlled A/B); rewriting
// the key on BOTH sides makes that summon button produce the target enemy —
// verified live twice (jailer button -> ninja appeared, with the tag left
// deliberately mismatched). The tag therefore only has to be a tag registered
// in this scene, so it is left untouched on both sides. Live entity identities
// store key<<4 in the 0xA0 slot pool, so a bare-key sweep can never hit them.
// True when a tag may take part in a pair match. With no whitelist configured
// the historical 0x90000..0x9FFFF band is used (the range the training room's
// roster tags live in). A configured whitelist replaces the band entirely, so
// tags outside it (e.g. Gozuki's world-map tag 0xCC15) can be targeted too.
bool PairTagAllowed(std::uint32_t tag) {
  const auto count = g_pairTagCount.load(std::memory_order_acquire);
  if (count == 0) {
    return tag >= 0x90000 && tag <= 0x9FFFF;
  }
  for (std::size_t i = 0; i < count && i < kMaxListEntries; ++i) {
    if (g_pairTags[i].load(std::memory_order_relaxed) == tag) {
      return true;
    }
  }
  return false;
}

// Target key configured for a summon button identified by its tag, or 0.
std::uint32_t PairMapLookup(std::uint32_t tag) {
  const auto count = g_pairMapCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count && i < kPairMapMax; ++i) {
    if (g_pairMapTag[i].load(std::memory_order_relaxed) == tag) {
      return g_pairMapKey[i].load(std::memory_order_relaxed);
    }
  }
  return 0;
}

// True when `value` is a key this plugin writes (used to decide whether a
// remembered site still holds our write before restoring it).
bool IsOurTargetKey(std::uint32_t value) {
  if (value == g_pairToKey.load(std::memory_order_relaxed)) {
    return true;
  }
  const auto count = g_pairMapCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count && i < kPairMapMax; ++i) {
    if (g_pairMapKey[i].load(std::memory_order_relaxed) == value) {
      return true;
    }
  }
  return false;
}

// Records a site this plugin rewrote together with its original value, so it can
// be restored exactly later. The stored address is always the 4-byte KEY field
// (both pair roles keep the key there).
void RememberPairPatch(std::uintptr_t addr, std::uint32_t original) {
  const auto n = g_pairPatchedCount.load(std::memory_order_relaxed);
  if (n >= kPairPatchSlots) {
    return;
  }
  g_pairPatchedAddr[n].store(addr, std::memory_order_relaxed);
  g_pairPatchedOrig[n].store(original, std::memory_order_relaxed);
  g_pairPatchedCount.store(n + 1, std::memory_order_release);
}

// Restores every remembered site that still holds one of our target keys.
// Faults are swallowed and slots that were changed by the game are left alone.
void RevertPairPatches() {
  const auto n = g_pairPatchedCount.exchange(0, std::memory_order_acq_rel);
  std::size_t restored = 0;
  for (std::size_t i = 0; i < n && i < kPairPatchSlots; ++i) {
    const auto addr = g_pairPatchedAddr[i].load(std::memory_order_relaxed);
    const auto original = g_pairPatchedOrig[i].load(std::memory_order_relaxed);
    if (addr == 0 || original == 0) {
      continue;
    }
    __try {
      auto* slot = reinterpret_cast<std::uint32_t*>(addr);
      if (*slot != original && IsOurTargetKey(*slot)) {
        *slot = original;
        ++restored;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  _MESSAGE("%s: pair revert: restored %zu of %zu site(s)", kPluginName,
           restored, n);
}

void PairScanShard(const RosterShard& shard, std::uint32_t fromKey,
                   std::uint32_t toKey, std::atomic<std::uint64_t>* logTick) {
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
    for (std::size_t off = 8; off + 8 <= n; off += 4) {
      std::uint32_t key = 0;
      std::memcpy(&key, buf + off, 4);
      std::uint32_t tagAfter = 0;
      std::uint32_t tagBefore = 0;
      std::uint32_t oneBefore = 0;
      std::memcpy(&tagAfter, buf + off + 4, 4);
      std::memcpy(&tagBefore, buf + off - 4, 4);
      std::memcpy(&oneBefore, buf + off - 8, 4);
      // Two shapes carry the key (see the header comment): the parameter-table
      // entry (tag before, 0x1F3F0x after) and the request-side descriptor
      // (tag after, 1.0f before). The tag identifies the summon button.
      const bool requestShape = oneBefore == 0x3F800000 && tagAfter != 0;
      const bool paramShape = (tagAfter & 0xFFFFFFF0u) == 0x1F3F00u &&
                              tagBefore != 0;
      if (!requestShape && !paramShape) {
        continue;
      }
      const std::uint32_t tag = paramShape ? tagBefore : tagAfter;
      // PairMap (tag -> key) wins; the legacy PairFromKey -> PairToKey rule is
      // kept as a fallback and additionally needs an allowed tag.
      std::uint32_t target = PairMapLookup(tag);
      if (target == 0) {
        if (key == fromKey && PairTagAllowed(tag)) {
          target = toKey;
        } else {
          continue;
        }
      }
      if (target == key) {
        continue;
      }
      const auto addr =
          reinterpret_cast<std::uintptr_t>(shard.base + chunk + off);
      __try {
        auto* slot = reinterpret_cast<std::uint32_t*>(addr);
        if (*slot == key) {
          *slot = target;
          if (g_pairRevert.load(std::memory_order_acquire)) {
            RememberPairPatch(addr, key);
          }
          const auto total =
              g_pairSwapCount.fetch_add(1, std::memory_order_relaxed);
          const auto now = GetTickCount64();
          auto last = logTick->load(std::memory_order_relaxed);
          // The first patches are always logged: every role of every mapping
          // must be visible (a rate limit once hid the request-desc line).
          // After that the log is throttled to one line per 5s.
          if (total < 32 ||
              (now - last > 5000 &&
               logTick->compare_exchange_strong(last, now))) {
            _MESSAGE("%s: pair swap #%llu tag 0x%05X key 0x%05X -> 0x%05X "
                     "(%s) at %p",
                     kPluginName, static_cast<unsigned long long>(total + 1),
                     tag, key, target, paramShape ? "param-entry"
                                                  : "request-desc",
                     reinterpret_cast<void*>(addr));
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
  }
}

void PairSweepOnce(std::uint32_t fromKey, std::uint32_t toKey,
                   std::atomic<std::uint64_t>* logTick) {
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
    if (mbi.State != MEM_COMMIT ||
        (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_IMAGE) ||
        mbi.RegionSize < 0x10000 ||
        (mbi.Protect != PAGE_READWRITE && mbi.Protect != PAGE_WRITECOPY &&
         mbi.Protect != PAGE_EXECUTE_READWRITE)) {
      continue;
    }
    shards.push_back(
        {reinterpret_cast<const std::uint8_t*>(mbi.BaseAddress),
         static_cast<std::size_t>(mbi.RegionSize)});
  }
  constexpr std::size_t kThreads = 4;
  if (shards.size() < kThreads * 2) {
    for (const auto& s : shards) {
      PairScanShard(s, fromKey, toKey, logTick);
    }
    return;
  }
  std::vector<std::thread> pool;
  std::atomic<std::size_t> next{0};
  for (std::size_t t = 0; t < kThreads; ++t) {
    pool.emplace_back([&shards, &next, fromKey, toKey, logTick]() {
      for (;;) {
        const auto i = next.fetch_add(1);
        if (i >= shards.size() || g_shutdown.load()) {
          break;
        }
        PairScanShard(shards[i], fromKey, toKey, logTick);
      }
    });
  }
  for (auto& th : pool) {
    th.join();
  }
}

void PairLoop() {
  try {
    std::atomic<std::uint64_t> logTick{0};
    bool wasActive = false;
    std::uint64_t lastGeneration = 0;
    while (!g_shutdown.load(std::memory_order_acquire)) {
      const auto from = g_pairFromKey.load(std::memory_order_acquire);
      const auto to = g_pairToKey.load(std::memory_order_acquire);
      const bool haveLegacy = from != 0 && to != 0 && from != to;
      const bool haveMap =
          g_pairMapCount.load(std::memory_order_acquire) != 0;
      const bool active = g_pairSwap.load(std::memory_order_acquire) &&
                          (haveLegacy || haveMap);
      const auto generation = g_configGeneration.load(std::memory_order_acquire);
      // Leaving the active state - or reloading the config, which may retarget
      // the mapping - restores everything we wrote first. A tag's key lives in
      // ONE global record, so a stale patch would be visible in every scene.
      // PairRevert=0 keeps the old "permanent rewrite" behaviour by never
      // remembering the sites.
      if (wasActive && (!active || generation != lastGeneration)) {
        RevertPairPatches();
        wasActive = false;
      }
      if (active) {
        PairSweepOnce(from, to, &logTick);
        wasActive = true;
        lastGeneration = generation;
      }
      Sleep(2000);
    }
    if (wasActive) {
      RevertPairPatches();
    }
  } catch (...) {
  }
}


// Menu-load orchestrator hook body (reached via CaptureHookContext in
// HookStub.asm). Entry RVA 0x2E60C0; kick-bp stack capture puts this frame
// exclusively on menu-summon loads (shrine/world respawn loads enter via
// 0x3635F2/0x3DF407/0x3B2141 and never reach here), so no scene gate is
// needed beyond the source-key hit. arg1 (RCX) is the spawn-request wrapper:
// it holds a pointer to the raw entity somewhere in its first ~0x1200 bytes.
// The entity is identified by its embedded id ([entity]>>4 == key) plus its
// identity record ([entity+0xF8]->record, record[1] == key). Patching BOTH
// before this function enqueues the asset load makes the worker resolve the
// TARGET's catalog entry and files — the game loads the target by itself,
// and the downstream builder/factory read one consistent identity. This is
// the pre-load, zero-inconsistency swap point.
extern "C" void CreateSwapHookBody(void* arg1, void* entity, void* arg3,
                                   void* arg4) {
  const auto target = g_createTarget.load(std::memory_order_acquire);
  const auto a1 = reinterpret_cast<std::uintptr_t>(arg1);
  if (g_createSwap.load(std::memory_order_acquire) && target != 0 &&
      a1 >= 0x10000000000ULL && a1 <= 0x800000000000ULL) {
    __try {
      const auto sourceCount =
          g_createSourceCount.load(std::memory_order_acquire);
      for (std::uint32_t off = 0; off < 0x1200; off += 8) {
        const auto p = *reinterpret_cast<const std::uintptr_t*>(a1 + off);
        if (p < 0x10000000000ULL || p > 0x800000000000ULL) {
          continue;
        }
        const std::uint32_t emb = *reinterpret_cast<const std::uint32_t*>(p);
        const std::uint32_t key = emb >> 4;
        if (key == 0 || !IsListed(g_createSources, sourceCount, key)) {
          continue;
        }
        const auto rec = *reinterpret_cast<const std::uintptr_t*>(p + 0xF8);
        if (rec < 0x10000000000ULL || rec > 0x800000000000ULL ||
            *reinterpret_cast<const std::uint32_t*>(rec + 4) != key) {
          continue;
        }
        // Evidence first: who called us + the manifest bytes the
        // orchestrator will read. Dumped on EVERY matched summon (all three
        // training bosses), independent of the rate cap, so cross-boss
        // snapshots stay comparable. The cap only gates the patch below.
        DumpSummonEvidence(a1, key, entity, arg3, arg4);
        if (!CreateRateAllow()) {
          break;
        }
        *reinterpret_cast<std::uint32_t*>(p) = target << 4;  // embedded id
        *reinterpret_cast<std::uint32_t*>(rec + 4) = target;  // record key
        g_createSwapCount.fetch_add(1, std::memory_order_relaxed);
        _MESSAGE("%s: orchestrator swap 0x%05X -> 0x%05X "
                 "(wrapper=%p entity=%p rec=%p off=+%X)",
                 kPluginName, key, target, arg1,
                 reinterpret_cast<void*>(p),
                 reinterpret_cast<void*>(rec), off);
        break;  // one entity per menu summon
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  g_entryOriginal(arg1, entity, arg3, arg4);
}

// Catalog-query hook body (reached via CaptureCatalogContext in HookStub.asm).
// Entry RVA 0x4EEF44. arg3 (R8) points at the embedded id dword (key<<4) that
// the query reads via `mov edi,[r8]; shr edi,04`. The rewrite is TEMPORARY:
// 2026-09-18 终审 evidence — a PERSISTENT rewrite lands in the roster object
// and breaks every later summon (roster validation rejects 0xA263C and the
// spawn aborts silently, leaving the stuck key behind). Restoring the
// original key right after the query returns gives: assets + params resolved
// for the TARGET during the query window, while every downstream stage
// (roster check, placement, spawn finalize) still sees the source key.
extern "C" void CatalogQueryHookBody(void* arg1, void* arg2, void* keyPtr,
                                     void* arg4) {
  (void)arg1; (void)arg2; (void)arg4;
  const auto kp = reinterpret_cast<std::uintptr_t>(keyPtr);
  std::uint32_t savedEmb = 0;
  bool restore = false;
  if (g_catalogSwap.load(std::memory_order_acquire) &&
      kp >= 0x10000 && kp < 0x7FFF00000000ULL) {
    __try {
      const auto emb = *reinterpret_cast<const std::uint32_t*>(kp);
      const auto key = emb >> 4;
      const auto target = g_createTarget.load(std::memory_order_acquire);
      const auto sourceCount =
          g_createSourceCount.load(std::memory_order_acquire);
      if (key != 0 && target != 0 &&
          IsListed(g_createSources, sourceCount, key)) {
        *reinterpret_cast<std::uint32_t*>(kp) = target << 4;
        savedEmb = emb;
        restore = true;
        g_createSwapCount.fetch_add(1, std::memory_order_relaxed);
        _MESSAGE("%s: catalog query swap 0x%05X -> 0x%05X (ptr=%p, temp)",
                 kPluginName, key, target, keyPtr);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      restore = false;
    }
  }
  g_catalogOriginal(arg1, arg2, keyPtr, arg4);
  if (restore) {
    __try {
      *reinterpret_cast<std::uint32_t*>(kp) = savedEmb;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
}


// The packed exe decrypts .text lazily; a pattern can be unreadable at
// plugin-init time even though it is correct. Retry until the region is
// committed, instead of giving up at game boot. Each hook installs at most
// once — a pattern miss retries only the missing piece (double inline hooks
// on one address corrupt each other and can leave the trampoline null).
void InstallHooksWithRetry() {
  try {
    for (int attempt = 0; attempt < 600 && !g_shutdown.load(); ++attempt) {
      bool complete = true;

      if (!g_factoryHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t factoryCore =
            HookUtils::ScanIDAPattern(kFactoryCorePattern);
        if (factoryCore == 0) {
          complete = false;
        } else {
          HookLambda(reinterpret_cast<FnFactoryCore>(factoryCore),
                     [](void* arg1, void* arg2, std::uint32_t spawnId,
                        void* paramObject) {
                       const std::uint32_t swappedId =
                           g_factorySwap.load(std::memory_order_acquire)
                               ? MaybeSwap(spawnId)
                               : spawnId;
                       original(arg1, arg2, swappedId, paramObject);
                     });
          g_factoryHooked.store(true, std::memory_order_release);
          _MESSAGE("%s: factory core hook installed at %p", kPluginName,
                   reinterpret_cast<void*>(factoryCore));
        }
      }

      // Factory REAL entry (kFactoryEntryPattern). The full-coverage creation
      // hook: unlike RVA 0x679895 it is reached for every entity that is built
      // or ensured, and RDX is the entity at that moment. Installed with
      // MapPurple (it is what finally makes the 一難 marking cover everything
      // the placement-record hook missed) and also with FactoryDiag so a plain
      // spawn can be sampled without changing any behaviour.
      if ((g_mapPurple.load(std::memory_order_acquire) ||
           g_factoryDiag.load(std::memory_order_acquire)) &&
          !g_factoryEntryHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t factoryEntry =
            HookUtils::ScanIDAPattern(kFactoryEntryPattern);
        if (factoryEntry == 0) {
          complete = false;
          _MESSAGE("%s: factory entry pattern not found (will retry)",
                   kPluginName);
        } else {
          HookLambda(reinterpret_cast<FnFactoryEntry>(factoryEntry),
                     [](void* handler, void* entity, std::uint32_t key,
                        std::uint32_t category) {
                       MapPurpleFactoryEntryBody(handler, entity, key, category);
                       original(handler, entity, key, category);
                     });
          g_factoryEntryHooked.store(true, std::memory_order_release);
          _MESSAGE("%s: factory entry hook installed at %p (full-coverage "
                   "一難 marking)",
                   kPluginName, reinterpret_cast<void*>(factoryEntry));
        }
      }

      if (g_createSwap.load(std::memory_order_acquire) &&
          !g_createHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t menuEnqueue =
            HookUtils::ScanIDAPattern(kMenuEnqueuePattern);
        if (menuEnqueue == 0) {
          complete = false;
        } else {
          // Pre-load identity swap at the menu-summon enqueue entry (see
          // kMenuEnqueuePattern). Wired through CaptureHookContext
          // (HookStub.asm). Shrine/world respawns never call this function,
          // so the mass-respawn freeze path is structurally unreachable.
          auto hook = safetyhook::create_inline(
              reinterpret_cast<void*>(menuEnqueue),
              reinterpret_cast<void*>(&CaptureHookContext));
          if (hook) {
            g_entryOriginal =
                reinterpret_cast<FnMenuEnqueue>(hook.trampoline().address());
            g_entryHook = std::move(hook);
            g_createHooked.store(true, std::memory_order_release);
            _MESSAGE("%s: menu enqueue hook installed at %p (pre-load swap)",
                     kPluginName, reinterpret_cast<void*>(menuEnqueue));
          } else {
            complete = false;
          }
        }
      }

      if (g_catalogSwap.load(std::memory_order_acquire) &&
          !g_catalogHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t catalogQuery =
            HookUtils::ScanIDAPattern(kCatalogQueryPattern);
        if (catalogQuery == 0) {
          complete = false;
        } else {
          auto hook = safetyhook::create_inline(
              reinterpret_cast<void*>(catalogQuery),
              reinterpret_cast<void*>(&CaptureCatalogContext));
          if (hook) {
            g_catalogOriginal =
                reinterpret_cast<FnCatalogQuery>(hook.trampoline().address());
            g_catalogHook = std::move(hook);
            g_catalogHooked.store(true, std::memory_order_release);
            _MESSAGE("%s: catalog query hook installed at %p (key swap)",
                     kPluginName, reinterpret_cast<void*>(catalogQuery));
          } else {
            complete = false;
          }
        }
      }

      // MapHook=0 (default): the placement table is randomised once by
      // MapSweepWorker and NO code is patched, so a pattern miss here is not a
      // failure and must not keep the installer spinning.
      if (g_mapHookEnabled.load(std::memory_order_acquire) &&
          g_mapBoss.load(std::memory_order_acquire) &&
          !g_mapHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t mapKeyRead =
            HookUtils::ScanIDAPattern(kMapPlacementKeyPattern);
        if (mapKeyRead == 0) {
          complete = false;
          _MESSAGE("%s: map placement pattern not found (will retry)",
                   kPluginName);
        } else {
          // Displaced bytes: mov r14d,[rax+04] (4) + test rbx,rbx (3).
          g_mapResume = mapKeyRead + 7;
          auto hook = safetyhook::create_inline(
              reinterpret_cast<void*>(mapKeyRead),
              reinterpret_cast<void*>(&MapBossHookStub));
          if (hook) {
            g_mapHook = std::move(hook);
            g_mapHooked.store(true, std::memory_order_release);
            _MESSAGE("%s: map placement hook installed at %p (source-level "
                     "swap, resume %p)",
                     kPluginName, reinterpret_cast<void*>(mapKeyRead),
                     reinterpret_cast<void*>(g_mapResume));
          } else {
            complete = false;
          }
        }
      }

      // MapPurple=1: hook the per-frame 一難 flag writer so EVERY entity is
      // covered, whichever path spawned it. Independent of MapHook — that one
      // decides the enemy key, this one only sets the purple bit.
      if (g_mapPurple.load(std::memory_order_acquire) &&
          !g_mapPurpleHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t flagWrite =
            HookUtils::ScanIDAPattern(kMapPurpleFlagPattern);
        if (flagWrite == 0) {
          complete = false;
          _MESSAGE("%s: purple flag pattern not found (will retry)",
                   kPluginName);
        } else {
          const std::uintptr_t site = flagWrite + kMapPurpleHookOffset;
          g_mapPurpleResume = site + kMapPurpleDisplaced;
          auto hook = safetyhook::create_inline(
              reinterpret_cast<void*>(site),
              reinterpret_cast<void*>(&MapPurpleHookStub));
          if (hook) {
            g_mapPurpleHook = std::move(hook);
            g_mapPurpleHooked.store(true, std::memory_order_release);
            _MESSAGE("%s: purple flag hook installed at %p (resume %p)",
                     kPluginName, reinterpret_cast<void*>(site),
                     reinterpret_cast<void*>(g_mapPurpleResume));
          } else {
            complete = false;
          }
        }
      }

      if (g_assetSwap.load(std::memory_order_acquire) &&
          !g_getResHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t getResId = HookUtils::ScanIDAPattern(
            kGetResIdByFileKtidPattern, 0, 1, 5);
        if (getResId == 0) {
          complete = false;
          _MESSAGE("%s: GetResIdByFileKtid pattern not found (will retry)",
                   kPluginName);
        } else {
          HookLambda(reinterpret_cast<FnGetResIdByFileKtid>(getResId),
                     [](void* assetIdManager,
                        std::uint32_t ktid) -> std::uint32_t {
                       // 0xFFFFFFFF is this function's "not found" answer —
                       // verified in its miss path (CE 2026-09-19, RVA
                       // 0x609F74: `or eax,-1` on both failure exits; the hit
                       // path computes an index via the sorted-ktid binary
                       // search). A ktid the CURRENT SCENE never registered
                       // answers 0xFFFFFFFF, and handing that to the loader
                       // breaks the asset, so fall back to the original id.
                       constexpr std::uint32_t kInvalidResId = 0xFFFFFFFFu;
                       if (g_assetTrace.load(std::memory_order_relaxed)) {
                         g_assetTraceTotal.fetch_add(1,
                                                     std::memory_order_relaxed);
                         const auto slot = (ktid * 2654435761u) >> 23;
                         if (g_assetTraceHits[slot].fetch_add(
                                 1, std::memory_order_relaxed) == 0) {
                           g_assetTraceKey[slot].store(
                               ktid, std::memory_order_relaxed);
                         }
                       }
                       const auto map =
                           g_ktidSwapMap.load(std::memory_order_acquire);
                       if (map && !map->empty()) {
                         if (const auto it = map->find(ktid);
                             it != map->end()) {
                           const std::uint32_t target = it->second;
                           const std::uint32_t redirected =
                               original(assetIdManager, target);
                           if (redirected != kInvalidResId) {
                             const auto n = g_assetSwapCount.fetch_add(
                                 1, std::memory_order_relaxed);
                             if (n < 64) {
                               _MESSAGE("%s: asset swap 0x%08X -> 0x%08X "
                                        "(resId 0x%08X)",
                                        kPluginName, ktid, target, redirected);
                             }
                             return redirected;
                           }
                           const auto m = g_assetMissCount.fetch_add(
                               1, std::memory_order_relaxed);
                           if (m < 16) {
                             _MESSAGE("%s: asset swap MISS 0x%08X -> 0x%08X "
                                      "(target unregistered in this scene; "
                                      "keeping the original)",
                                      kPluginName, ktid, target);
                           }
                         }
                       }
                       return original(assetIdManager, ktid);
                     });
          g_getResHooked.store(true, std::memory_order_release);
          _MESSAGE("%s: GetResIdByFileKtid hook installed at %p", kPluginName,
                   reinterpret_cast<void*>(getResId));
        }
      }

      if (g_identitySwap.load(std::memory_order_acquire) &&
          !g_identityHooked.load(std::memory_order_acquire)) {
        const std::uintptr_t initSlot =
            HookUtils::ScanIDAPattern(kIdentityInitPattern);
        if (initSlot == 0) {
          complete = false;
          _MESSAGE("%s: identity-init pattern not found (will retry)",
                   kPluginName);
        } else {
          // The return value MUST be forwarded: CreateSlot hands InitSlot's
          // RAX (the slot pointer) to its own caller with `mov rdx,rax`.
          using FnIdentityInit = void* (*)(void* slot, std::uint64_t identity,
                                           std::uint8_t flag);
          HookLambda(
              reinterpret_cast<FnIdentityInit>(initSlot),
              [](void* slot, std::uint64_t identity,
                 std::uint8_t flag) -> void* {
                std::uint64_t out = identity;
                const auto key = static_cast<std::uint32_t>(identity) >> 4;
                const auto target =
                    g_identityTo.load(std::memory_order_acquire);
                const auto fromCount =
                    g_identityFromCount.load(std::memory_order_acquire);
                if (key != 0 && target != 0 && key != target &&
                    IsListed(g_identityFrom, fromCount, key)) {
                  const auto tags =
                      g_identityTags.load(std::memory_order_acquire);
                  if (tags != nullptr) {
                    const auto it = tags->find(target);
                    if (it != tags->end()) {
                      out = (static_cast<std::uint64_t>(it->second) << 32) |
                            (static_cast<std::uint64_t>(target) << 4);
                      const auto n = g_identitySwapCount.fetch_add(
                          1, std::memory_order_relaxed);
                      if (n < 64) {
                        _MESSAGE("%s: identity swap 0x%05X -> 0x%05X "
                                 "(tag 0x%05X) slot=%p",
                                 kPluginName, key, target, it->second, slot);
                      }
                    }
                  }
                }
                return original(slot, out, flag);
              });
          g_identityHooked.store(true, std::memory_order_release);
          _MESSAGE("%s: identity-init hook installed at %p", kPluginName,
                   reinterpret_cast<void*>(initSlot));
        }
      }

      if (complete) {
        _MESSAGE("%s: all hooks online (attempt %d)", kPluginName,
                 attempt + 1);
        return;
      }
      Sleep(2000);
    }
    _MESSAGE("%s: hook installation gave up", kPluginName);
  } catch (const std::exception& e) {
    _MESSAGE("%s: installer exception: %s", kPluginName, e.what());
  } catch (...) {
    _MESSAGE("%s: installer unknown exception", kPluginName);
  }
}

}  // namespace

extern "C" __declspec(dllexport) bool nioh3_plugin_initialize(
    const Nioh3PluginInitializeParam* param) {
  try {
    _MESSAGE("%s: init start", kPluginName);
    LoadConfig(param);
    try {
      std::thread(MapSweepWorker).detach();
      std::thread(InstallHooksWithRetry).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: installer thread creation failed: %s", kPluginName,
               e.what());
    }
    try {
      std::thread(LogLoop).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: log thread creation failed: %s", kPluginName, e.what());
    }
    try {
      std::thread(RosterLoop).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: roster thread creation failed: %s", kPluginName, e.what());
    }
    try {
      std::thread(PairLoop).detach();
    } catch (const std::exception& e) {
      _MESSAGE("%s: pair thread creation failed: %s", kPluginName, e.what());
    }
    _MESSAGE("%s: initialized (hooks pending)", kPluginName);
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
    g_shutdown.store(true, std::memory_order_release);
  }
  return TRUE;
}
