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

// kFepRawLogMax was removed 2026-09-22 with the unconditional probe it capped.

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
// Defined in src/maps.cpp. Declared here so the hook-path write below can use the
// SAME validator the sweep uses to recognise a placement record - one definition
// of "this dword is a placement flag word" for both callers.
bool MapRecordFlagsLookLikePlacement(std::uint32_t flags);

// Placement-record field offsets. See the long note in state.h: AOB signatures
// pin where the CODE is, never where a struct field is, so these are derived from
// kMapPlacementKeyPattern's disp8 (main.cpp: DeriveRecordOffsets) rather than
// hardcoded, and the constants here are only the v2.0.2.0 measurements used until
// that derivation runs.
std::atomic<std::uint32_t> g_recordKeyOffset{4};
std::atomic<std::uint32_t> g_recordFlagsOffset{8};
std::atomic_bool g_recordOffsetsDerived{false};

// Counts writes refused because the dword at the flags offset did not look like a
// placement record's flag word. Capped, and deliberately a different counter from
// g_mapPurpleFlagWrites so "the plugin stopped touching records" is distinguishable
// from "the plugin had nothing to do".
std::atomic<std::uint32_t> g_recordShapeRejects{0};

void ApplyTargetFlags(std::uintptr_t record) {
  if (!g_mapPurple.load(std::memory_order_acquire)) {
    return;
  }
  // Offset from the derived layout, not the literal 8.
  const std::uint32_t flagsOffset =
      g_recordFlagsOffset.load(std::memory_order_acquire);
  auto* flagWord = reinterpret_cast<volatile std::uint32_t*>(record + flagsOffset);
  const std::uint32_t current = *flagWord;

  // Refuse to write a dword that does not look like a placement flag word.
  //
  // This is the one thing a signature cannot do for us. Every code site this
  // plugin patches is pinned by a byte pattern, but record+0x08 is a hardcoded
  // STRUCT offset: if a game update inserts or reorders a field, the patterns can
  // still match perfectly while this offset now points at something else, and the
  // write below would rewrite the high 16 bits of an unrelated dword - bounded,
  // but completely invisible. The sweep has validated records this way since the
  // beginning (maps.cpp); the hook paths never did.
  if (!MapRecordFlagsLookLikePlacement(current)) {
    const std::uint32_t n =
        g_recordShapeRejects.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      _MESSAGE("%s: REFUSED to write record %p: the dword at +0x%X reads %08X, "
               "which is not a placement flag word - the record layout probably "
               "moved (offset is derived; see DeriveRecordOffsets)",
               kPluginName, reinterpret_cast<void*>(record), flagsOffset, current);
    }
    return;
  }

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
  // ordinary value, so this function clears bit24 rather than setting it.
  //
  // Appearance is decided somewhere else entirely, and not by this word:
  // entity+0xEA == 0 is the purple state, entity+0xEA == 1 forces plain. See
  // kKillPlainMarkPattern.
  const std::uint32_t updated =
      (current & 0x0000FFFFu) | (desired & 0xFFFF0000u);
  *flagWord = updated;
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
// Its own one-shot flag: the third gate lives at a different pattern address and
// the two are written independently, so a build that reshapes one of the two
// code copies still gets the other applied. See kRevivePlainBranch2Pattern.
std::atomic_bool g_reviveBranch2Patched{false};
// The kill-time mark-plain store is a third site again, so it gets a third flag.
// See kKillPlainMarkPattern.
std::atomic_bool g_killPlainMarkPatched{false};

// MapPurpleRender / ApplyIchiNanRenderPatch / kIchiNanRenderPattern were REMOVED
// 2026-09-22: the patch was falsified by isolation (see the tombstone in
// patterns.h). entity+0xEA is the appearance switch, not that predicate.

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
    // Opcode only. The signature wildcards both displacements, and a conditional
    // branch's displacement carries no meaning for "NOP this branch" - the 25
    // bytes of surrounding code are what prove this is the right site. Requiring
    // the old 0x21 / 0x3A here would re-introduce exactly the drift the wildcards
    // were added to remove.
    if (jump[0] != 0x75) {
      _MESSAGE("%s: revive/plain branch reads %02X, expected 75 (jne) - NOT "
               "patched (game build changed?)",
               kPluginName, jump[0]);
      return false;
    }
    // Second gate. Validate before writing either one.
    if (e9[0] != 0x74) {
      _MESSAGE("%s: revive entity-mark branch reads %02X, expected 74 (je) - "
               "NOT patched (game build changed?)",
               kPluginName, e9[0]);
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

// MapForceEmpower, third gate: NOPs the two-byte `jne` at patternAddress +
// kRevivePlainBranch2JumpOffset, in the sibling copy of the same plain/empower
// decision. Opening only the first pair of gates is NOT enough - the spawn runs
// through this copy as well, and this copy stores `entity+0xEA = 1` (the
// MARK-PLAIN flag) on its way out. That store is what turned the target plain
// again after a kill even though it had been purple moments earlier. See
// kRevivePlainBranch2Pattern for the measured sequence that isolated it.
//
// Returns true when the patch is in place.
bool ApplyRevivePlainBranch2Patch(std::uintptr_t patternAddress) {
  if (!g_mapForceEmpower.load(std::memory_order_acquire)) {
    return false;
  }
  if (!g_mapPurple.load(std::memory_order_acquire)) {
    return false;  // same switch as the rest of the purple support
  }
  if (g_reviveBranch2Patched.load(std::memory_order_acquire)) {
    return true;
  }
  if (patternAddress == 0) {
    return false;
  }
  auto* jump = reinterpret_cast<std::uint8_t*>(
      patternAddress + kRevivePlainBranch2JumpOffset);
  __try {
    if (jump[0] != 0x75) {
      _MESSAGE("%s: revive/plain branch #2 reads %02X, expected 75 (jne) - NOT "
               "patched (game build changed?)",
               kPluginName, jump[0]);
      return false;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(jump, 2, PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
      _MESSAGE("%s: revive/plain branch #2 patch failed (VirtualProtect)",
               kPluginName);
      return false;
    }
    jump[0] = 0x90;
    jump[1] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(jump, 2, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), jump, 2);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    _MESSAGE("%s: revive/plain branch #2 patch raised - not patched", kPluginName);
    return false;
  }
  g_reviveBranch2Patched.store(true, std::memory_order_release);
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  _MESSAGE("%s: MapForceEmpower: NOPed the THIRD plain gate at %p (RVA %llX, "
           "75 21 -> 90 90, second already-killed lookup gate); without it the "
           "sibling copy still stores entity+0xEA = 1 and the enemy comes back "
           "plain after a kill",
           kPluginName, reinterpret_cast<void*>(jump),
           static_cast<unsigned long long>(
               reinterpret_cast<std::uintptr_t>(jump) - base));
  return true;
}

// MapForceEmpower, KILL-TIME gate: rewrites the `mov byte [rdi+0xEA], 1` at
// patternAddress + kKillPlainMarkImmOffset so it stores 0 instead. This is the
// store that latches an entity plain when the player kills it, and it lives in a
// different function from both spawn-time gates - see kKillPlainMarkPattern for
// the hardware-breakpoint evidence and the in-game confirmation.
//
// Writing 0x00 rather than NOPing means an entity already latched plain by an
// earlier kill is repaired by the next kill instead of staying plain.
//
// Returns true when the patch is in place.
bool ApplyKillPlainMarkPatch(std::uintptr_t patternAddress) {
  if (!g_mapForceEmpower.load(std::memory_order_acquire)) {
    return false;
  }
  if (!g_mapPurple.load(std::memory_order_acquire)) {
    return false;  // same switch as the rest of the purple support
  }
  if (g_killPlainMarkPatched.load(std::memory_order_acquire)) {
    return true;
  }
  if (patternAddress == 0) {
    return false;
  }
  auto* imm = reinterpret_cast<std::uint8_t*>(
      patternAddress + kKillPlainMarkImmOffset);
  __try {
    // Confirm we are looking at the immediate byte of `mov byte [rdi+0xEA], ?`,
    // rather than validating its VALUE: the signature wildcards that byte, and a
    // re-run in a process where the patch is already in place would legitimately
    // read 0x00 here. The six shape bytes are what prove the instruction.
    if (imm[-6] != 0xC6 || imm[-5] != 0x87 || imm[-4] != 0xEA ||
        imm[-3] != 0x00 || imm[-2] != 0x00 || imm[-1] != 0x00) {
      _MESSAGE("%s: kill-time mark-plain store shape reads %02X %02X %02X %02X "
               "%02X %02X, expected C6 87 EA 00 00 00 - NOT patched (game build "
               "changed?)",
               kPluginName, imm[-6], imm[-5], imm[-4], imm[-3], imm[-2], imm[-1]);
      return false;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
      _MESSAGE("%s: kill-time mark-plain patch failed (VirtualProtect)",
               kPluginName);
      return false;
    }
    *imm = 0x00;
    DWORD ignored = 0;
    VirtualProtect(imm, 1, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), imm, 1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    _MESSAGE("%s: kill-time mark-plain patch raised - not patched", kPluginName);
    return false;
  }
  g_killPlainMarkPatched.store(true, std::memory_order_release);
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  _MESSAGE("%s: MapForceEmpower: kill-time mark-plain store neutralised at %p "
           "(RVA %llX, mov byte [rdi+0xEA],1 -> ...,0); this is the store that "
           "made the enemy plain again after every kill, and with it the target "
           "both respawns and stays purple",
           kPluginName, reinterpret_cast<void*>(imm),
           static_cast<unsigned long long>(
               reinterpret_cast<std::uintptr_t>(imm) - base));
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

// ApplyIchiNanRenderPatch lived here. REMOVED 2026-09-22 - falsified, see the
// tombstone in patterns.h and analysis/purple_variant_re_2026-09-22.md section 37.1.

// ORs the 一難/purple bit into one entity, if that entity carries our target key.
//
// Layout, CE-verified 2026-09-20 on a live map: the dword at entity+0xE8 reads
// 0x101 for the engine's own powered-up placements and 0x1 for every ordinary
// enemy built from the SAME key, so byte +0xE9 is the engine's "powered-up
// CANDIDATE" mark. The engine's only recurring writer touches byte +0xE8 alone
// (see kMapPurpleFlagPattern), so once this bit is set nothing puts it back -
// which is why the mark survives map re-entry and needs no re-assert thread.
//
// CORRECTED 2026-09-22: this mark is NOT the appearance switch. entity+0xEA is
// (0 = purple, 1 = forced plain), and with MapForceEmpower on, the +0xE9 gate is
// NOPed anyway - so under the current patch set this write changes nothing. It
// only has an effect with MapForceEmpower=0, where it is still not sufficient on
// its own (the "already killed" lookup sends the spawn down the plain branch).
// Kept because MapPurple=1 is also the switch that installs the factory-entry
// hook, and because removing the per-frame marker is a separate change.
void MapPurpleMark(std::uintptr_t entity) {
  // Inert under MapForceEmpower, so bail out before touching any memory. That
  // switch NOPs the very +0xE9 gate this mark exists to satisfy (RVA 0x54FB6B),
  // and the sibling copy at RVA 0x54FC00 can now only write entity+0xEA = 0, so
  // the mark cannot change how anything looks. It matters because this is called
  // from a hook that runs for every active entity on every frame. Measured
  // 2026-09-22: with MapForceEmpower on, the target is purple whether or not this
  // byte is set. The mark still has a purpose with MapForceEmpower=0, which is
  // why it is gated rather than deleted.
  if (g_mapForceEmpower.load(std::memory_order_relaxed)) {
    return;
  }
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
  // Same inertness as MapPurpleMark, but tested first so the IsMapTargetKey scan
  // below is skipped too. See the note in MapPurpleMark.
  if (g_mapForceEmpower.load(std::memory_order_relaxed)) {
    return;
  }
  if (g_mapPurple.load(std::memory_order_relaxed) == 0 || entity == nullptr ||
      key == 0 || !IsMapTargetKey(key)) {
    return;
  }
  MapPurpleMark(reinterpret_cast<std::uintptr_t>(entity));
}

// Factory-entry body (see FnFactoryEntry). Runs at the creation/ensure moment
// for every entity, which the placement-record hook does not cover - the
// long-standing "coverage" problem. Passes through untouched; it only marks and,
// when FactoryDiag is on, records a field snapshot for the first kFepLogMax
// distinct target entities.
//
// Everything here is a plain read of the entity. No allocation, no locks.
extern "C" void MapPurpleFactoryEntryBody(void* handler, void* entity,
                                          std::uint32_t key,
                                          std::uint32_t category) {
  (void)handler;
  (void)category;
  // REMOVED 2026-09-22: an "unconditional probe" block used to sit here, ahead of
  // every filter. It counted and logged the first kFepRawLogMax calls ("fep raw
  // #...") to settle a one-off question - whether this hook is on the creation
  // path at all, or on it but rejecting every call on a key-convention mismatch.
  // That question was answered on 2026-09-21, and the block was NOT behind any
  // switch: with MapPurple=1 it ran for every entity. Removed rather than gated
  // because its question is closed.
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
  // Diagnostic field snapshot. Gated on FactoryDiag as of 2026-09-22: this hook
  // is installed for the MARKING, not for the sampling, so with MapPurple=1 it
  // used to emit up to kFepLogMax lines even with FactoryDiag=0. Same shape as
  // the sweep counters, same purpose - "was the plain one ever offered here?".
  if (g_factoryDiag.load(std::memory_order_relaxed)) {
    g_fepSeen.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t n = g_fepLogged.fetch_add(1, std::memory_order_relaxed);
    if (n < kFepLogMax) {
      _MESSAGE("%s: fep #%llu ent=%p cat=%X id=%X d38=%X d90=%X d94=%X dE8=%X",
               kPluginName, static_cast<unsigned long long>(n),
               reinterpret_cast<void*>(entity), category, id, d38, d90, d94,
               dE8);
    }
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

