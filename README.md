# Red Alert 2 / Yuri's Revenge — Linux setup

Steam app **2229850**, Bazzite / KDE Wayland, Proton Experimental,
5120×1440 monitor. Updated 2026-09-15.

Paths below are written as `<GAME>` (the game directory), `<PREFIX>` (the Proton prefix,
`steamapps/compatdata/2229850/pfx`) and `<REPO>` (this folder). The actual values for this machine,
plus its network and display details, are in `LOCAL.md`, which git ignores. The scripts still
hard-code the game directory (`GAME`/`G` near the top of each) and the display output name.

## Working configuration

| Component | Setting |
|---|---|
| Renderer DLL | cnc-ddraw **6.3.0.0** |
| `RA2MD.INI` | `ScreenWidth=2560`, `ScreenHeight=1440` |
| `ddraw.ini` global | `renderer=gdi` |
| `ddraw.ini` **[gamemd]** | **`windowed=false`**, `fullscreen=false`, `width=0`, `height=0` |
| Wine DLL override | `"ddraw"="native,builtin"` |
| Steam launch options | `gamescope -w 2560 -h 1440 -W 5120 -H 1440 -f -- %command%` |

`fullscreen=false` is cnc-ddraw's optional stretch-to-desktop switch;
`windowed=false` is what selects fullscreen operation. Keep that distinction.
Gamescope centres the game's output on the ultrawide display.

Choose **Yuri's Revenge** when launching from Steam. The final menu and gameplay
checks cover Yuri's Revenge. Base Red Alert 2 was not fully retested.

## Menu fix

Forcing cnc-ddraw into windowed mode made all Skirmish combo boxes disappear.
On cnc-ddraw 6.3, changing only `[gamemd] windowed=true` to `windowed=false`
restored usable dropdowns and a centred menu without a stale splash image.

The 6.3 fullscreen configuration is the tested result. Version 7.1 fullscreen
was not tested, so a downgrade requirement has **not** been established.

TS-DDraw restored dropdowns, but under gamescope it left menus off-centre with
an old splash image behind them. It is not the final renderer.

## Restore and evidence

Run `./apply-working.sh` to restore the saved game files and ensure the Wine
DDraw override exists. It prints the required Steam launch options; it does
not edit Steam settings. Close the game before restoring files.

- `backups/working/`: final configuration.
- `backups/menu-investigation/`: exact files at the start of menu investigation.
- `backups/cnc-ddraw-previous-working/`: former 7.1 high-resolution setup.
- `backups/stock/`: EA stock files.
- `logs/menu-investigation/RESULTS.md`: test results and limitations.
- `logs/menu-investigation/cnc-ddraw63-centered-main-menu.png`: menu capture.
- `logs/menu-investigation/cnc-ddraw63-centered-gameplay.png`: gameplay capture,
  measured **2560×1440+1280+0** on the full display.
- `logs/menu-investigation/HANDOFF.before-menu-fix.md` and
  `README.before-menu-fix.md`: historical notes.

Final DLL SHA256:
`f71bf13e02503f08b1604cfe8b906656417435586188c9b5f1865f55749dd639`

`restore-stock.sh` is an emergency game-directory rollback. It leaves the Wine
native override active, which is incompatible with EA's DDrawCompat on this
setup. Read its warning before using it.

## Debugging constraints

- Preserve INI CRLF line endings and edit only while the game is closed.
- Do not restart or change settings while the user is testing.
- Use `spectacle -b -n -f -o file.png` for composited screenshots. X window-buffer
  captures were misleading here. A sleeping display also produces black captures.
- Verify `/proc/<gamemd PID>/maps` contains the game-directory `ddraw.dll`.
- Keep 2560×1440; 5120×1440 crashed the game engine in the earlier investigation.
- Steam must be running even for the manual Proton test launch.
- Launch `RA2MD.exe` through Steam's runtime. Direct `gamemd.exe` attempts caused
  String Manager initialization errors. Update 2026-09-25: a direct launch with the game directory as
  the working directory works; see the quick skirmish launcher section.
- Do not infer that menus are correct from a centred battlefield: verify both.

## Source

Official renderer release:
https://github.com/FunkyFr3sh/cnc-ddraw/releases/tag/v6.3.0.0

## Steam Remote Play (2026-09-16)

The failed stream logged `k_ECaptureFailedReasonPipewireRequired`: Steam was
running on KDE Wayland without `-pipewire`. Added that flag to every Steam
Exec entry in `~/.config/autostart/steam.desktop` and the user launcher
`~/.local/share/applications/steam.desktop` (copied from the system launcher).
Original launchers are saved in `backups/steam-pipewire/`.

Steam was restarted through `/usr/bin/bazzite-steam -pipewire -silent`.
For manual terminal launches, use `/usr/bin/bazzite-steam -pipewire`.
Select the monitor and click **Share** in KDE's sharing dialog if prompted.
The flag belongs to Steam itself; the game's launch options remain as above.
A successful stream still needs verification from the receiving computer.

On the first retry, Steam correctly used `-pipewire` but logged
`k_ECaptureFailedReasonPipewirePermissions`. The user confirmed dismissing
the monitor-sharing prompt. Restarted KDE's portal service and Steam to
request sharing again. Select the monitor and click **Share** before retrying.

### Temporary Remote Play test: Gamescope disabled

After permission was granted, the 08:29 stream sent audio but never started
a video encoder. User closed Yuri before this test. Removed only the game's
Steam `LaunchOptions` Gamescope wrapper with Steam stopped, then restarted
Steam with `-pipewire`. Game INIs and renderer remain unchanged.
The working-configuration table above describes the original local-play
setup; the current launch options are temporarily empty for this test.
Restore through Steam Properties → Launch Options with:
`gamescope -w 2560 -h 1440 -W 5120 -H 1440 -f -- %command%`.
Backup: `backups/steam-pipewire/localconfig.immediately-before-no-gamescope.vdf`.
Streaming and screen layout without Gamescope still need user verification.

### Test result: original Gamescope setup restored

The no-Gamescope test produced a poor layout and slow streaming. Steam's
2026-09-16 08:38 session statistics reported 3024×852 output, 45.05 ms average
capture time, 21.78 FPS, and 3.29 ms average ping. Restored the original
Gamescope launch options after confirming Yuri had exited, with Steam stopped
during the edit. Restarted Steam with `-pipewire`. The working-configuration
table above now matches the active game launch options again.

Sunshine is already running as a Flatpak; its startup log reports DMA-BUF
capture and Vulkan hardware encoders. No Sunshine configuration changes
were made. Moonlight/Sunshine with a 2560×1440 streaming display is a proposed
next test, not a verified fix.

## Moonlight / Sunshine profile (2026-09-16)

Sunshine now has **Yuri - 1440p**, alongside the existing Desktop profile.
The host's LAN address is in `LOCAL.md`. In Moonlight on the client,
select 2560×1440, 60 FPS, around 25 Mbps, then open this profile. It opens
Steam's library; select **Yuri's Revenge** explicitly.

`sunshine-display.py start` saves the main display output's current mode and selects
2560×1440 at approximately 60 Hz. Sunshine's prep-command undo runs
`sunshine-display.py stop` to restore the saved mode when the app/session is
quit. Merely disconnecting can leave the session resumable and the display
in streaming mode. Close Yuri before choosing Moonlight's **Quit Session**.
Manual recovery: `python3 <REPO>/sunshine-display.py stop`.

Steam launch options are now:
`/usr/bin/python3 <REPO>/yuri-gamescope.py %command%`
The wrapper retains the tested Gamescope game size (2560×1440), fullscreen
behavior and renderer, but chooses the output size from the main display output's active mode.
It produces the original 5120×1440 output locally and 2560×1440 for this
streaming profile. This supersedes the literal launch options in the table.

Verified: actual Flatpak-to-host resolution switch, wrapper arguments at
both resolutions, restoration to the original mode, and Sunshine restart.
Client streaming smoothness and in-game layout still require user verification.
Sunshine capture/encoder defaults were retained; startup probes use DMA-BUF
and Vulkan hardware encoders. Backups are under `backups/sunshine/`.

Pairing repair: PIN pairing produced a duplicate of the existing client's
certificate. Sunshine 2026.914 rejects multiple records matching one
certificate. Backed up the state file, retained the original enabled client
entry, removed only the duplicate certificate entry, and restarted the normal
Sunshine service. Client reconnection still requires verification.

## Mod: Liberator, an Allied-only Tesla tank (2026-09-25)

Lessons learned (engine facts, art conventions, voxel lighting, pitfalls): `mod/MODDING-NOTES.md`.

An overpowered Tesla tank with its own model, cameo and name, buildable by every **Allied** country
(America, Korea, France, Germany, Britain; changed from America-only on 2026-09-25).
It is installed as loose files in the game directory; these override the stock copies in the
MIX archives. Only Yuri's Revenge reads them. No MIX file is modified.

| Installed file | Content |
|---|---|
| `rulesmd.ini` | stock rules (`expandmd01.mix`) + unit `ATTNK` (list entry 85), weapons, warhead |
| `artmd.ini` | stock art (`ra2md.mix/localmd.mix`) + `[ATTNK]` art entry |
| `ra2md.csf` | stock strings (`langmd.mix`) + `Name:ATTNK` = "Liberator" |
| `aimd.ini` | stock AI (`ra2md.mix/localmd.mix`) + a Liberator team for Allied AI |
| `attnk.vxl/.hva`, `attnktur.vxl/.hva` | hull and turret models |
| `attkicon.shp` | 60×48 sidebar cameo |

Commands:
- `python3 mod/build-tesla-mod.py install` builds every file and copies it into the game directory.
- `python3 mod/build-tesla-mod.py uninstall` removes all of those files.
- `mod/make_graphics.py` regenerates `mod/assets/` from `liberator_model.py`. It needs numpy
  and Pillow; install itself needs neither, because it only copies `mod/assets/`. To set up:
  `python3 -m venv mod/.venv && mod/.venv/bin/pip install numpy pillow`, then
  `cd mod && .venv/bin/python make_graphics.py`.
- **To rename:** change `UNIT_NAME` in `build-tesla-mod.py` and the cameo label
  (`"Liberator"` in `make_graphics.py` `main`). Then run `make_graphics.py` and `install`.
- Previews: `mod/previews/liberator-model.png` and `liberator-cameo.png`.
- Libraries: `liberator_model.py` (the model), `mixextract.py` (encrypted MIX reader), `vxl.py` (VXL/HVA), `csf.py`
  (string table), `render.py` (preview renderer and SHP read/write).

Art: a completely custom model, built from primitives by `mod/liberator_model.py`.
It shares no geometry with the Tesla tank.
- **Hull** (48×31×12 voxels):
  - a low, angular body with a sloped glacis;
  - house-colour armour skirts over the tracks, with road wheels below them;
  - a recessed engine grille, headlights, and white stars on the fenders.
- **Turret** (36×24×21 voxels, pivoting at the hull centre, sitting on the hull top at z 9.5):
  - a faceted house-colour turret with a steel roof;
  - a copper-wound Tesla cannon ending in a glowing cyan electrode ball;
  - twin capacitor banks, and a rear Tesla mast with copper rings and an orb.
- **Cameo:** rendered from the model and labelled LIBERATOR in the stock label style.
- **Previews** in `mod/previews/`: `liberator-model.png` (turret at several angles),
  `liberator-ingame-scale.png` (about 1 px per voxel) and `liberator-cameo.png`.
- **Palette:** every colour index used is identical in all six theatre unit palettes.
- **Muzzle position:** the fire point is still the stock `PrimaryFireFLH=60,0,100`. The
  electrode sits about 3 voxels further forward and lower than the stock coil tips, so check
  where the bolt starts in game.
- **Previous version:** the earlier recolour of the stock model is kept in `mod/assets-recolor/`,
  with its previews in `recolor-*.png`. To go back to it, copy it over `mod/assets/` and run `install`.
  Its generator code was removed, so that folder is the only copy. Running `make_graphics.py`
  afterwards overwrites it with the custom model again.

Stats compared with the stock Tesla tank:
- 1500 HP (stock 300), speed 4 (stock 6; was 9, halved and rounded down because Speed is an integer), self-healing, immune to mind control and radiation.
- Weapon: 300 damage, range 7; elite: 450 damage, range 8. The bolt chains between targets.
- Warhead `LibertyElectric` does full damage to buildings.
- Requires an Allied War Factory and an Allied Battle Lab (`GAWEAP,GATECH`). The Battle Lab
  itself needs an Air Force Command. Cost 3000 (was 1500). Crates can't grant it.
- **AI:** an AI playing any Allied country builds teams of 3 Liberators and attacks with them once it
  owns a Battle Lab. The setup copies the German Tank Destroyer team: weight 500, the stock
  "General Vehicle Attack" script, one team at a time, on every difficulty. Soviet and Yuri
  AI never builds it.

The in-game check has not been done yet. Old saves and online matches may not work
while the mod is installed; uninstall it for those. `restore-stock.sh` does not
remove these files; run the uninstall command as well.

## Mod: Cheat Defense (2026-09-25)

An observer's building, added by the same installer (`mod/build-tesla-mod.py`, `DEF_*` settings).
It lets the user watch the AI without losing. `[CHEATDEF]` (list entry `407`) is a full copy
of the Grand Cannon (`GTGCAN`) with these changes:

- **Build:** any country can build it, in the defence tab, with only a Construction Yard.
  It costs 1 credit and needs no power (`Power=0`, `Powered=no`). `AIBuildThis=no`, and it
  appears in no AI list or team, so the AI never builds it.
- **Placement:** `Adjacent=255` means it can be placed up to 255 cells from any of your
  buildings. `BaseNormal=no` means it does not extend your base for other buildings. Put the
  first one on ground you can already see; placement may be slower while it is on the cursor.
- **Map reveal:** `SpySat=yes` reveals the whole map, like the Spy Satellite Uplink.
- **Defence:**
  - `Immune=yes` makes it invulnerable. It also has 10000 HP, can't be captured, drained or
    mind-controlled, and the turret turns fast.
  - Weapon `CheatBolt`: 10000 damage, fires every 10 frames, range 15 (the Grand Cannon's),
    no minimum range.
  - It hits both ground and air (`CheatProj`: `AA=yes`, `AG=yes`) and fires a Tesla lightning bolt.
  - Warhead `CheatWH` does 100% damage against every armour type, with no splash.
- **Art:** its own art, generated by `mod/cheatdef_art.py` from one procedural 3D scene. The scene is
  built from signed distance functions and ray-marched at the stock voxel scale (1 px per unit; a cell is 42.4 units).
  - **Base** (`ggchdf.shp`, 232×168 canvas like the Grand Cannon): a dirt and concrete ground pad
    and a concrete foundation. On top sits a tall house-colour drum with dark ribs, a chrome band and a steel rim.
    The deck has a glowing channel ring, and four tall gunmetal pylons with chrome bands and house-colour sleeves
    carry blue emitter crystals. There are normal and two damaged frames plus shadows; the damaged frames never
    show, because the building is `Immune`.
  - **Style (2026-09-25 restyle after the first in-game look):** the colours follow a count of stock Allied building
    pixels. The art is mostly dark gunmetal (48–63), olive grime (112–127), khaki concrete (64–79) and steel (80–95).
    House colour is used on panels, white only in chrome highlights, and small icy-blue lights replace the earlier
    neon cyan. Shading uses a low ambient light, ambient occlusion in crevices, grime noise and chrome reflections.
  - **Turret** (`chdftur.vxl/.hva`, the voxel `TurretAnim`, scale 1.3): modelled on the stock voxel turrets.
    It is a tall angular house-colour housing with a sloped steel front plate, leaning side armour behind dark
    gaps, a stepped dark rear block with capacitor drums, and a roof hatch. At the front are an eye and twin steel
    prongs with bronze coils and glowing tips; on the roof is a mast holding a caged glowing orb. It is a single
    piece with no barrel, and `TurretRecoil=no`.
    - **What the stock turrets do** (Grand Cannon, Patriot, Flak, Laser, Outpost, analysed from their voxels):
      - flat colours from the dark end of each ramp (house colour always 23, steel 92–94, greys 55–57);
      - no painted-in shading;
      - large faces at distinct angles, so the engine's lighting table makes each face a different shade.
      Our turret copies that: dark base colours (remap 22, steel 91, grey 55) and big faceted forms.
    - **Voxel colours go through `voxels.vpl`,** the game's 32-level voxel lighting table. The first turret used
      index 251 for the glow, which the table maps to magenta (204–207). Glow now uses 192/81/193.
    - `VPL_LIGHT` is the voxel light model fitted to two in-game screenshots at different facings. Faces pointing
      screen lower-left are lit, and up-facing tops only get level ~4. So tops use lighter indices and undersides
      darker ones, plus a mild sky-visibility and edge bake. All of these are symmetric about z, so they stay
      right as the turret turns.
    - The previews and the last build-up frames draw the voxel through this model, so they show roughly what
      the game does.
  - **Build-up** (`g?chdfmk.shp`, 12 frames plus shadows): the plinth rises with welding sparks along the
    growing edge, then the pylons grow, the turret assembles facing north, and the lights come on in the last frames.
    There is one identical copy per theater letter (T A U D L N) plus generic `G`.
  - **Active animation** (`chdfglow.shp`, `[CHDFGLOW]`, 16-frame loop, `LoopEnd=15` inclusive): a bright wave runs round the deck
    ring, the crystals flicker, and electric arcs crawl from the pylons to the ring.
  - **Cameo** (`chdficon.shp`): the building on a dark scanning-grid backdrop, labelled CHEAT DEFENSE.
  - Art ID `GTCHDF` with `NewTheater=yes`. The base and anim rely on the stock fallback to letter `G`,
    the same way stock `gggcan.shp` and `ngtsla_a.shp` work.
  - Only colour indices identical in all six theater unit palettes are used.
  - `TurretAnimX=0` (stock Grand Cannon: 3), because the base and pivot are exactly centred.
  - `PrimaryFireFLH=270,0,225` aims at the prong tips. It is an estimate from about 6 leptons per unit, so check
    where the bolt starts in game.
  - Previews: `mod/previews/cheatdef-*.png`: building with turret facings and anim, scale next
    to the stock plate, build-up, anim, damage, and cameo.
  - Regenerate with `cd mod && .venv/bin/python cheatdef_art.py` (about 30 s), then run `install`.
- **Still to check in game:** where the bolt starts; whether the turret sits centred on the deck; whether the
  anim ring or arcs paint over the prongs (if so, move `ActiveAnimZAdjust` from -30 towards 0); and whether the build-up
  hands off to the turret without a pop.
- **Known gaps:** Iron Curtain or Force Shield targets and submerged submarines may still survive.
  The in-game check has not been done yet.

## Quick skirmish launcher (2026-09-25)

`spawner/` starts Yuri's Revenge straight into a skirmish, with no intro and no menus. It is for testing mods.
- `python3 spawner/spawn.py install` builds `yspawn.dll`. On first use it also builds a podman image with the
  32-bit mingw compiler. It then writes `gamemd-spawn.exe` into the game directory. That file is a copy of
  `gamemd.exe` that also loads `yspawn.dll`, made by `add_import.py`. The stock `gamemd.exe` and the normal
  Steam launch are not changed.
  - cnc-ddraw picks its settings section by exe name. Its stock `[gamemd-spawn]` section (meant for CnCNet)
    lacks `windowed=false`, so `install` copies the `[gamemd]` settings into it.
  - `apply-working.sh` restores the backed-up `ddraw.ini`, so run `install` again after using it.
- `python3 spawner/spawn.py run [MAP]` starts a match with the settings in `spawner/yspawn.ini`: map, country,
  colour, credits, speed and AI opponents. `MAP` is any `.mmx`/`.yro` file in the game directory. The launch
  uses the same gamescope, SteamLinuxRuntime_4 and Proton path as Steam, and Steam must be running.
  When a match ends, the game exits instead of showing the menu.
- `python3 spawner/spawn.py uninstall` removes `gamemd-spawn.exe`, `yspawn.dll/.ini/.map/.log`.
- The map inside a `.mmx`/`.yro` archive is the one named in its `.pkt` packet. It is not always named after the
  archive: `amazon.mmx` holds `AMAZON01.map`. Before 2026-09-25 `run` could not start that map.
- Every launch copies the `[gamemd]` settings into `[gamemd-spawn]` again, so a launch after `apply-working.sh`
  keeps the tested display settings.
- `yspawn.log` in the game directory records each start step. A crash writes `except.txt` there too, and its
  `Eip` shows where the crash happened.
- **Start positions and teams** (added 2026-09-25): `Start=` (0–7, or -1 for random) and `Team=` (0–3 = A–D,
  or -1 for none) in `[Settings]` and in each `[AIn]` section. Players on the same team are allied from the start.
  Before this, your start was always position 0.

### Skirmish Setup window (2026-09-25)

`spawner/skirmish.py` is a GTK 4 / libadwaita window for setting up the match, then starting it through `spawn.py`.
It must run with `/usr/bin/python3`; the linuxbrew `python3` on PATH has no GTK bindings.
- **Start it:** `/usr/bin/python3 spawner/skirmish.py`, or **Skirmish Setup** in the application menu.
  `--install-desktop` writes the menu entry, `~/.local/share/applications/ra2yr-skirmish-setup.desktop`.
- **Maps:** a searchable list of every map in the game directory, with its in-game name, player count and
  preview picture. The names come from the game's string table and the pictures from the map's `[PreviewPack]`
  (decoded by `spawner/mappreview.py`). The start positions are drawn over the preview as numbered circles.
  A position someone has chosen is filled with that player's colour.
- **Settings:**
  - your name;
  - a players table, with a row for you and one per opponent. Each row has country, colour, team (none or A–D) and
    start position (random or 1 to the map's player count), plus difficulty for opponents;
  - up to 7 opponents, but only as many as the map allows;
  - speed, credits, starting units and tech level;
  - the rule switches: bases, short game, superweapons, crates, MCV repacks, build off ally, bridges.
- **Random:** Random country or colour is picked when you press Start. Random colours never repeat a colour
  that is already taken.
- **Game mode:** always Battle (`GameMode=1`). `mpmodesmd.ini` says it is the only mode that allows AI players.
  The list shows only maps made for it (`GameMode=standard` in the map's packet), which is currently all of them.
- **Start is disabled** while there is a problem:
  - too many players for the map;
  - two players with the same colour or the same start position;
  - everyone on the same team;
  - the game is already running, or it was started less than 20 s ago;
  - Steam is not running;
  - the launcher is not installed. A banner then offers **Install**, which runs `spawn.py install`.
- **Your last settings** are saved in `~/.config/ra2-yr-setup/skirmish.json`. The window changes neither
  `spawner/yspawn.ini` nor `spawn.py run`. The first time it opens, its defaults come from that file.
- **Presets:** the folder button in the header saves the current settings under a name (the same name
  overwrites) and lists the saved ones. Click one to load it; the bin deletes it. They are kept in
  `~/.config/ra2-yr-setup/skirmish-presets.json`. If a preset's map is gone, the current map is kept.
- **`--dry-run`:** Start only writes `yspawn.ini` and `yspawn.map` into the game directory, and does not launch.
- **Tested:** the map scan (all 53 archives extract), the settings it writes (the same as the CLI's for the same
  settings), and the window in dry-run mode. It has not yet started a real match.

How it works (`spawner/yspawn.c`):
- The DLL replaces the two calls to the main menu (`0x48CDD3`, `0x48CFAA`) and skips the intro and logo.
- In place of the menu, it:
  - loads the countries and sides (normally done by the menus; without it the start crashes at `0x6847B4`);
  - fills in the session's game options and the AI slots, including each AI's start (`AISlots.Starts`) and
    team (`AISlots.Allies`);
  - adds a player entry for you (`NodeNameType`, with `StartPoint` at `+0x5B` and `Team` at `+0x63`);
  - calls the game's own `StartScenario`.
- Addresses and layouts come from the YRpp headers and CnCNet's `yrpp-spawner`. This is a much smaller,
  skirmish-only version of the spawner's start path.
- Several addresses were checked against this exe: the call sites, the HouseTypeClass vtable (slot 25
  `LoadFromINI` = `0x511850`), and `IsHumanPlayer` at `+0x1EC`.
- The Steam exe is 1.001 (build 2001-10-31, "1.001TUC"). It already has the launcher and copy-protection
  checks patched out. Starting `gamemd.exe` directly with the game directory as the working directory
  reached the main menu (2026-09-25). The earlier String Manager error was probably caused by the wrong
  working directory; that was not reproduced.
- Verified (2026-09-25):
  - `uninstall`, `install` and `run` all worked, and the match started on Tsunami as America against an easy
    Russian AI.
  - The display was centred at 2560×1440, the same as a Steam launch, with the game directory's
    `ddraw.dll` loaded.
  - Shroud was normal and the camera was on our MCV.
  - A memory read showed house 0 = Americans, human, and house 1 = Russians, AI.
  - After a finished match, `yspawn.log` ended with `match over, exiting` and there was no crash report.
- Start positions and teams use the game's own code, the same fields the skirmish menu fills. Traced in this exe
  on 2026-09-25:
  - `ScenarioClass::AssignHouses` (`0x687F10`) copies each player's start into `HouseClass+0x16058` and team into
    `+0x1605C`. For you it reads the start through `0x696F50`, for AIs from `AISlots`.
  - MPGameMode vtable `+0x80` (`0x5D6BE0`) places every house whose start is not -2, and `+0x84` (`0x5D6C70`)
    gives the rest random free positions. So "random" must be **-2**. The DLL turns the ini's -1 into -2.
  - Before this change the AI slots got -1. `0x5D6BE0` then read the start list at index -1 and wrote the house
    index just before `HouseIndices`. The random pass then gave the AI a proper position, so it went unnoticed.
  - `AllyTeams` (vtable `+0x88`, `0x5D74A0`, called unconditionally from `0x686AE4`) allies, both ways, every
    two houses with the same team (-1 and -2 mean no team). `Allies` is the bitfield at `HouseClass+0x5788`
    (`IsAlliedWith`, `0x4F9A10`).
  - `MakeAlly` (`0x4F9B70`) can call the AI "paranoid" check (`0x501640`), which allies the AIs against a
    human. That check returns at once when `ScenarioClass+0x11E0` is set, and `AssignHouses` sets it whenever
    an AI has a team. So teams don't make the AIs gang up on you, the same as with the menu.
  - The start list (`0x688380`) holds waypoints 0, 1, 2, … in order. All 53 maps define waypoints
    0 to max−1 with no gaps, so start k is always waypoint k.
  - The 1-based numbers in Skirmish Setup are the map's waypoints 0–7. On the preview, cell (x, y) is at
    x − y + W, (x + y − W) / 2, where W is the `[Map] Size` width. This matches the game's red markers to within a pixel.
  - **Not yet checked in game:** the build is installed but no match has been played with teams or fixed starts.
- The Cheat Defense has `SpySat=yes`, so building one reveals the whole map. That is intended and is not
  caused by the launcher.
