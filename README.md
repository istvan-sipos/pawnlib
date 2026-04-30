# pawnlib

A pawn archive mod for *Dragon's Dogma: Dark Arisen*.

Every time you rest at an inn, pawnlib saves a copy of your main pawn to
disk. Later, you can hire any of those saved pawns from the rift, just
like a regular online pawn. If you change their gear or teach them new
things while they are in your party, the changes are written back to the
saved copy when you save the game.

A separate command-line tool, `restore_pawn.exe`, can promote any saved
pawn into your own main pawn slot.

## Features

- Auto-saves your main pawn on every inn rest.
- Lets you re-hire any saved pawn from the rift, sorted newest first.
- Updates a saved pawn's gear and knowledge when you save the game.
- Restore tool to swap any saved pawn into your main pawn slot.
- Works fully offline; pawnlib does not change your participation in
  the live rift — other players can still hire your pawn as normal.

## Requirements

- *Dragon's Dogma: Dark Arisen* on PC (Steam version). Linux + Proton
  works too.
- [ddda-dinput8](https://github.com/kubik-jaroslav/ddda-dinput8) — the
  mod loader. pawnlib needs it to start. If you already use any other
  DDDA mod that runs through ddda-dinput8, you have it installed.
- An active Steam login. pawnlib uses your real Steam session; you do
  not need any emulator or workaround.

## Installation

1. Install ddda-dinput8 first. Its `dinput8.dll` and `dinput8.ini`
   should sit next to `DDDA.exe` in your game folder.
2. Download the latest pawnlib release archive.
3. Extract its contents into the same folder where `dinput8.dll`
   lives. You should end up with these new files next to `DDDA.exe`:
   - `pawnlib.dll`
   - `pawnlib.ini`
   - `restore_pawn.exe`
   - `README.md`
4. Open `dinput8.ini` in a text editor. Under the `[main]` section,
   add or change this line:

   ```
   loadLibrary = pawnlib.dll
   ```

5. Launch the game. On first run a `pawnlib.log` file appears next to
   `DDDA.exe` so you can confirm the mod loaded.
6. Rest at an inn. A new folder named `pawnlib` will be created next
   to `DDDA.exe`; that is where your saved pawns live.

That's it. From then on, every inn rest adds another saved pawn, and the
rift's "Search by level" list shows your saved pawns instead of online
players.

## Configuration

The mod reads `pawnlib.ini` once when the game starts. The options:

- **max_search_results** — how many saved pawns the rift shows per
  level. Range 0 to 100. Default 100. Set to 0 to make the rift look
  empty (useful if you want to browse archives outside the game without
  the rift cluttering up).

- **logging** — controls the `pawnlib.log` file.
  - `disabled` — no log is written.
  - `truncate` — log is reset to empty each time the game starts.
    This is the default.
  - `append` — new sessions add to the existing log.

- **enable_exports** — set to `1` (default) to save your main pawn
  on every inn rest. Set to `0` to stop creating new saves; existing
  saves stay on disk and remain hireable.

- **enable_updates** — set to `1` (default) to write a hired pawn's
  gear and knowledge back to its saved copy when you save the game.
  Set to `0` to leave saved copies untouched; hired pawns will revert
  to their saved state the next time you summon them.

## Importing a saved pawn into your main pawn slot

`restore_pawn.exe` is a command-line tool that swaps any saved pawn
into your own main pawn slot. To use it:

1. Quit the game and back up `DDDA.sav` from your Steam userdata
   folder. The tool overwrites the save file in place.
2. Find the saved pawn you want under `pawnlib\<level>\`. Each saved
   pawn is a pair of files (`.pawn` and `.xml`) sharing a name like
   `0096E48B`.
3. Open a command prompt next to `restore_pawn.exe` and run:

   ```
   restore_pawn.exe pawnlib\019\0096E48B.pawn "C:\path\to\DDDA.sav"
   ```

   Use the actual path to your save file and the saved pawn you
   picked.
4. Launch the game. Your main pawn now has the imported pawn's
   skills, gear, vocation, knowledge, augments, and inclinations.
5. Rest at an inn. This refreshes the checkpoint save — without
   this step, loading the checkpoint will restore your previous main
   pawn.

What transfers: skills, equipped gear, vocation and vocation level,
augments, inclinations, learned knowledge, study flags.

What does not transfer: appearance (face, body, voice, name). Your
main pawn keeps the look you already had. Use the in-game stylist or
a separate appearance mod for visual changes.

## Releasing hired pawns

Releasing a hired saved pawn through the normal in-game dialog is
safe. The release gift and rating you would send to the pawn's owner
go nowhere — there is no owner, it's your own archive. Only the
pawn's last saved state before release is persisted. To make a
gear or knowledge change stick, save the game (inn rest or quick
save) while the pawn is still in your party.

## Credits

- [**ddda-dinput8**](https://github.com/kubik-jaroslav/ddda-dinput8)
  by kubik-jaroslav — the mod loader that lets pawnlib run alongside
  the game. pawnlib cannot start without it.
- [**ddsavetool**](https://www.fluffyquack.com/) by FluffyQuack — the
  reverse-engineering reference for the DDDA save file format. Its
  decryption pipeline made the whole project feasible.

## Source

Source code, build instructions, and issue tracker:
<https://github.com/istvan-sipos/pawnlib.git>

## License

pawnlib is released under the MIT License. See the `LICENSE` file in the
release archive (or the source repository) for the full text.
