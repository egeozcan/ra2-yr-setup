# Menu investigation — 2026-09-15

Starting files saved in `backups/menu-investigation/` (binary copies, CRLF preserved).
Steam launch options and persistent DLL override have not been edited.

## Baseline

Launched RA2MD.exe through SteamLinuxRuntime_4 and Proton Experimental, with
WINEDLLOVERRIDES=ddraw=b, no gamescope, RA2MD.INI 1024x768.
Verified process maps point to Proton's builtin ddraw.dll.
User confirmed: menus open and skirmish dropdowns are visible.

Direct gamemd.exe launch attempts were invalid (wrong working directory / String
Manager initialization error). Always launch RA2MD.exe via the Steam runtime.

## Wrapper only — failed

Same resolution and launch path, without the temporary environment override.
Verified process maps point to the game-directory cnc-ddraw DLL.
User confirmed skirmish dropdowns missing. This isolates cnc-ddraw from gamescope.

## Upstream checks

https://github.com/FunkyFr3sh/cnc-ddraw/issues/220
https://github.com/FunkyFr3sh/cnc-ddraw/wiki/How-to-fix-Red-Alert-2-and-Yuri's-Revenge-UI-glitches

The documented UTF-8 system locale problem is not present: this prefix has ACP=1252,
OEMCP=437 and DPI=96. No registry change made.

## Child repaint disabled — failed

Added fixchilds=1 only in [gamemd]. All other settings unchanged.
Source inspection: util_enum_child_proc in src/utils.c copies primary surface
pixels over direct child windows every frame with fixchilds=2. Mode 1 retains
detection without that BitBlt.

User reported menu fails to finish loading / cannot navigate with fixchilds=1. Reverted.

## Hook 3 — failed

Restored starting ddraw.ini and added hook=3 in [gamemd] only.
User confirmed dropdowns still missing. Reverted.

## Child drawing redirected — failed

Restored starting ddraw.ini and added fixchilds=3 in [gamemd] only.

User confirmed dropdowns still missing with fixchilds=3. Reverted.

## TS-DDraw 1.1.4.15 — dropdowns visible

Replaced only ddraw.dll after restoring starting ddraw.ini. Same 1024x768
resolution, no gamescope, existing native,builtin registry override.
Composited capture ts-ddraw-skirmish-1024.png shows all dropdowns.
Interaction and final high-resolution setup verification pending.

## TS-DDraw final setup — not accepted

User confirmed working dropdowns and match at 2560x1440 with gamescope, but
placement was wrong. Adding -w 2560 -h 1440 to gamescope centred the battlefield:
Spectacle capture measured 2560x1440+1280+0 on the 5120x1440 display.
However, user correctly reported that menus remain off-centre and a stale splash
image remains behind them. This is NOT a completed fix.

Steam was briefly shut down in preparation for saving launch options, but no VDF
edit was made. Steam was restarted; the direct test launcher requires Steam running.
backups/working and apply-working.sh were provisionally updated to TS-DDraw before
this menu issue was reported; these must be finalized or corrected before handoff.

Current test: TS-DDraw + gamescope -w 2560 -h 1440 -W 5120 -H 1440 -f.
User requests no changes/restarts while they are testing. Await completion of test.

## cnc-ddraw 6.3 — windowed failed, fullscreen passed menus

Replaced TS-DDraw with cnc-ddraw 6.3.0.0, retained original INI and
gamescope -w 2560 -h 1440 -W 5120 -H 1440 -f.
User: menus centred, dropdowns missing.

Changed ONLY windowed=true to windowed=false within [gamemd].
User: dropdowns visible and usable, menu centred.
The stale splash also clears. Final gameplay check pending.
This isolates forced windowed mode as a cause on cnc-ddraw 6.3;
7.1 fullscreen was not tested, so a downgrade requirement is not established.

## Final verification and persistence

User confirmed: dropdowns work and menu is centred; match works and is centred
at 2560x1440. User finished testing and closed the game.
Composited gameplay capture: cnc-ddraw63-centered-gameplay.png,
measured 2560x1440+1280+0 on the 5120x1440 display.
Main menu capture: cnc-ddraw63-centered-main-menu.png.

With Steam and the game closed, saved launch options:
gamescope -w 2560 -h 1440 -W 5120 -H 1440 -f -- %command%
Previous localconfig.vdf backed up under backups/menu-investigation/.
Refreshed backups/working with the final cnc-ddraw 6.3 files and updated
apply-working.sh, README.md and HANDOFF.md. The earlier provisional TS-DDraw
backup was replaced. DLL SHA256:
f71bf13e02503f08b1604cfe8b906656417435586188c9b5f1865f55749dd639
