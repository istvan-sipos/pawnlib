pawnlib
=======

A pawn archive mod for Dragon's Dogma: Dark Arisen.

Every time you rest at an inn, pawnlib saves a copy of your main pawn to
disk. Later, you can hire any of those saved pawns from the rift, just
like a regular online pawn. If you change their gear or gain enemy /
quest / location knowledge while they are in your party, the changes are
written back to the saved copy when you save the game.

A separate command-line tool, restore_pawn.exe, can promote any saved
pawn into your own main pawn slot - skills, gear, vocation, knowledge,
inclinations, AND visible appearance (face, body, hair, colors, name,
voice, moniker).


FEATURES
--------

- Auto-saves your main pawn on every inn rest.
- Lets you re-hire any saved pawn from the rift, sorted newest first.
- Updates a saved pawn's gear, knowledge flags, AND progression
  counters (kill counts, encounter time, unique-event counters) when
  you save the game - so a hired pawn keeps accumulating progress
  toward knowledge unlocks across multiple hires.
- Restore tool to swap any saved pawn into your main pawn slot,
  covering both the live save and the death/respawn checkpoint copy.
- Works fully offline; pawnlib does not change your participation in
  the live rift - other players can still hire your pawn as normal.


REQUIREMENTS
------------

- Dragon's Dogma: Dark Arisen on PC (Steam version). Linux + Proton
  works too.
- dinput8 - allows loading of external dll's into the game.
  https://www.nexusmods.com/dragonsdogma/mods/96
- An active Steam login. pawnlib uses your real Steam session; you
  do not need any emulator or workaround.


INSTALLATION
------------

1. Install dinput8 first. Its dinput8.dll and dinput8.ini should sit
   next to DDDA.exe in your game folder.

2. Download the latest pawnlib release archive.

3. Extract its contents into the same folder where dinput8.dll
   lives. You should end up with these new files next to DDDA.exe:

       dinput8.dll
       dinput8.ini
       pawnlib.dll
       pawnlib.ini
       restore_pawn.exe
       README.txt

4. Open dinput8.ini in a text editor. Under the [main] section, add
   or change this line:

       loadLibrary = pawnlib.dll

5. Launch the game. On first run a pawnlib.log file appears next to
   DDDA.exe so you can confirm the mod loaded.

6. Rest at an inn. A new folder named pawnlib will be created next
   to DDDA.exe; that is where your saved pawns live.

That's it. From then on, every inn rest adds another saved pawn, and
the rift's "Search by level" list shows your saved pawns instead of
online players.


CONFIGURATION
-------------

The mod reads pawnlib.ini once when the game starts. The options:

max_search_results - how many saved pawns the rift shows per level.
Range 0 to 100. Default 100. Set to 0 to make the rift look empty
(useful if you want to browse archives outside the game without the
rift cluttering up).

logging - controls the pawnlib.log file.
  disabled - no log is written.
  truncate - log is reset to empty each time the game starts. Default.
  append   - new sessions add to the existing log.

enable_exports - set to 1 (default) to save your main pawn on every
inn rest. Set to 0 to stop creating new saves; existing saves stay
on disk and remain hireable.

enable_updates - set to 1 (default) to write a hired pawn's gear and
knowledge (flags AND progression counters) back to its saved copy
when you save the game. Set to 0 to leave saved copies untouched;
hired pawns will revert to their saved state the next time you
summon them.


IMPORTING A SAVED PAWN INTO YOUR MAIN PAWN SLOT
-----------------------------------------------

restore_pawn.exe is a command-line tool that swaps any saved pawn
into your own main pawn slot. To use it:

1. Quit the game and back up DDDA.sav from your Steam userdata
   folder. The tool overwrites the save file in place.

2. Find the saved pawn you want under pawnlib\<level>\. Each saved
   pawn is a set of files (.pawn, .meta, .xml) sharing a name like
   0096E48B.

3. Open a command prompt next to restore_pawn.exe and run:

       restore_pawn.exe pawnlib\019\0096E48B.pawn "C:\path\to\DDDA.sav"

   Use the actual path to your save file and the saved pawn you
   picked.

4. Launch the game. Your main pawn now has the imported pawn's
   appearance, name, voice, moniker, skills, gear, vocation,
   knowledge, augments, and inclinations. The change survives a
   death-and-respawn without needing an inn rest first, because
   the tool patches both the live mCmc[0] copy and the checkpoint
   snapshot the game falls back to on respawn.


RELEASING HIRED PAWNS
---------------------

Releasing a hired saved pawn through the normal in-game dialog is
safe. The release gift and rating you would send to the pawn's
owner go nowhere - there is no owner, it's your own archive. Only
the pawn's last saved state before release is persisted. To make a
gear or knowledge change stick, save the game (inn rest or quick
save) while the pawn is still in your party.


CREDITS
-------

dinput8 by kubik-jaroslav - allows loading of third party dll's
  https://www.nexusmods.com/dragonsdogma/mods/96
  https://github.com/kubik-jaroslav/ddda-dinput8


SOURCE
------

Source code, build instructions, and issue tracker:
https://github.com/istvan-sipos/pawnlib.git
