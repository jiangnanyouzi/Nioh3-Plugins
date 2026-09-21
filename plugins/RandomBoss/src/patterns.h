// patterns.h - AOB signatures and hook offsets for the engine functions
// RandomBoss hooks.
//
// Split out of main.cpp verbatim: these are pure data (byte patterns plus two
// hook offsets) with no dependency on plugin state, so they can live in their
// own header. inline constexpr is deliberate: it makes each pattern a single
// object with external linkage, so every translation unit that includes this
// header shares one address instead of getting a private copy.
//
// No namespace wrapper here on purpose - the includer decides. main.cpp pulls
// this in from inside its anonymous namespace, which is where these names used
// to live.
#pragma once

#include <cstdint>

// Component factory core (v2.0.1.0 RVA 0x5FC500). At function entry r8d is
// the spawn parameter id; the constructor receives it as r8. Swapping r8
// here rewires the battle-data component. Whether that alone changes the
// visible model is exactly what this test plugin determines.
// NOTE: the pattern starts at function+6, NOT the entry. The entry bytes
// are single-use: whichever plugin hooks factory core first overwrites them
// (safetyhook inline patch), destroying any entry-anchored pattern for
// every other plugin. The +6 tail survives cohabitation and is still unique.
inline constexpr const char* kFactoryCorePattern =
    "48 8B D9 4D 85 C9 74 22 41 8B 41 68 D1 E8 24 01";
// Assembly main (v2.0.1.0 RVA 0x247960). Entry: RCX = output entity, RDX =
// source request object. Discover mode scans [rdx, rdx+0x400) for dword
// values inside the enemy-id range and logs (offset, value) pairs, so the
// identity field offset calibrates itself from a single load storm.
inline constexpr const char* kAssemblyMainPattern =
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
inline constexpr const char* kFactoryEntryPattern =
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
inline constexpr const char* kMenuEnqueuePattern =
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
inline constexpr const char* kOrchestratorPattern =
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
inline constexpr const char* kGetResIdByFileKtidPattern =
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
inline constexpr const char* kCatalogQueryPattern =
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
inline constexpr const char* kIdentityInitPattern =
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
inline constexpr const char* kMapPlacementKeyPattern =
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
inline constexpr const char* kMapPurpleFlagPattern =
    "8A 8F 40 19 00 00 C0 E9 06 F6 D1 40 22 CE 88 88 E8 00 00 00";
constexpr std::uintptr_t kMapPurpleHookOffset = 14;
constexpr std::uintptr_t kMapPurpleDisplaced = 6;

// "Already-killed placement" branch in the entity activation path. v2.0.2.0:
// pattern lands at RVA 0x54FB6F, the two-byte `jne` at RVA 0x54FB7D (offset 14).
// Found 2026-09-21 with a CE hardware WRITE breakpoint on entity+0xEA, which
// reported this site with RBX = the placement record and RSI = the entity:
//
//   mov  rcx,[7FF75C201848]        ; the manager
//   mov  edx,[rbx]                 ; RBX = placement record, EDX = record+0x00 instanceId
//   call <lookup>                  ; bool f(manager, instanceId)
//   test al,al
//   jne  +0x21                     <-- THIS JUMP (offset 14 in the pattern)
//   ...call <empower>...           ; al == 0 -> the empowered (紫皮) path
//   mov  byte [rsi+0xEA], 1        ; al != 0 -> mark plain, SKIP the empower call
//
// The lookup answers "has the player already killed this placement?" — hence a
// Gozuki the user had killed once came back plain forever. That is a game rule,
// not a plugin bug, and it is why every record-side fix failed: the engine
// re-decides here, per spawn, from the record's instanceId alone. NOPing the
// jump sends every spawn down the empowered path. Verified in game 2026-09-21:
// the two bytes were NOPed with CE, the enemy was respawned, and it turned
// purple on screen. Off by default (MapForceEmpower) because it also affects
// every OTHER revived enemy in the game, not just the target.
inline constexpr const char* kRevivePlainBranchPattern =
    "48 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 75 21 48 8B 05";
constexpr std::uintptr_t kRevivePlainBranchJumpOffset = 14;
constexpr std::size_t kRevivePlainBranchJumpSize = 2;
