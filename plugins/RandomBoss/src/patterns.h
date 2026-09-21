// patterns.h - AOB signatures and hook offsets for the engine functions
// RandomBoss hooks.
//
// Split out of main.cpp verbatim: these are pure data (byte patterns plus their
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
