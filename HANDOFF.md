# HANDOFF — menu fix verified, 2026-09-15

Read README.md for the final configuration and restore instructions.

## Result

User confirmed Skirmish dropdowns are visible and usable, menus centred, and
2560×1440 gameplay works and is centred. The game was closed before saving.

Final setup:

- **cnc-ddraw 6.3.0.0** in the game directory.
- `ddraw.ini` **[gamemd] `windowed=false`**. Keep this: `windowed=true` broke the
  dropdowns in the controlled comparison. Global renderer remains GDI.
- RA2MD.INI: 2560×1440.
- Persistent Wine override remains `ddraw=native,builtin`.
- Steam launch options saved as:
  `gamescope -w 2560 -h 1440 -W 5120 -H 1440 -f -- %command%`
- Final game files are in `backups/working`; `apply-working.sh` restores them.
- Base Red Alert 2 was not fully tested. Select Yuri's Revenge in Steam.

## What was established

1. Builtin Wine DirectDraw, no gamescope, 1024×768: dropdowns work.
2. cnc-ddraw 7.1, no gamescope, same resolution: dropdowns missing.
3. fixchilds=1, fixchilds=3 and hook=3 did not solve this.
4. TS-DDraw makes dropdowns work, but leaves off-centre menus and stale splash
   graphics under gamescope. It is not the final solution.
5. cnc-ddraw 6.3 + windowed=true: centred menus, missing dropdowns.
6. Change only [gamemd] windowed=false: centred menus, working dropdowns,
   splash clears. User verified gameplay on the same setup.

**Do not claim 7.1 requires downgrading:** fullscreen on 7.1 was not tested.
6.3 is retained because this exact combination passed the checks.

## Evidence and precautions

`logs/menu-investigation/RESULTS.md` contains the experiment record.
Separate main-menu and gameplay Spectacle captures are stored beside it.
Gameplay capture measures 2560×1440+1280+0 on the 5120×1440 display.

The old handoff and README are archived in `logs/menu-investigation/`.
Previous 7.1 working files are in `backups/cnc-ddraw-previous-working/`.
Exact files at investigation start are in `backups/menu-investigation/`.

Do not fiddle with settings or restart while the user is testing. Wait for their
report. Preserve INI CRLF bytes, and edit only with the game closed.

Steam must be running for manual Proton launches. Use RA2MD.exe through Steam's
runtime; direct gamemd.exe tests caused String Manager initialization errors.
Do not use `steam -applaunch 2229850` to assume Yuri's Revenge: it opened base RA2.
