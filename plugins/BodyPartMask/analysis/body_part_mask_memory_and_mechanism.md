# Body part hiding: verified facts, withdrawn claims and revalidation baseline

## Scope

This note records only what was reproduced live in the running game while
building `BodyPartMask`.  Every claim carries its evidence.  Claims that were
later falsified are kept in the "Withdrawn claims" section on purpose: they are
the traps a future re-derivation will fall into again.

It does **not** assign semantics to a value merely because it changes when
equipment changes, and it does not claim the row map below is complete - it is
explicitly partial and says so.

Baseline for this document: **game 2.0.2.0**, module base `0x7FF7C77E0000`
(session-specific), verified 2026-10-06.

## The phenomenon being explained

Certain equipment makes the character's body skin disappear underneath it: the
chest, the waist and the upper arms stop being drawn while that item is worn.
The question was whether the hiding is baked into the equipment's model, or
driven at runtime from data.

It is **runtime data**: a per-appearance-group 32-bit mask that the game reads
every frame.

## Verified mechanism

### The mask loop

`Nioh3.exe+0x3FEC48` (inside the per-entity appearance application function)
tests one bit per body part and calls the apply primitive:

```asm
Nioh3.exe+3FEC48  mov   rcx,r12
Nioh3.exe+3FEC4B  call  Nioh3.exe+3FECC8C     ; number of parts
Nioh3.exe+3FEC59  bt    esi,ebx               ; test bit i of the mask
Nioh3.exe+3FEC61  setae r8b                   ; mode = !bit
Nioh3.exe+3FEC65  call  Nioh3.exe+3FEC90      ; ApplyPart(obj, i+1, mode)
```

`esi` is the accumulated hide mask.  **Bit set means that part is NOT drawn;
bit clear means it is drawn.**

Signature (unique, 27 bytes):

```text
49 8B CC E8 ? ? ? ? 8D 7B 01 3B FD 73 18 0F A3 DE 8B D7 49 8B CC 41 0F 93 C0
```

### Where the mask comes from

The same function accumulates `esi` from the entity's appearance slots.  The
slot loop is `Nioh3.exe+3FEB2B` .. `Nioh3.exe+3FEBA5`:

```asm
Nioh3.exe+3FEB2B  mov  ecx,[r14]              ; slot type
Nioh3.exe+3FEB2E  sub  ecx,4   ; je -> case type 4
Nioh3.exe+3FEB33  sub  ecx,1   ; je -> case type 5
Nioh3.exe+3FEB38  sub  ecx,1   ; je -> case type 6
Nioh3.exe+3FEB3D  sub  ecx,1   ; je -> case type 7
Nioh3.exe+3FEB42  cmp  ecx,1
Nioh3.exe+3FEB45  jne  Nioh3.exe+3FEB67       ; any other type: normal path
Nioh3.exe+3FEB47  test byte [rdi+08],10       ; type 8
Nioh3.exe+3FEB4B  je   Nioh3.exe+3FEB67
Nioh3.exe+3FEB4D  jmp  Nioh3.exe+3FEBA2       ; SKIP this slot

Nioh3.exe+3FEB4F  test byte [rdi+08],08       ; type 7 -> bit 3
Nioh3.exe+3FEB55  test byte [rdi+08],04       ; type 6 -> bit 2
Nioh3.exe+3FEB5B  test byte [rdi+08],02       ; type 5 -> bit 1
Nioh3.exe+3FEB61  test byte [rdi+08],01       ; type 4 -> bit 0
Nioh3.exe+3FEB65  jne  Nioh3.exe+3FEBA2       ; flag set -> SKIP

Nioh3.exe+3FEB67  mov  rdx,[r14+10]           ; slot object
Nioh3.exe+3FEB6B  test rdx,rdx ; je skip
Nioh3.exe+3FEB70  mov  rcx,[rdx+20]
Nioh3.exe+3FEB74  test rcx,rcx ; je skip
Nioh3.exe+3FEB79  call Nioh3.exe+162934       ; gate
Nioh3.exe+3FEB7E  test al,al   ; je skip
Nioh3.exe+3FEB82  cmp  [r14+0x308],bl ; jne skip
Nioh3.exe+3FEB8B  mov  edx,[rdx+0x1F8]        ; KEY for this slot
Nioh3.exe+3FEB91  mov  rcx,[r15+60]           ; container object
Nioh3.exe+3FEB95  call Nioh3.exe+3FEDAC       ; -> record
Nioh3.exe+3FEB9F  or   esi,[rax+08]           ; accumulate the mask
```

**The skip rule, exactly:** for slot types 4..8 the slot is skipped when the
matching bit of the wrapper's flag byte `[wrapper+8]` is set
(type 4 -> bit 0, 5 -> bit 1, 6 -> bit 2, 7 -> bit 3, 8 -> bit 4).  Every other
type always takes the normal path.  A slot is also skipped when its object is
null, when `[object+0x20]` is null, when the `+162934` gate returns false, or
when `[slot+0x308] != 0`.

Accumulation signature (unique, 14 bytes, this is the first of three identical
`or esi,[rax+08]` sites and the only one reached with a live slot):

```text
0B 70 08 49 81 C6 40 03 00 00 4C 3B F5 0F 85
```

### The record lookup

`Nioh3.exe+0x3FEDAC` maps a key to a container record.  Its tail is distinctive
enough to be the signature; the entry point is the match minus `0x1C`:

```asm
Nioh3.exe+3FEDAC  push rbx ; sub rsp,20
Nioh3.exe+3FEDB2  mov  rbx,rcx
Nioh3.exe+3FEDB5  mov  rcx,[rcx+20]
Nioh3.exe+3FEDB9  call Nioh3.exe+455DBC      ; key -> index
Nioh3.exe+3FEDBE  mov  rdx,[rbx]              ; array base
Nioh3.exe+3FEDC1  cmp  eax,[rdx+04]           ; bounds check
Nioh3.exe+3FEDC4  jae  Nioh3.exe+3FEDDA
Nioh3.exe+3FEDC6  mov  eax,eax
Nioh3.exe+3FEDC8  lea  rax,[rax+rax*4]        ; index*5
Nioh3.exe+3FEDCC  lea  rax,[rax+02]
Nioh3.exe+3FEDD0  lea  rax,[rdx+rax*4]        ; arrayBase + index*20 + 8
Nioh3.exe+3FEDD4  add  rsp,20
Nioh3.exe+3FEDD8  pop  rbx
Nioh3.exe+3FEDD9  ret
```

Signature (matches 10 times - the compiler emitted equivalent clones, all
usable because the function is a pure `(container, key) -> record` lookup):

```text
48 8D 04 80 48 8D 40 02 48 8D 04 82 48 83 C4 20 5B C3
```

### Container layout

```text
container      = [[Nioh3.exe+0x45B9E30] + 0x60]
arrayBase      = [container]
recordCount    = *(uint32_t*)(arrayBase + 4)      ; 1634 in the verification session
record(i)      = arrayBase + i*20
record.mask    = *(uint32_t*)(record(i) + 0x10)
```

`or esi,[rax+08]` reads `arrayBase + i*20 + 0x10` because the lookup returns
`arrayBase + i*20 + 8`.

### Bit index equals part-table row

**Verified rule: mask bit `i` is row `i` of the body part table `0x5291EA5D`
(29 rows, indices 0..28).**

Evidence: with a non-pausing logging breakpoint on the clear branch
`Nioh3.exe+0x20273C` (`RDX` = `1<<bit`, `RCX` = bit, `R14` = the part hash), a
mask of `0x03324000` produced these hits, a strict 1:1 correspondence:

| bit (`RCX`) | `RDX` | `R14` (part hash) | GRP row |
| ---: | ---: | ---: | ---: |
| 5 | `0x00000020` | `0x4A4A65AD` | 5 (chest) |
| 7 | `0x00000080` | `0x0D6E216B` | 7 |
| 8 | `0x00000100` | `0xEEFFFF4A` | 8 (waist) |
| 9 | `0x00000200` | `0xD091DD29` | 9 |
| 13 | `0x00002000` | `0x9FD86AA1` | 13 |
| 15 | `0x00008000` | `0x368E0092` | 15 |
| 18 | `0x00040000` | `0x81C0BA4C` | 18 |
| 21 | `0x00200000` | `0xD4490D68` | 21 |
| 22 | `0x00400000` | `0xB5DAEB47` | 22 |
| 25 | `0x02000000` | `0x6B5B346E` | 25 |

### Body part table `0x5291EA5D` - row to hash (rows 0..28)

```text
row  0  5A6F3776     row 10  495B5DB8     row 20  2314764E
row  1  3C011555     row 11  84B3ED1B     row 21  D4490D68
row  2  F1815E7C     row 12  4945CF3A     row 22  B5DAEB47
row  3  D3133C5B     row 13  9FD86AA1     row 23  976CC926
row  4  68B887CE     row 14  646A4CC0     row 24  78FEA705
row  5  4A4A65AD     row 15  368E0092     row 25  6B5B346E
row  6  2BDC438C     row 16  8737DE93     row 26  4CED124D
row  7  0D6E216B     row 17  D7E1BC94     row 27  2E7EF02C
row  8  EEFFFF4A     row 18  81C0BA4C     row 28  1010CE0B
row  9  D091DD29     row 19  D26A984D
```

**Row 10 (`0x495B5DB8`) has no part record at all** in the model's part list
(28 records exist for rows 0-9 and 11-28).  It is therefore never drawn,
regardless of the mask bit.  This is the real reason a "hide by pointing the
show-anchor at row 10" trick works, and it is not a mask effect.

### Row map - what is actually verified

| Row(s) | Part | Evidence |
| --- | --- | --- |
| 2, 3 | right hand / right forearm / right elbow | in-game: hiding 2,3 removed them; nothing else changed |
| 5 | chest | in-game, repeated; clear-branch hit `R14=0x4A4A65AD` |
| 8 | waist | clear-branch hit `R14=0xEEFFFF4A` |
| 12 | left upper arm | in-game |
| 23, 27 | knee | in-game probe |
| 4, 6, 13, 14 | no visible change when hidden alone | in-game probe; likely covered by armour or not drawn |
| 10 | no part record | part-table dump |

Geometry measured from the active body model
(`mods\Nioh3_Female_Nude_Body_Vanilla_Shape_Loose_File_Loader\0x4BA258DF.g1m`,
29 sub-meshes, bounding boxes in model space, `+X`/`-X` are the two sides):

```text
row  0  X  49.8 ..  67.4   Y  95.5 .. 111.4   forearm
row  1  X  36.7 ..  53.0   Y 108.2 .. 123.7   elbow
row  2  X -67.4 .. -49.8   Y  95.5 .. 111.4   forearm (mirror)
row  3  X -53.0 .. -36.7   Y 108.2 .. 123.7   elbow (mirror)
row  4                                        head/neck
row  5                                        chest
row  6                                        inner torso
row  7                                        zero vertices
row  8                                        waist
row  9                                        zero vertices
row 10                                        no part record
row 11  X  11.0 ..  26.0   Y 133.7 .. 151.5   clavicle
row 12  X  20.0 ..  42.4   Y 118.0 .. 140.3   upper arm
row 13  X -26.0 .. -11.0   Y 133.7 .. 151.5   clavicle (mirror)
row 14  X -42.4 .. -20.0   Y 118.0 .. 140.3   upper arm (mirror)
row 15/18, 16/19, 17/20, 21/25, 22/26, 23/27, 24/28   legs and hips
```

The geometry is a hint, not proof: rows 12 and 14 are the two upper arms by
symmetry, yet hiding **14 alone produced no visible change** while 12 worked.
Treat the geometry column as a starting point for probing, never as the answer.

## Player-side structures (for the F9 capture)

```text
playerTableGlobal = Nioh3.exe+0x4745348
playerTable       = [playerTableGlobal]
playerObject      = [playerTable + 0x1338]
wrapper           = playerObject + 0x9F0
wrapperFlags      = *(uint8_t*)(wrapper + 8)          ; the skip-test byte

slotBase          = GetBase(wrapper) + 0x57C0          ; 7 records, stride 0x340
slotRecord(i)     = slotBase + i*0x340
slotRecord.type   = *(uint32_t*)(slotRecord + 0x00)
slotRecord.object = *(void**)(slotRecord + 0x10)
slotKey(i)        = *(uint32_t*)(slotRecord.object + 0x1F8)
record(i)         = Lookup(container, slotKey(i))
index(i)          = ((record - 8) - arrayBase) / 20
```

`GetBase` at `Nioh3.exe+0xF5358` is a **pure getter** - it only reads memory, so
it is safe to call from a non-game thread:

```asm
Nioh3.exe+F5358  mov  rax,[rcx+0x9F60]
Nioh3.exe+F535F  test rax,rax
Nioh3.exe+F5362  jne  Nioh3.exe+F5369
Nioh3.exe+F5364  lea  rax,[rcx+0x20]
Nioh3.exe+F5368  ret
Nioh3.exe+F5369  cmp  dword ptr [rcx+0x9F6C],0
Nioh3.exe+F5370  jne  Nioh3.exe+F5368
Nioh3.exe+F5372  jmp  Nioh3.exe+F5364
```

`Lookup` at `Nioh3.exe+0x3FEDAC` is a read-only hash lookup.  It is called from
a worker thread only on demand (F9), wrapped in `__try/__except`.

## Verified live writes

Writing one bit into a record's mask changes rendering **immediately**, with no
restart and no other side effect.

| Date | Index | Written | Observed |
| --- | ---: | ---: | --- |
| 2026-10-06 | `0x634` | `0x00024010` | clear-branch hit for `R14=0x68B887CE` (row 4), `RDX=0x10`, `RCX=4` |
| 2026-10-06 | `0x634` | `0x00024020` | clear-branch hit for `R14=0x4A4A65AD` (row 5, chest), `RDX=0x20`, `RCX=5`; **user confirmed the chest was hidden** |
| 2026-10-06 | `0x634` | `0x00024000` | restored |
| 2026-10-06 | `0x3C9` | `0x000053A0` | chest + waist + both upper arms hidden (user confirmed) |

Reproducible Cheat Engine recipe:

```text
1. arrayBase = [[[Nioh3.exe+0x45B9E30]+0x60]]
2. record    = arrayBase + index*20
3. mask      = *(uint32_t*)(record + 0x10)
4. hide row r :  mask |=  (1 << r)
   show row r :  mask &= ~(1 << r)
5. The game reads the record every frame, so the change is visible at once.
```

Masks for common targets:

```text
chest (row 5)            bit 5   = 0x00000020
waist (row 8)            bit 8   = 0x00000100
left upper arm (row 12)  bit 12  = 0x00001000
right upper arm (row 14) bit 14  = 0x00004000
everything               bits 0..28 = 0x1FFFFFFF
```

## Why the index is not the key

The value read from `[slotObject+0x1F8]` is a **key**, not an array index.
`Lookup` converts it.  Both are small integers and it is easy to confuse them -
this mistake cost a full debug cycle:

| Slot | key (`[object+0x1F8]`) | real record index |
| --- | ---: | ---: |
| 1 | `0xC22` | `0x3C9` |
| 4 | `0x116` | `0x293` |
| 6 | `0x634` | `0x220` |
| 6 | `0xA4F` | `0x1` |

The mapping is stable while the game data is unchanged, but it changes when the
equipment changes: transmog moved slot 1 from `0x3C9` to `0x3D3` to `0x3DD`
across three sessions, and slot 4 from `0x3CB` to `0x34F`.

**Record contents do not change when equipment changes.**  A "diff the
container before/after equipping" discovery method therefore always reports
zero changes.  What changes is *which index the slot points at*.

## Withdrawn claims

Each of these was asserted during the investigation and then falsified.  They
are recorded because each one is a reproducible trap.

1. **"`RCX` at the accumulation site is the container index."**
   False.  At `Nioh3.exe+0x3FEB9F` `RCX` is a leftover from the `Lookup` call
   (the callee clobbers it).  The reliable value is `RAX`, the record pointer;
   the index is `((RAX-8) - arrayBase)/20`.  Writing to
   `arrayBase + RCX*20 + 0x10` writes to an unrelated record and has no effect.

2. **"The container records change when equipment changes, so F9 can diff
   them."**  False.  See above.  The F9 implementation now walks the player's
   slot structures instead.

3. **"Slot types 4..8 are always skipped, so their records are unusable."**
   False.  The skip depends on the wrapper's flag byte `[wrapper+8]`; with
   `flags = 0x01` only type 4 is skipped, and types 5 and 7 were proven usable
   in-game.  The plugin now mirrors the flag test exactly.

4. **"`index 0x1` is the always-on base body slot, so targeting it hides parts
   unconditionally."**  Not proven and contradicted in practice: its slot is
   type 17 (never skipped by the type switch) and the record was written
   successfully, yet nothing changed on screen.  The cause is not identified.
   Do not rely on index `0x1`.

5. **"`GetModuleHandleW(nullptr)` gives the plugin's own module."**
   False - it returns the host EXE, so the plugin read `Nioh3.ini`, silently
   fell back to every default, and loaded zero rules.  The plugin now uses the
   handle passed to `DllMain`, preferring `param->plugins_dir`.

6. **"`mask = (mask | hide) & ~show` is idempotent and sufficient."**
   False for reconfiguration: once a bit is in the record, `| hide` never
   removes it, so deleting a row from `HideRows` had no effect.  The plugin now
   tracks the bits it added and cleared per index and undoes its own previous
   edit before applying the new configuration.

7. **"A `safetyhook::create_mid` hook at the accumulation site is the right
   implementation."**  Rejected after two hangs: the site runs tens of
   thousands of times per frame, and even with a throttled hotkey poll the
   game never presented a frame.  The shipped design patches no code at all.

## Address resolution - signature first, RVA fallback

Every address is resolved by signature at load and validated to lie inside the
main module; a false match is discarded and the last verified RVA is used
instead.  The log prints both the address and the RVA so a mismatch is visible
immediately.

| Purpose | Signature | Tail to entry | Fallback RVA (2.0.2.0) |
| --- | --- | ---: | ---: |
| Player table global | `48 8B 15 ? ? ? ? 33 C0 48 85 D2 74 ? 83 F9 03 77 ? 48 63 C1` + RIP-relative at `+3`, instruction length 7 | - | `0x4745348` |
| Slot-array getter `GetBase` | `48 8B 81 60 9F 00 00 48 85 C0 75 05 48 8D 41 20 C3` | - | `0xF5358` |
| Record lookup | `48 8D 04 80 48 8D 40 02 48 8D 04 82 48 83 C4 20 5B C3` | `-0x1C` | `0x3FEDAC` |
| Container global | `4C 8B 3D ? ? ? ? 41 8B 0E 83 E9 04 74` + RIP-relative at `+3`, instruction length 7 | - | `0x45B9E30` |

The player-table signature is the one the shipped `AppearanceRefreshHotkey`
plugin already uses, so it stays valid as long as that plugin works.

Structural offsets are **not** signatures - they are data layout and stay
hardcoded: `+0x1338`, `+0x9F0`, `+8` (flags), `+0x57C0`, `0x340`, `+0x00`
(type), `+0x10` (object), `+0x1F8` (key), `+0x60`, record stride `20`, mask
offset `+0x10`.

## Cheat Engine revalidation procedure after a game update

Work through these in order.  Do not trust a matching RVA alone after a patch.

1. Attach CE to `Nioh3.exe`.  Record the executable version and the module
   base, and read the plugin log line
   `playerTable=... (rva ...) getBase=... (rva ...) lookup=... (rva ...) containerGlobal=... (rva ...)`.
   If a line says `from fallback RVA`, that signature no longer matches.

2. **Player table.**  Scan `+X` for the player-table signature and confirm the
   RIP-relative target is inside the module.  Dereference it, add `0x1338`,
   and check the result is a valid heap pointer whose first field, at `+0x9F0`,
   points back at it (`[[playerTable+0x1338]+0x9F0][0] == [playerTable+0x1338]`).

3. **`GetBase`.**  Scan for its signature; confirm the body still reads
   `[rcx+0x9F60]`, falls back to `lea rax,[rcx+0x20]`, and consults
   `[rcx+0x9F6C]`.  If it changed, the slot base offset `+0x57C0` must be
   re-derived from the caller, not guessed.

4. **Record lookup.**  Scan for the tail signature and step back `0x1C`.
   Disassemble: it must still be
   `[rcx+0x20] -> call -> [rbx] -> bounds check -> (index*5+2)*4`.
   If the `lea rax,[rax+rax*4]` chain changed, the record stride is no longer
   `20` and every mask offset in this document is wrong.

5. **Container global.**  Scan for its signature and confirm the RIP-relative
   target.  Then walk `[[global]+0x60]`, read `[arrayBase+4]` and check the
   count is plausible (about 1600).  Read a few records and confirm the mask
   field at `+0x10` holds small bit patterns.

6. **Accumulation site.**  Scan for `0B 70 08 49 81 C6 40 03 00 00 4C 3B F5 0F 85`.
   It must be unique.  Disassemble backwards to `mov ecx,[r14]` and forwards to
   `or esi,[rax+08]`; confirm the surrounding shape is unchanged.

7. **Prove the write path end to end.**  Start the debugger (VEH interface is
   enough), set a **non-pausing logging** breakpoint at the clear branch
   `Nioh3.exe+0x20273C` with register capture, then write a single bit into a
   record you know the player uses:

   ```text
   record = arrayBase + index*20
   *(uint32_t*)(record + 0x10) |= (1 << row)
   ```

   Hits must appear with `RCX == row`, `RDX == 1<<row` and `R14` equal to that
   row's hash from the table above.  If the row's hash never appears, the row
   has no part record or the index is not the player's.  Remove the breakpoint
   when done - the branch fires thousands of times per second.

8. **Re-derive the row map** with the same breakpoint: hide all rows
   (`0x1FFFFFFF`), then remove one row at a time and note which part
   reappears.  Do not trust the geometry table for rows that give no visible
   change.

9. **Re-verify the slot walk.**  Press F9.  The log must show
   `slot <i> type <t> key 0x<k> -> index 0x<n>` lines and a
   `paste into Indices:` line.  Cross-check one index against the CE recipe:
   the record address the plugin computes must equal
   `arrayBase + index*20 + 0x10`.

10. **Only then** re-verify the plugin end to end: set `Indices` to the captured
    index and `HideRows` to a row with a known visual effect, press F9, and
    confirm the part disappears.

## Failure signatures and what they mean

| Log line | Meaning |
| --- | --- |
| `no Indices; all rules will come from mod files` | expected when the plugin ini carries no rules |
| `config loaded - 0 rule(s)` | no rule anywhere; nothing will be applied |
| `-> N index(es) but nothing to hide or show; skipped` | a mod file named indices but has no rows and nothing to inherit |
| `addresses unresolved` | one of the four signatures failed and the fallback is zero |
| `player table not ready` | the chain resolved but the game has not built the player object yet |
| `slot capture faulted` | a pointer in the chain was invalid; the `__try` caught it |
| `slot capture - slot base not ready` | `GetBase` returned null |
| `no usable slot right now` | every slot was skipped by the type/flag test |

## Open questions

These are **not** answered and should not be assumed:

- The complete 29-row to body-part mapping.  Rows 2, 3, 5, 8, 12 and 23, 27 are
  verified; the rest are geometry hints or unknowns.
- Why a record that a non-skipped slot points at (`index 0x1`, slot type 17)
  produces no visible change when written.
- The semantics of the container's ~1634 records.  They group into runs whose
  masks tile the 28 bits (for example `0x28:0x0000000F`,
  `0x2A:0x00003FF0`, `0x2B:0x000FC000`, `0x2C:0x0FF00000`, OR-ing to
  `0x0FFFFFFF`), which looks like a per-appearance-group part selection, but
  that is an observation, not a proven model.
- Whether the same container is shared between the player and NPCs.  The mask
  loop runs for every entity, so a record written for the player is also read
  by any entity whose slot resolves to the same index.

## Configuration surface

One section, `[BodyPartMask]`, shared by the plugin's own ini and by every mod
rule file.  `analysis/Your-Mod-BodyPartMask.ini` in this directory is the
template handed to mod authors.

| Key | Meaning |
| --- | --- |
| `Enabled` | master switch (plugin ini only) |
| `Indices` | container record indices, **hex**, comma separated; several allowed |
| `HideRows` | rows to force hidden, **decimal**, comma separated |
| `ShowRows` | rows to force shown, applied after `HideRows` |
| `LogIndices` | 1 = dump every non-zero mask record at startup |
| `ToggleKey` | F8 by default |
| `CaptureKey` | F9 by default |

Rules are read from:

```text
<plugins>\BodyPartMask.ini
<game>\mods\*-BodyPartMask.ini
<game>\mods\<one subdirectory>\*-BodyPartMask.ini
```

The mod scan mirrors LooseFileLoader: mods root first, then subdirectories
sorted by name.  A mod file needs only `Indices`; when it leaves
`HideRows`/`ShowRows` empty it inherits the plugin ini's values.  Rules naming
the same index are merged by OR-ing their hide bits.

`HideRows` accepts `0..31` but only `0..28` correspond to a part row; listing
`29`/`30` is harmless and simply sets bits nothing reads.

Both `3C9` and `0x3C9` parse identically - the parser uses base 16, which
accepts the optional prefix.

## Plugin implementation map

| Document fact | Where it lives in `main.cpp` |
| --- | --- |
| Four signatures + module-range validation | `ResolveAddresses`, `InMainModule` |
| Container walk | `ResolveContainer`, `RecordMask` |
| Per-index add/clear tracking | `TrackedIndex`, `FindTracked`, `ApplyOne` |
| Rule merge from all config files | `AddRule`, `LoadRuleFile`, `LoadConfig` |
| Mod rule discovery (`mods` + one subdirectory) | `CollectModConfigs` |
| F9 capture and the exact skip test | `CaptureSlots` |
| F9 = reload + apply + capture | `WorkerThread` |
| Own ini path | `ResolveOwnIni` |
