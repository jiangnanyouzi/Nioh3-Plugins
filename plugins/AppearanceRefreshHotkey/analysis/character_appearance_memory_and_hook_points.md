# Character appearance: verified facts and withdrawn claims

## Scope

This note records only observations reproduced in the current reverse-
engineering work.  It intentionally does **not** assign game semantics to
values merely because they change when an appearance is changed.

## Verified object relationship

The current player was identified in-session by the matching maximum-HP value.
That discriminator is temporary and must not be treated as a stable identity.

```text
Player Object
  +0x3A0 -> CharacterParent

CharacterParent
  +0x50  max HP
  +0x58  current HP
  +0x90  -> subobject A
  +0x408 -> subobject B
```

The HP offsets were cross-checked while equipping items that changed maximum
HP.  Heap addresses are session-specific.

## Verified update path

```text
Nioh3.exe+1F135C
  RCX = UpdateContext
  RDX = RCX
  RCX = [RCX+38]
  call Nioh3.exe+1F1374

Nioh3.exe+1F1374
  dispatches an object array according to a runtime mode

Nioh3.exe+1F14A3
  calls Nioh3.exe+1F1534 for each selected object

Nioh3.exe+1F1534
  reads [Object+3B0] and enters later update work
```

`+1F135C` has no static direct callers in the current binary.  It is therefore
consistent with a scheduler/indirect-callback entry, not evidence of a UI or
appearance Setter.

Calling the thunk after its natural return faulted at:

```asm
Nioh3.exe+1F1383: mov r8,[rcx]
```

The fault occurred because the later call reached `+1F1374` with an invalid
dispatcher pointer.  Calling `+1F1374(dispatcher, context)` only while those
arguments are live returned normally.  This proves a calling-lifetime fact;
it does not prove that the dispatcher applies appearance data.

## Verified writes; transition-layer semantics only

During appearance changes, these writes were observed for the current
`CharacterParent`:

```asm
Nioh3.exe+5B5929: movss [rcx+28],xmm0   ; rcx = CharacterParent+408
Nioh3.exe+5B596B: movss [rbx+40],xmm6   ; rbx = CharacterParent+408
Nioh3.exe+5B6F3C: movss [rdi+A4],xmm12  ; rdi = CharacterParent+90
```

They address, respectively:

```text
CharacterParent+430
CharacterParent+448
CharacterParent+134
```

The values are 32-bit floating-point writes.  The following claims are
withdrawn and must not be used as implementation assumptions:

- that any of these locations is a raw appearance ID;
- that the values are equipment-slot values;
- that the update functions are appearance Setters;
- that a short observed `+1` sequence defines their normal semantics.

The first two writes are now narrowed further.  `Nioh3.exe+0x5B590C` is the
entry of the function containing both `+0x5B5929` and `+0x5B596B`; it adds
the incoming single-precision value to fields at `+0x28` and `+0x40`, then
compares/clamps and advances related state.  The player update path passes
`CharacterParent+0x408` as its `this` value.  It is therefore an animation or
transition-state update routine, not evidence of a raw appearance assignment
or of an appearance setter.  `+0x5B6F3C` remains only a transition write.

These sites are excluded as hotkey targets.  They are still useful as proof
that a visual change reaches the player's update layer, but not as a source
object or causal input path.

## Cached-change control result

With the non-breaking breakpoint on `Nioh3.exe+0x42A517` armed, a subsequent
appearance change produced zero hits.  That instruction belongs to the
resource fan-out routine beginning at `+0x42A358`; it processes a resource
descriptor (`RBX`, whose `+0x08` is tested against `0xAD57EBBA`) and queues
resource work.  Earlier cold-load captures did hit it, but this control shows
that it is bypassed when the required asset is already cached.

Consequently this path is a cold-load observation only.  It cannot identify
the selection transaction, its slot source, or a safe hotkey call.  The
breakpoint was removed rather than leaving a misleading inactive probe armed.

## Equipment-record synchronization (2026-07-14)

The object previously labelled `CharacterParent` matches the already-known
`PlayerData` layout: its `+0x50/+0x58` values are the maximum/current-health
members reached through `PlayerData+0x38`.  Its two equipment sets are laid
out as follows:

```text
PlayerData +0x570   set 0, 17 records, 0xE8 bytes each
PlayerData +0x14E0  set 1, 17 records, 0xE8 bytes each
PlayerData +0x2450  active set index
```

During a controlled chest-appearance selection, a hardware write watch on
the current player's records observed this exact copy routine:

```asm
Nioh3.exe+6B9C38  ; validates set (0..1) and slot (0..16)
                   ; destination = RCX + set*0xF70 + slot*0xE8
                   ; source = R8
Nioh3.exe+6B9C92  mov [rcx+02], ax
```

At entry the actual parameters were:

```text
RCX = current PlayerData+0x570
RDX = slot index
R8  = source 0xE8-byte equipment record
R9  = set index
```

The controlled action copied all relevant records, including the chest,
arms, and knee records.  The source and destination words at `record+0x02`
were equal to their respective `record+0x00` item IDs both before and after
the action.  Therefore `record+0x02` is a real synchronized field but is
**not proven to be the source of the preview appearance selected in this
test**.  It must not yet be used as an appearance-copy field.

`+0x6B9C38` is only a generic 0xE8-byte record-copy helper with many static
callers.  The raw Cheat Engine stack captured at that helper contained local
and spill values rather than a reliable call chain, so it is also not a
hotkey target.  The next in-process probe logs the same verified parameters
with a Win64-unwound stack, only while F6 capture is armed.

## Current safe instrumentation boundary

The plugin can identify the live player object during the update path and can
capture the live `(dispatcher, UpdateContext)` pair.  This is suitable for
observing argument lifetime and for a later refresh after a proven write.  It
is not yet sufficient to copy or set an appearance.

## Appearance-state word write (2026-07-14)

This result is based on three controlled chest-appearance changes and on
byte-for-byte comparisons of the live player object.  It supersedes the
earlier assumption that the `0xE8` equipment records might contain the
preview-selection field.

The complete `PlayerData` range observed by its own update code (`0x3600`
bytes) was snapshotted before and after a chest change.  The equipment records
for both sets and slots 4--8 were unchanged.  The only stable changed word was
inside this player-state block:

```text
PlayerData +0x2450  : 0x4E-byte synchronized state block
  +0x40             : two-word pair
  +0x42             : changed on every controlled chest appearance selection
```

The apparent 32-bit value at `+0x40` changes only in its upper word.  A
hardware write watch proved that the write is a copy from a separate source
block, not a direct assignment to `PlayerData`:

```text
source block                     -> PlayerData +0x2450
0x138501F0CC8 (this session)     -> 0x1384E219BC0
```

The `0x4E` bytes compared equal after the synchronization.  The synchronizer
is `Nioh3.exe+0x6B9A9C`; its internal fixed-size copy helper is
`Nioh3.exe+0x6B9F70`.  This is still a consumer path, not the setter.

The source-field watch reached the actual writer:

```asm
Nioh3.exe+0x22390B8
  cmp edx,27
  ja  return
  movsxd rax,edx
  mov [rcx+rax*2],r8w
  ret
```

Thus the verified calling convention is:

```cpp
void WriteIndexedStateWord(std::uint16_t* state,
                           std::uint32_t index,
                           std::uint16_t value);
```

For each controlled chest change, the arguments were:

```text
RCX = source state block (0x138501F0CC8 in this session)
EDX = 0x21
R8W = selected 16-bit value (for example 0xABA9, then 0xEC15)
```

`state[0x21]` is therefore a **verified chest-selection state word for this
test path**.  Its semantic item/model ID encoding and the indices for the
other appearance slots remain unverified.  The function has no direct static
CALL references, so its immediate caller is indirect/tail-dispatched; the
raw CE stack is insufficient evidence to name a caller.  The plugin now
filters this exact state pointer and index, then captures a Win64-unwound
stack from inside the setter on the next chest selection.  On a new game
session, the first recognized-player write at index `0x21` bootstraps the
source pointer when the synchronizer has not yet run; subsequent logging again
requires the exact captured state address.

## Verified UI bridge and F7 refresh test (2026-07-14)

The unwound stack from a chest write identifies the direct business call site
as `Nioh3.exe+0x246A996`, returning to `+0x246A99B`.  That site calls
`Nioh3.exe+0x14BCA6C` with four live arguments.  The bridge is fully
disassembled:

```cpp
void SetAppearanceStateWord(void* tableBase, std::uint32_t stateSelector,
                            std::uint32_t wordIndex, std::uint16_t value) {
  auto* state = static_cast<std::uint16_t*>(tableBase) + stateSelector * 0x28;
  state[wordIndex] = value; // tail-jumps to +0x22390B8
}
```

Its current controlled chest-selection invocation was:

```text
RCX = 0x000001E2D3BF0CC8  state table/source block
RDX = 0x00000000          stateSelector
R8D = 0x00000021          wordIndex
R9W = 0x166D              selected value
```

The bridge is a direct tail-call wrapper, not a scheduler or resource-thread
function.  `AppearanceCopyHotkey` now provides a deliberately narrow refresh
test: after the plugin has observed a distinct prior chest value, **F7** runs
from the live player's update callback and invokes this bridge with the saved
previous value.  It then lets the normal update continue.  This test is
reversible through the UI and must be evaluated by its observed visual result;
it is not a general appearance-copy feature yet.  F8 remains unrelated and
must not be used.

The initial F7 test proved the bridge write happened but did **not** change the
visual result, ruling out a write-only hotkey.  The controlled UI path has one
immediate post-write call: `Nioh3.exe+0x2477990()`.  It takes no arguments,
locates the current `PlayerData`, builds a fresh local context, and calls
`+0x5B46C8(PlayerData, context)`.  The revised F7 test uses exactly the
verified `bridge -> +0x2477990` sequence.  It intentionally does not call the
later `+0x247782C`, whose `RCX` is an unverified UI object.

The revised F7 test was run successfully: after one controlled chest change,
F7 restored the immediately previous chest appearance.  This proves the
following chest-only refresh path in the tested game build:

```text
captured source state[0x21] = prior 16-bit chest value
  -> Nioh3.exe+0x14BCA6C(state, 0, 0x21, priorValue)
  -> Nioh3.exe+0x2477990()
  -> natural PlayerData update/render refresh
  -> observed previous chest appearance
```

This is now a proven hotkey path for restoring the previous **chest**
appearance value captured in the same session.  It does not yet prove source
format compatibility for copying from NPCs, nor the state indices for the
other armor slots.

## Five-slot transaction selector (2026-07-14)

The original conclusion that `state[0x21]` was shared by every appearance
slot is **superseded** by the 2026-07-15 original-writer captures below.  The
earlier arms observation reached the same source block, but did not identify
the final persistent word reliably.  The selector relationship in this
section remains valid; the persistent words are now known to be slot-specific
(`0x20..0x24`).

That object was obtained directly from the setter's preserved `RDI` register.
Three controlled transitions (chest -> arms -> chest) identified two mirrored
32-bit fields at `transaction+0x1404` and `transaction+0x1410`.  Their final,
stable values were then measured after a separate selection in every slot:

| Slot | Selector value |
| --- | ---: |
| Head | `0x0A` |
| Chest | `0x0B` |
| Arms | `0x0C` |
| Knee | `0x0D` |
| Legs | `0x0E` |

The `+0x1404` field's write watch showed a final arms write of `0x0C`; the
observed writer was a generic subobject setter (`RCX = transaction+0x1400`,
new value in `EDX`) and therefore does not by itself constitute a hotkey
target.  Intermediate selector values can occur while navigating the UI; only
the value after the controlled selection was used for the mapping above.

Consequently a complete future copy action needs both pieces of live context:
the active selector (`0x0A..0x0E`) and that selector's captured persistent
state word, followed by the verified bridge and refresh sequence.

The selector-field watch also resolved its own immediate writer at
`Nioh3.exe+0xFB33E0`:

```asm
mov eax,[rcx+14]   ; current selector becomes previous
mov [rcx+20],eax
mov [rcx+14],edx  ; incoming selector (0x0A..0x0E)
ret
```

At the watch site, `RCX = transaction+0x1400`; `transaction+0x1404` is this
subobject's current-selector member.  This is a small current/previous UI
state setter, not the complete transaction and not a hotkey target.

## Required evidence before implementing copy

1. Identify a source object whose bytes/fields change causally with one
   appearance selection, not merely with frame updates.
2. Capture the writer or transaction that assigns that source object, including
   its live arguments and caller stack.
3. Establish slot mapping and ownership across at least two independent
   appearance changes.
4. Only then determine whether copying data plus the proven refresh is valid.

## Five-slot F7 restore: verified state-word mapping (2026-07-15)

The earlier non-chest F7 implementation was withdrawn because it replayed a
UI transaction from a player-update callback and assumed that every slot used
the chest word (`state[0x21]`).  Controlled Cheat Engine captures disproved
that assumption for knee appearance selection:

```text
selector 0x0D (knee), UI model 0x41 -> state[0x23] = 0x6720
selector 0x0D (knee), UI model 0x40 -> state[0x23] = 0xF31D
```

Both captures used the same live state block (`0x1B1407F0CC8` in that
session).  The address is session-specific; the index mapping is the relevant
result.  A hardware execution breakpoint at the original writer body
`nioh3.exe+0x22390BD` recorded the real arguments immediately before
`mov [rcx+rax*2],r8w`:

```text
RCX = state block
RDX = word index
R8W = new persistent appearance value
```

The final plugin therefore treats the live UI model setter only as the start
of a short association window.  The next matching original state-word write
captures the actual `(state pointer, word index, old value, new value)` for
that selector.  It does not infer the word index from chest behavior.

### Verified selector map and visual tests

| Appearance slot | Selector | Captured word | F7 visual result |
| --- | ---: | ---: | --- |
| Head | `0x0A` | `state[0x20]` | restored prior head appearance |
| Chest | `0x0B` | `state[0x21]` | restored prior chest appearance |
| Arms | `0x0C` | `state[0x22]` | restored prior arms appearance |
| Knee | `0x0D` | `state[0x23]` | restored prior knee appearance |
| Legs | `0x0E` | `state[0x24]` | restored prior legs appearance |

The live log contains examples for every row.  In the verification session:

```text
head:  state[0x20] 0x8CC9 -> F7 -> 0x6251
chest: state[0x21] 0x594B -> F7 -> 0x5906
arms:  state[0x22] 0x1D34 -> F7 -> 0xFFFF
knee:  state[0x23] 0x150B -> F7 -> 0x1CF2
legs:  state[0x24] 0xC326 -> F7 -> 0xFFFF
```

All five F7 actions were observed in-game to restore the immediately prior
appearance for the active slot.

### F7 path now used

For the active slot, after at least one captured change has established a
distinct previous value, the plugin runs from the matching player's natural
update callback:

```text
SetAppearanceStateWord(capturedState, 0, capturedWordIndex, previousValue)
  -> WriteIndexedStateWord(capturedState, capturedWordIndex, previousValue)
RefreshPlayerAppearance()
```

Passing selector zero is deliberate: `capturedState` is the exact `RCX`
observed at the original writer, and the bridge adds `selector * 0x50` before
tail-calling the bounds-checked writer.  This avoids reconstructing a UI
transaction or retaining a short-lived UI object.  The model/transaction
replay path is no longer used by F7.

This feature restores the immediately previous selection captured in the
current game session.  It does not establish a compatible source format for
copying NPC appearance data; that remains separate work.

## F8: refresh all current transmog overrides without UI (2026-07-15)

`0xFFFF` is the observed **no appearance override** value.  It was first
verified from arms and legs with no transmog, then used as a controlled
resource-refresh boundary.

F8 resolves the live state table directly; it does **not** require an open UI,
a selected slot, or a transmog change in the current session:

```text
liveState = [[Nioh3.exe+0x44734B0] + 0x228F28]
```

This pointer chain was checked in CE from the UI caller at `+0x246A978`:

```text
[Nioh3.exe+0x44734B0] = globalHolder
[globalHolder]         = tableOwner
tableOwner + 0x228F28  = RCX at the live state-word writer
```

The contiguous five-slot armor mapping is established by the writer captures:

| Slot | selector | word in `liveState` |
| --- | ---: | ---: |
| Helmet | `0x0A` | `0x20` |
| Chest | `0x0B` | `0x21` |
| Arms | `0x0C` | `0x22` |
| Knees | `0x0D` | `0x23` |
| Legs | `0x0E` | `0x24` |

For every active armor word, F8 saves the exact value and performs two normal
state-bridge/refresh passes:

```text
all active current overrides
  -> SetAppearanceStateWord(liveState, 0, wordIndex, 0xFFFF)
  -> RefreshPlayerAppearance()
  -> wait 1000 ms / player-update callback
  -> SetAppearanceStateWord(liveState, 0, wordIndex, savedValue)
  -> RefreshPlayerAppearance()
```

The original 100ms interval logged both writes correctly but could finish
before asynchronous model rebuilding had a full scheduling window, causing
the user to need repeated F8 presses.  The one-second interval was deployed
and verified in-game: one F8 correctly refreshed the Mod while preserving all
selected transmog values.

This is intentionally different from F7: F7 restores a previously captured
selection, whereas F8 returns each slot to its **same current** transmog after
forcing a no-override refresh.  The old F8 update-dispatcher replay diagnostic
remains removed; F8 does not invoke that previously faulting path.

## Reproducible F8/F10 refresh baseline (game 1.0.7.0, 2026-07-16)

This section is the current implementation baseline for both the legacy F8
path and the standalone `AppearanceRefreshHotkey` plugin (default F10).  All
RVA values below are version-specific: revalidate them after every game patch
before writing memory or calling an internal function.

### Primitive functions and state records

| Purpose | RVA | Evidence / contract |
| --- | ---: | --- |
| Indexed state-word writer | `0x22390B8` | `RCX=record`, `RDX=word index`, `R8W=value`; validates the index then writes a word. |
| State bridge | `0x14BCA6C` | `SetAppearanceStateWord(table, selector, word, value)` selects `table + selector*0x50`, then tail-calls the indexed writer. |
| Player appearance refresh | `0x2477990` | Argument-free refresh routine used after the UI's persistent state write. |
| Player lookup used by refresh | `0x12ADCC` | With `ECX=0`, returns the current player object from the player-object table. |

The live appearance table is independently resolved on every hotkey press:

```text
liveState = [[Nioh3.exe+0x44734B0] + 0x228F28]
```

The state bridge uses a `0x50`-byte record stride.  A writer hit with
`RCX=liveState+0x50` is therefore **selector 1**, not a different unrelated
state table.

### Complete verified refresh map

`clear` is the value written for the one-second refresh boundary.  The saved
original word is then restored unchanged.

| Category | Item | bridge selector | word index | clear |
| --- | --- | ---: | ---: | ---: |
| Armor | Head | `0` | `0x20` | `0xFFFF` |
| Armor | Chest | `0` | `0x21` | `0xFFFF` |
| Armor | Arms | `0` | `0x22` | `0xFFFF` |
| Armor | Knees | `0` | `0x23` | `0xFFFF` |
| Armor | Legs | `0` | `0x24` | `0xFFFF` |
| Weapon | 刀 | `0` | `0x01` | `0x0000` |
| Weapon | 双刀 | `0` | `0x02` | `0x0000` |
| Weapon | 枪 | `0` | `0x03` | `0x0000` |
| Weapon | 斧头 | `0` | `0x04` | `0x0000` |
| Weapon | 大太刀 | `0` | `0x06` | `0x0000` |
| Weapon | 剃刀镰 | `0` | `0x09` | `0x0000` |
| Weapon | 手甲 | `0` | `0x0B` | `0x0000` |
| Weapon | 弓 | `0` | `0x18` | `0x0000` |
| Weapon | 火枪 | `0` | `0x19` | `0x0000` |
| Weapon | 火炮 | `0` | `0x1A` | `0x0000` |
| Ninja armor | 忍者头盔 | `1` | `0x20` | `0xFFFF` |
| Ninja armor | 忍者胸甲 | `1` | `0x21` | `0xFFFF` |
| Ninja armor | 忍者臂甲 | `1` | `0x22` | `0xFFFF` |
| Ninja armor | 忍者膝甲 | `1` | `0x23` | `0xFFFF` |
| Ninja armor | 忍者腿甲 | `1` | `0x24` | `0xFFFF` |
| Ninja weapon | 忍刀 | `1` | `0x0C` | `0x0000` |
| Ninja weapon | 忍双刀 | `1` | `0x0D` | `0x0000` |
| Ninja Weapon | 锁链 | `1` | `0x05` | `0x0000` |
| Ninja Weapon | 旋棍 | `1` | `0x07` | `0x0000` |
| Ninja Weapon | 机关棍 | `1` | `0x0A` | `0x0000` |
| Ninja Weapon | 手斧 | `1` | `0x08` | `0x0000` |
| Ninja weapon | 忍手甲钩 | `1` | `0x0E` | `0x0000` |
| Ninja weapon | 忍弓 | `1` | `0x18` | `0x0000` |
| Ninja weapon | 忍枪 | `1` | `0x19` | `0x0000` |
| Ninja weapon | 忍火炮 | `1` | `0x1A` | `0x0000` |


`0x0000` is the weapon equivalent of "original appearance / remove transmog".
It was directly captured at the original writer for a selector-0 melee weapon
(刀), a selector-0 ranged weapon (弓), selector-1 weapons (锁链、手斧), and
the selector-1 忍刀.  `0xFFFF` was directly verified as no armor override
from arms and legs, and again at selector-1 `state[0x20]` by 解除忍者头盔.

The ninja rows were captured with `RCX=liveState+0x50` and `RAX=1` at the
writer, proving that they use selector 1 rather than selector 0.  After adding
all eleven ninja rows, F10 was verified in-game to refresh an equipped ninja
armor or ninja weapon Mod correctly.

### Safe hotkey transaction

For every map row whose current word differs from its `clear` value:

```text
saved = *(uint16_t*)(liveState + selector*0x50 + wordIndex*2)
SetAppearanceStateWord(liveState, selector, wordIndex, clear)

after all active rows are cleared:
  RefreshPlayerAppearance()
  wait 1000 ms

for every active row:
  SetAppearanceStateWord(liveState, selector, wordIndex, saved)
RefreshPlayerAppearance()
```

The 1000ms gap is deliberate.  A prior 100ms implementation logged both
writes but intermittently failed to refresh loose-file mods until F8 was
pressed repeatedly.  The one-second boundary was verified to refresh on one
press while preserving the same selected transmog.

The refresh and internal writes must execute on the game's **player update
thread**, not an arbitrary update worker.  Calling the argument-free refresh
from an arbitrary update thunk caused a game crash; this is a confirmed unsafe
context.

The player update callback is identified without Max HP:

```text
playerTable     = [Nioh3.exe+0x4618398]
playerObject    = [playerTable+0x1338]
playerParent    = [playerObject+0x3A0]
updateParent    = [UpdateSingleObject.RCX+0x3A0]
run hotkey only if updateParent == playerParent
```

This is the same player lookup used at the start of `RefreshPlayerAppearance`:
`+0x247799B` clears `ECX`, calls `+0x12ADCC`, then reads `[RAX+0x3A0]`.
In the verification session the resulting parent was `0x131ED417750`; its
`+0x50` Max HP happened to be `3148`, but that number is not part of the
current identity test.  The pointer chain was verified in CE and the
Max-HP-independent F10 build was subsequently verified in-game.

### Cheat Engine revalidation procedure after an update

1. Attach CE to `Nioh3.exe`; record the executable version and module base.
2. Verify the four primitive functions above against their byte signatures in
   the source.  Do not trust a matching RVA alone after a patch.
3. Disassemble `RefreshPlayerAppearance`.  Confirm it still calls the player
   lookup with `ECX=0`, checks the return for null, then reads `[RAX+0x3A0]`.
4. Follow the player lookup's RIP-relative global and verify its selector-zero
   entry produces the same `CharacterParent` pointer as the player update
   object's `RCX+0x3A0`.
5. Set a **non-pausing logging** execution breakpoint at the indexed writer
   `Nioh3.exe+0x22390B8` (or its verified body).  Change exactly one transmog
   item in the UI.  Record `RCX`, `RDX`, `R8W`, and the bridge selector if
   available.  Remove the breakpoint immediately after each capture.
6. For every changed weapon family, perform the UI action "原本外观 / 解除幻化"
   and capture the clear value.  Do not assume armor's `0xFFFF` applies to a
   weapon; selector-0 and selector-1 weapon samples both proved `0x0000`.
7. Rebuild the map only from repeated writer captures.  Verify a new row by
   manually running clear -> refresh -> restore -> refresh on that row before
   adding it to the hotkey array.
8. Test with one armor and one weapon Mod from each affected selector.  Confirm the clear interval is
   visually temporary, the original transmog returns, and the loose-file Mod
   refreshes on one hotkey press.

### Standalone plugin configuration

`AppearanceRefreshHotkey.dll` is intentionally independent from the legacy
F7/F8 plugin and contains only the refresh path above.  On first load it
creates this file beside the DLL:

```ini
[AppearanceRefreshHotkey]
Hotkey=F10
```

`Hotkey` accepts `F1` through `F24`, decimal virtual-key values (for example
`121`), or hexadecimal virtual-key values (for example `0x79`).  The plugin
tracks the high "key currently down" bit and its own press edge rather than
using `GetAsyncKeyState`'s shared low transition bit.
