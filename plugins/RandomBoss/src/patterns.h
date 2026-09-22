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
    "44 38 A6 E9 00 00 00 74 3A 8B 13 48 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 75 21";
// The `je` at pattern + 7 (RVA 0x54FB6B). It tests entity+0xE9 - the engine's
// "this entity is a powered-up one" mark. With the record's bit24 cleared (see
// g_targetFlags in main.cpp) the engine no longer sets that mark, so this jump
// fires and the spawn goes plain BEFORE it can reach the `jne` at +25. NOPing
// only the second gate (the historical behaviour) therefore did nothing for a
// bit24-clear placement - measured 2026-09-22, that was why clearing bit24 gave
// respawn but a plain enemy. Both gates have to be opened.
constexpr std::uintptr_t kRevivePlainBranchE9Offset = 7;
constexpr std::uintptr_t kRevivePlainBranchJumpOffset = 25;
constexpr std::size_t kRevivePlainBranchJumpSize = 2;

// The THIRD plain gate, and the one that actually demoted the target after a
// kill. Same function as kRevivePlainBranchPattern, but a different entry path:
// a second, near-identical copy of the two gates is emitted further down, so
// NOPing the first pair is not enough - the spawn runs through this copy too and
// stores `entity+0xEA = 1` (the MARK-PLAIN flag) on the way out.
//
// CE-verified 2026-09-22 on v2.0.2.0. The pattern lands at RVA 0x54FC00:
//
//   cmp  [rsi+0xE9],r12b         ; the same "powered-up entity" mark
//   je   -> skip                  ; +7, 74 45
//   mov  rax,[...]
//   mov  dl,[rsi+0x95]
//   mov  rcx,[rax] / mov rcx,[rcx+48]
//   call <query>                  ; "has the player already killed this one?"
//   test al,al
//   jne  -> store                 ; +36, 75 21   <-- this pattern's target
//   ... call <empower> / jmp over the store
//   mov  byte [rsi+0xEA],1        ; +71, 0xC6 86 EA 00 00 00 01 - MARK PLAIN
//
// Measured sequence, with kRevivePlainBranchPattern's two gates already live in
// memory:
//   1. clearing entity+0xEA on the target turned it PURPLE on screen (user-
//      confirmed), so +0xEA==0 is the purple state and nothing else was missing;
//   2. killing it turned it plain again - and +0xEA read back 1 - so some other
//      site set the flag, because the `jne` at RVA 0x54FB7D was already NOPed and
//      its store at 0x54FBA0 was therefore unreachable;
//   3. NOPing this `jne` LOOKED like the fix at the time (user-confirmed), but
//      that run also had the ichi-nan render gate enabled, so two variables moved
//      together and the attribution did not hold: the next session, with the
//      render gate off, came back plain after a kill again.
//
// So treat this as "one of the spawn-time demotion paths", not as the fix. The
// decisive patch is kKillPlainMarkPattern below: entity+0xEA is the appearance
// switch, and the store that latches it after a kill lives in a DIFFERENT
// function. Whether THIS gate is load-bearing on its own has never been isolated.
//
// Off with the rest of MapForceEmpower: it opens the plain path for every enemy,
// not just the target.
inline constexpr const char* kRevivePlainBranch2Pattern =
    "44 38 A6 E9 00 00 00 74 45 48 8B 05 ? ? ? ? 8A 96 95 00 00 00 48 8B 08 "
    "48 8B 49 48 E8 ? ? ? ? 84 C0 75 21";
// The `jne` at pattern + 36 (RVA 0x54FC24).
constexpr std::uintptr_t kRevivePlainBranch2JumpOffset = 36;
constexpr std::size_t kRevivePlainBranch2JumpSize = 2;

// The KILL-TIME mark-plain store, and the one that finally made "purple AND
// respawns" hold through a kill. The two patterns above only gate what happens
// when an entity is SPAWNED; this one is what the engine does when the player
// KILLS the entity, and it is in a completely different function - which is why
// looking only at the spawn-time decision missed it twice.
//
// CE-verified 2026-09-22 on v2.0.2.0. The pattern lands at RVA 0x2A224F:
//
//   test rdi,rdi                    ; rdi = the entity
//   je   -> +7
//   mov  byte [rdi+0xEA],1          ; +5, 0xC6 87 EA 00 00 00 01 - MARK PLAIN
//
// There is no other guard: killing the entity latches entity+0xEA to 1, and
// entity+0xEA == 1 is what renders it plain. Found with a hardware WRITE
// breakpoint on the live entity's +0xEA - the single hit reported
// RDI = the entity itself and RIP one instruction past this store.
//
// Applied by writing 0x00 over the immediate byte instead of NOPing the store:
// writing zero both leaves the flag alone AND actively clears it, so an entity
// that was latched plain by an earlier kill is repaired by the next kill rather
// than staying plain forever.
//
// Evidence chain (all user-confirmed in game, same session, in this order):
//   1. hardware write breakpoint on entity+0xEA, kill the entity ->
//      exactly one hit, at RVA 0x2A2254, RDI = that entity, and a read-back
//      showed +0xEA had become 1 and the enemy had gone plain;
//   2. byte 0x01 -> 0x00 at RVA 0x2A225A (CE, live), clear +0xEA once ->
//      kill the entity, revive at a shrine -> the enemy comes back PURPLE.
//
// Note this is NOT a "spawn" gate: entity+0xEA == 0 is the purple state
// (measured both with the render gate on and off), so all this patch has to do
// is stop anything from writing 1 there after a kill.
inline constexpr const char* kKillPlainMarkPattern =
    "48 85 FF 74 07 C6 87 EA 00 00 00 01";
// The 0x01 immediate at pattern + 11 (RVA 0x2A225A).
constexpr std::uintptr_t kKillPlainMarkImmOffset = 11;

// REMOVED 2026-09-22: kIchiNanRenderPattern / kIchiNanRenderJumpOffset / ...Size,
// and the MapPurpleRender ini key with them.
//
// That patch NOPed the `je` at RVA 0x9A2188 - the predicate the renderer asks "is
// this the powered-up variant?". It was believed to be the other half of the
// purple answer. It is not, and it was falsified by isolation:
//
//   * render gate OFF + only entity+0xEA cleared  -> the enemy is PURPLE;
//   * render gate ON  + entity+0xEA == 1          -> the enemy is PLAIN.
//
// The session that first "confirmed" it had the gate enabled AND the kill-time
// writer (kKillPlainMarkPattern) still unpatched, so two variables moved together
// and the conclusion did not hold. entity+0xEA is the appearance switch; this
// predicate is not.
//
// The predicate itself is real and stays recorded, just not patched - see
// analysis/purple_variant_re_2026-09-22.md section 35.2: it reads record+0x08
// bit24 at RVA 0x9A2181 and its only caller is RVA 0x2B4949. Do NOT re-add it.

// FALSIFIED 2026-09-22, do not re-add: the sibling jump at patternAddress + 47
// (RVA 0x54FB9E, `EB 07` - the `jmp` that skips `mov byte [rsi+0xEA], 1`) is NOT
// a lost "respawn marker". `entity+0xEA == 1` is the engine's MARK-PLAIN flag -
// see the "mark plain" note on kRevivePlainBranchPattern above. Retargeting that
// jump to +0 made the empower call fall through into the store, and the user
// reported in game that every purple enemy turned plain. Reverted.
//
// The general lesson: the two branches here are (empower) and (mark plain), and
// the plain branch's only side effect is what MAKES it plain. Running both does
// not combine their effects, it cancels them.

// MapIgnoreBlocked (ini key, default 0 = off). A DIFFERENT gate from the one
// above: that one decides how an enemy looks (plain vs purple), this one decides
// whether the placement is allowed to produce a live enemy at all.
//
// CE-verified 2026-09-22 on the v2.0.2.0 build, inside the per-pass "disable"
// function (RVA 0x133D18, entry `mov [rsp+10],rsi / push rdi / sub rsp,20`):
//
//   mov  eax,[rbx+0x8D4]            ; RVA 0x133F65 - a small placement state enum
//   cmp  eax,3                     ; RVA 0x133F6B
//   jnl  <write AE14 = 0>          ; RVA 0x133F6E <-- THIS JUMP (offset 9 here)
//   cmp  eax,2                     ; RVA 0x133F74
//   jl   <skip>                    ; RVA 0x133F77
//   cmp  byte [rbx+0x107],0
//   je   <skip>
//   cmp  byte [rbx+0x859],0
//   je   <write AE14 = 0>          ; RVA 0x133F89
//   <skip>                         ; RVA 0x133F8F
//
// placement+0x8D4 == 3 marks a placement the load never finished building. The
// engine then keeps placement+0xAE14 (the enable byte) pinned at 0, so the
// placement is never collected into the active set: the enemy is either absent,
// or - if the flags are forced afterwards, which is what a live CE edit does - a
// half-spawn with idle animation, no AI, no movement and no attack, because its
// component sub-objects were never created. Only the load-time path can build
// them, so the fix has to be in place BEFORE a load, not applied afterwards.
//
// NOPing the jump makes 3 fall through to the +0x107 / +0x859 tests. A placement
// stuck at 3 has +0x107 == 0, so it takes the <skip> branch and +0xAE14 is left
// alone; the normal enable pass (RVA 0x840FC) can then set it to 1.
inline constexpr const char* kBlockedPlacementPattern =
    "8B 83 D4 08 00 00 83 F8 03 0F 8D ? ? ? ? 83 F8 02";
constexpr std::uintptr_t kBlockedPlacementJumpOffset = 9;
constexpr std::size_t kBlockedPlacementJumpSize = 6;
