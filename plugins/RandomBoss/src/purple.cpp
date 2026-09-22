// purple.cpp - the powered-up "ichi-nan" (一難) variant for RandomBoss.
//
// Split out of main.cpp. Everything specific to the purple variant lives here:
// the record-side variant-flag rewrite, the entity-side mark, and the two hook
// bodies that drive them.
//
// See analysis/experiments/purple_HANDOFF_NEXT.md for the full investigation,
// and purple_HANDOFF_KILLEDENEMY.md for the open question "why does a killed
// enemy come back plain?" (that file also records that the factory-entry probe
// below is currently silent - fix it before trusting any coverage claim).
// In short: the variant is decided before the entity is built, so it has to be
// correct in the placement record; the runtime mark at entity+0xE8 is what the
// engine itself maintains for its own powered-up placements, and it is sticky
// because the engine's recurring writer only touches the low byte.
#include "state.h"
#include "patterns.h"

namespace {

// Entity layout. Private to this module - nothing else reads these offsets.
constexpr std::uintptr_t kEntityVariantOffset = 0xE8;
constexpr std::uintptr_t kEntityKeyOffset = 0x28;

// Unconditional sample cap for the factory-entry probe (see
// MapPurpleFactoryEntryBody). Small on purpose: this is a one-run diagnostic
// whose only job is to say whether the hook fires at all and what its raw
// arguments look like.
constexpr std::uint32_t kFepRawLogMax = 24;

// One capped line per successful mark.
constexpr std::uint32_t kMapPurpleMarkLogMax = 512;

}  // namespace

// True when `key` is one of the enemies this plugin can have placed: a member of
// the global MapPool or of any per-source MapPool_<SRC> pool.
//
// This is the ONLY definition of "ours". Purple marking used to test
// `key == g_targetId` - a single id read from the ini's separate TargetId key -
// which meant two places stated the same thing and could disagree. When they did
// (MapPool changed to another enemy, TargetId left behind) every spawn came out
// plain and the only visible symptom was the `purple mark` counter sitting at
// zero, which reads as "the mechanic broke" rather than "TargetId is stale".
// TargetId is gone as of 2026-09-21, so membership is now the whole test: it
// covers a multi-target pool, and a pool edited while the game runs (hot reload).
bool IsMapTargetKey(std::uint32_t key) {
  if (key == 0) {
    return false;
  }
  const auto listed = [key](const std::atomic<std::uint32_t>* list,
                            std::size_t count) {
    if (count > kMaxListEntries) {
      count = kMaxListEntries;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (list[i].load(std::memory_order_relaxed) == key) {
        return true;
      }
    }
    return false;
  };
  if (listed(g_mapPool, g_mapPoolCount.load(std::memory_order_acquire))) {
    return true;
  }
  std::size_t slots = g_mapKeyPoolCount.load(std::memory_order_acquire);
  if (slots > kMapPoolSlots) {
    slots = kMapPoolSlots;
  }
  for (std::size_t slot = 0; slot < slots; ++slot) {
    if (g_mapKeyPools[slot].source.load(std::memory_order_relaxed) == 0) {
      continue;  // free slot
    }
    if (listed(g_mapKeyPools[slot].keys,
               g_mapKeyPools[slot].count.load(std::memory_order_acquire))) {
      return true;
    }
  }
  return false;
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
  // target: bit24 clear/set as g_targetFlags has it, bit16 forced clear.
  //
  // bit24 is NOT the purple switch, despite what earlier rounds assumed - it is
  // the engine's "ichi-nan / one-time placement" flag, and a record carrying it
  // is rebuilt as a shell after the enemy dies (measured 2026-09-22: bit24 set
  // <=> placement+0x0C0 == 0, across all 38 objects with no exceptions). See the
  // long note on g_targetFlags in main.cpp. g_targetFlags now carries the
  // ordinary value, so this function clears bit24 rather than setting it, and
  // the visible purple comes from MapPurpleMark's runtime write instead.
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

std::atomic<std::uint32_t> g_mapPurple{0};
std::atomic<std::uint64_t> g_mapPurpleFlagged{0};

// MapForceEmpower (ini key, DEFAULT 0 = off) and its one-shot patch flag. See
// kRevivePlainBranchPattern in patterns.h for the mechanism and the live proof.
std::atomic_bool g_mapForceEmpower{false};
std::atomic_bool g_reviveBranchPatched{false};

// NOPs the two-byte `jne` at patternAddress + kRevivePlainBranchJumpOffset, so
// every spawn takes the empowered branch instead of the "you already killed
// this one" plain branch. Returns true when the patch is in place.
//
// Do NOT also neuter the `EB 07` at patternAddress + 47. That jump skips the
// plain branch's `mov byte [rsi+0xEA], 1`, but +0xEA is the MARK-PLAIN flag, not
// a respawn flag: letting the empower call fall through into that store cancels
// the empowerment and every enemy comes out plain. Tried 2026-09-22, reverted
// the same day. See the FALSIFIED note in patterns.h.
//
// Deliberately a raw two-byte code patch rather than a safetyhook: the site is
// a conditional jump whose two outcomes are both inside the same function, so
// there is no return value to intercept and no sane call to hook. The bytes are
// validated before writing, so a game update that reshapes the sequence is
// reported and skipped instead of corrupting the function. Memory-only: nothing
// is written to disk and the patch is gone when the process exits.
bool ApplyRevivePlainBranchPatch(std::uintptr_t patternAddress) {
  if (!g_mapForceEmpower.load(std::memory_order_acquire)) {
    return false;
  }
  if (!g_mapPurple.load(std::memory_order_acquire)) {
    return false;  // same switch as the rest of the purple support
  }
  if (g_reviveBranchPatched.load(std::memory_order_acquire)) {
    return true;
  }
  if (patternAddress == 0) {
    return false;
  }
  auto* jump = reinterpret_cast<std::uint8_t*>(
      patternAddress + kRevivePlainBranchJumpOffset);
  auto* e9 = reinterpret_cast<std::uint8_t*>(
      patternAddress + kRevivePlainBranchE9Offset);
  __try {
    if (jump[0] != 0x75 || jump[1] != 0x21) {
      _MESSAGE("%s: revive/plain branch reads %02X %02X, expected 75 21 - NOT "
               "patched (game build changed?)",
               kPluginName, jump[0], jump[1]);
      return false;
    }
    // Second gate. Validate before writing either one.
    if (e9[0] != 0x74 || e9[1] != 0x3A) {
      _MESSAGE("%s: revive entity-mark branch reads %02X %02X, expected 74 3A - "
               "NOT patched (game build changed?)",
               kPluginName, e9[0], e9[1]);
      return false;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(jump, 2, PAGE_EXECUTE_READWRITE, &oldProtect) == 0 ||
        VirtualProtect(e9, 2, PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
      _MESSAGE("%s: revive/plain branch patch failed (VirtualProtect)", kPluginName);
      return false;
    }
    jump[0] = 0x90;
    jump[1] = 0x90;
    e9[0] = 0x90;
    e9[1] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(jump, 2, oldProtect, &ignored);
    VirtualProtect(e9, 2, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), jump, 2);
    FlushInstructionCache(GetCurrentProcess(), e9, 2);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    _MESSAGE("%s: revive/plain branch patch raised - not patched", kPluginName);
    return false;
  }
  g_reviveBranchPatched.store(true, std::memory_order_release);
  // Print the RVAs too, not just the absolute address: the pattern is a wildcard
  // match, so "which instruction did it actually land on" has to be checkable
  // from the log. The expected sites are RVA 0x54FB6B and 0x54FB7D in the
  // v2.0.2.0 build.
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  _MESSAGE("%s: MapForceEmpower: NOPed BOTH plain branches at %p (RVA %llX, "
           "74 3A -> 90 90, entity+0xE9 gate) and %p (RVA %llX, 75 21 -> 90 90, "
           "already-killed lookup gate); every spawn now takes the empowered "
           "path regardless of the record's ichi-nan bit",
           kPluginName, reinterpret_cast<void*>(e9),
           static_cast<unsigned long long>(
               reinterpret_cast<std::uintptr_t>(e9) - base),
           reinterpret_cast<void*>(jump),
           static_cast<unsigned long long>(
               reinterpret_cast<std::uintptr_t>(jump) - base));
  return true;
}

// MapIgnoreBlocked (ini key, DEFAULT 0 = off) and its one-shot patch flag. See
// kBlockedPlacementPattern in patterns.h for the mechanism and the evidence.
std::atomic_bool g_mapIgnoreBlocked{false};
std::atomic_bool g_blockedPlacementPatched{false};

// NOPs the six-byte `jnl` at patternAddress + kBlockedPlacementJumpOffset. That
// jump is the "placement+0x8D4 >= 3 -> write placement+0xAE14 = 0" arm, so NOPing
// it stops the engine from permanently disabling a placement whose state enum is
// stuck at 3. Returns true when the patch is in place.
//
// Same reasoning as ApplyRevivePlainBranchPatch above: the site is a conditional
// jump with both outcomes inside the same function, so there is no call to hook.
// The bytes are validated before writing, so a game update that reshapes the
// sequence is reported and skipped instead of corrupting the function. Memory
// only: nothing is written to disk and the patch is gone when the process exits.
bool ApplyBlockedPlacementPatch(std::uintptr_t patternAddress) {
  if (!g_mapIgnoreBlocked.load(std::memory_order_acquire)) {
    return false;
  }
  if (g_blockedPlacementPatched.load(std::memory_order_acquire)) {
    return true;
  }
  if (patternAddress == 0) {
    return false;
  }
  auto* jump = reinterpret_cast<std::uint8_t*>(
      patternAddress + kBlockedPlacementJumpOffset);
  __try {
    // `jnl rel32`: 0F 8D + 4 displacement bytes.
    if (jump[0] != 0x0F || jump[1] != 0x8D ||
        jump[kBlockedPlacementJumpSize - 1] != 0x00) {
      _MESSAGE("%s: blocked-placement jump reads %02X %02X ... %02X, expected "
               "0F 8D ... 00 - NOT patched (game build changed?)",
               kPluginName, jump[0], jump[1],
               jump[kBlockedPlacementJumpSize - 1]);
      return false;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(jump, kBlockedPlacementJumpSize, PAGE_EXECUTE_READWRITE,
                       &oldProtect) == 0) {
      _MESSAGE("%s: blocked-placement patch failed (VirtualProtect)", kPluginName);
      return false;
    }
    for (std::size_t i = 0; i < kBlockedPlacementJumpSize; ++i) {
      jump[i] = 0x90;
    }
    DWORD ignored = 0;
    VirtualProtect(jump, kBlockedPlacementJumpSize, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), jump, kBlockedPlacementJumpSize);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    _MESSAGE("%s: blocked-placement patch raised - not patched", kPluginName);
    return false;
  }
  g_blockedPlacementPatched.store(true, std::memory_order_release);
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  _MESSAGE("%s: MapIgnoreBlocked: blocked-placement jump NOPed at %p (RVA %llX, "
           "0F 8D xx xx xx xx -> 90 x6); placements stuck at +0x8D4 == 3 are no "
           "longer disabled",
           kPluginName, reinterpret_cast<void*>(jump),
           static_cast<unsigned long long>(
               reinterpret_cast<std::uintptr_t>(jump) - base));
  return true;
}

// Makes the ichi-nan RENDER gate answer "yes" on every placement, without
// touching the record. See kIchiNanRenderPattern for the mechanism and the
// in-game verification.
//
// Same switch as the revive/plain branch above (MapForceEmpower): that patch
// decides that a spawn takes the empowered path, this one decides that the
// result is still rendered as powered-up when the record carries no ichi-nan
// bit. Both are needed for "purple AND comes back after a kill" - the record
// bit alone cannot express that, because the same bit also makes the engine
// build the placement as a one-time shell.
//
// Idempotent through a function-local flag: the installer retries its whole
// sequence until it stops making progress, and the write itself would be a
// harmless no-op, but the flag keeps the log honest.
bool ApplyIchiNanRenderPatch(std::uintptr_t patternAddress) {
  static std::atomic_bool patched{false};
  if (!g_mapForceEmpower.load(std::memory_order_acquire)) {
    return false;
  }
  if (patched.load(std::memory_order_acquire)) {
    return true;
  }
  if (patternAddress == 0) {
    return false;
  }
  auto* jump = reinterpret_cast<std::uint8_t*>(
      patternAddress + kIchiNanRenderJumpOffset);
  __try {
    if (jump[0] != 0x74 || jump[1] != 0xE8) {
      _MESSAGE("%s: ichi-nan render gate reads %02X %02X, expected 74 E8 - NOT "
               "patched (game build changed?)",
               kPluginName, jump[0], jump[1]);
      return false;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(jump, 2, PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
      _MESSAGE("%s: ichi-nan render patch failed (VirtualProtect)", kPluginName);
      return false;
    }
    jump[0] = 0x90;
    jump[1] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(jump, 2, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), jump, 2);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    _MESSAGE("%s: ichi-nan render patch raised - not patched", kPluginName);
    return false;
  }
  patched.store(true, std::memory_order_release);
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  _MESSAGE("%s: MapForceEmpower: ichi-nan render gate NOPed at %p (RVA %llX, "
           "74 E8 -> 90 90); a placement whose record carries NO ichi-nan bit "
           "is still rendered as the powered-up variant",
           kPluginName, reinterpret_cast<void*>(jump),
           static_cast<unsigned long long>(
               reinterpret_cast<std::uintptr_t>(jump) - base));
  return true;
}

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
  std::uint32_t entityKey = 0;
  __try {
    entityKey = *reinterpret_cast<const volatile std::uint32_t*>(
        entity + kEntityKeyOffset);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
  if (!IsMapTargetKey(entityKey)) {
    return;
  }
  __try {
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
      if (n < kMapPurpleMarkLogMax) {
        const std::uint32_t id =
            *reinterpret_cast<const volatile std::uint32_t*>(entity + 0x20);
        // dE0/dF0 are the two fields that separate a placement the engine
        // instantiated normally from one a killed enemy came back through. They
        // are logged here so the "which entity is the plain one" question can
        // be answered from the log instead of from guesswork.
        const std::uint32_t dE0 =
            *reinterpret_cast<const volatile std::uint32_t*>(entity + 0xE0);
        const std::uint32_t dF0 =
            *reinterpret_cast<const volatile std::uint32_t*>(entity + 0xF0);
        _MESSAGE("%s: purple mark #%u ent=%p id=%X flag=%02X->%02X dE0=%X "
                 "dF0=%X",
                 kPluginName, n, reinterpret_cast<void*>(entity), id, before,
                 static_cast<unsigned>(before | 0x01), dE0, dF0);
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
      key == 0 || !IsMapTargetKey(key)) {
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
  // --- unconditional probe, deliberately FIRST -------------------------------
  // The previous version returned before its counter for every non-matching
  // key, so the silent log it produced could not tell the two candidate causes
  // apart: (H2) the wrapper is not on the creation path at all, or (H1) it is
  // called but the key argument is not the raw catalogue key and every call is
  // rejected by the mismatch test. Both look exactly like "fep = 0". Counting
  // and sampling BEFORE any filter makes one run settle it.
  {
    static std::atomic<std::uint32_t> rawSeen{0};
    const std::uint32_t raw =
        rawSeen.fetch_add(1, std::memory_order_relaxed);
    if (raw < kFepRawLogMax) {
      const auto probeEnt = reinterpret_cast<std::uintptr_t>(entity);
      std::uint32_t echo = 0;
      std::uint32_t dE8 = 0xFFFFFFFFu;
      __try {
        if (probeEnt >= 0x10000000000ULL && probeEnt < 0x800000000000ULL) {
          echo = *reinterpret_cast<const volatile std::uint32_t*>(
              probeEnt + kEntityKeyOffset);
          dE8 = *reinterpret_cast<const volatile std::uint32_t*>(
              probeEnt + kEntityVariantOffset);
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      _MESSAGE("%s: fep raw #%u key=%X key>>4=%X key<<4=%X cat=%X handler=%p "
               "ent=%p echo28=%X dE8=%X",
               kPluginName, raw, key, key >> 4, key << 4, category, handler,
               entity, echo, dE8);
    }
  }
  if (g_mapPurple.load(std::memory_order_relaxed) == 0 || entity == nullptr ||
      key == 0) {
    return;
  }
  const auto ent = reinterpret_cast<std::uintptr_t>(entity);
  if (ent < 0x10000000000ULL || ent >= 0x800000000000ULL) {
    return;
  }
  // H1: R8D may be the raw catalogue key OR the embedded id (key << 4), and the
  // entity's own +0x28 was measured carrying the RAW key (2026-09-21). Accept
  // every spelling of "this is one of ours" so the probe cannot stay dead on a
  // convention we merely guessed wrong. Now that the test is pool membership
  // rather than equality with one id, each spelling is checked on its own.
  if (!IsMapTargetKey(key) && !IsMapTargetKey(key >> 4) &&
      !IsMapTargetKey(key << 4)) {
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
    keyEcho = *reinterpret_cast<const volatile std::uint32_t*>(
        ent + kEntityKeyOffset);
    d38 = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0x38);
    d90 = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0x90);
    d94 = *reinterpret_cast<const volatile std::uint32_t*>(ent + 0x94);
    dE8 = *reinterpret_cast<const volatile std::uint32_t*>(
        ent + kEntityVariantOffset);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
  // keyEcho is the entity's own copy of the catalogue key; a mismatch means
  // this object is not (yet) the enemy we think it is. Both spellings are
  // accepted here too, for the same reason as above.
  if (!IsMapTargetKey(keyEcho) && !IsMapTargetKey(keyEcho >> 4)) {
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

