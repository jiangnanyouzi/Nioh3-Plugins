# BodyAppearanceRefreshHotkey

Refreshes the complete body-appearance transaction used by the **Change
Appearance** menu. It is intended for body, hair, face, and body-shape
loose-file mods; it does not refresh equipment transmog (use
`AppearanceRefreshHotkey` for that).

## Usage

1. Restart the game after copying or updating the DLL.
2. Open **Grooming -> Change Appearance**. You do not need to enter
   **Encountered Player Appearance**.
3. Press `F9` once. The game performs its own normal body-appearance rebuild.

On first load the plug-in creates:

```ini
[BodyAppearanceRefreshHotkey]
Hotkey=F9
```

in `plugins/BodyAppearanceRefreshHotkey.ini`. `Hotkey` accepts `F1`--`F24`, a
decimal virtual-key code, or a hexadecimal virtual-key code such as `0x78`.

## Safety boundary

The hotkey does not call an internal UI, event, resource, or body function. At
a verified point inside the game's existing UI update, after the game has
queried the current resource version, it makes the UI's cached version differ
for one comparison. The game's original conditional branch then constructs and
dispatches the ordinary rebuild transaction. The live UI pointer is used only
in that one hook invocation and is never retained.

It intentionally has no effect outside the required Change Appearance UI
state.
