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
- Previews: `make_graphics.py` also rewrites `mod/previews/liberator-*.png`.
- Libraries: `liberator_model.py` (the model), `mixextract.py` (encrypted MIX reader), `vxl.py` (VXL/HVA), `csf.py`
  (string table), `render.py` (preview renderers, including `draw_vpl`, which draws voxels through
  `voxels.vpl` like the game, and SHP read/write).

Art: a completely custom model, built from primitives by `mod/liberator_model.py`.
It shares no geometry with the Tesla tank.
- **Style (2026-09-26 restyle to match the stock Allied vehicles):**
  - It uses the palette indices the Grizzly, Prism Tank and Tank Destroyer use: blue-grey steel
    88/90/92/94, dark greys 55–59 for running gear and details, and house colour 23/26 on trim.
    Each material picks its index by face direction (top, side, underside), and colours are otherwise
    flat, so the engine's voxel lighting does the shading.
  - The Tesla parts follow the stock Tesla tank: grey coil discs (50/51/13) with dark gaps.
    The only light is a small blue-white electrode (192/193).
  - Like the stock models, only the outer shell of voxels is stored.
  - Drawn through `voxels.vpl` at 1 px per voxel, its average luminance is 80, close to the stock
    Allied tanks' 69–77, with a similar colour mix. The first version measured 112 and used pale
    lavender steel, orange copper, neon cyan and yellow lights.
- **Hull** (48×31×12 voxels):
  - a stepped deck with a sloped glacis, driver hatch and vents, and lower fenders beside it;
  - house-colour front fenders and three-panel side skirts;
  - tracks with road wheels, sprocket and idler;
  - stowage boxes and headlights on the fenders, two engine grilles and exhausts at the rear.
- **Turret** (38×24×13 voxels, pivoting at the hull centre, sitting on the turret ring at z 12):
  - a faceted steel turret with house-colour cheeks and a rear bustle;
  - a Tesla gun of four coil discs ending in the electrode;
  - two capacitor coils on the roof, a commander cupola and an antenna.
- **Cameo** (painted 2026-09-26): `mod/cameo-art/liberator.png`, labelled LIBERATOR in the stock label style.
  - How it was made: an image model (fal.ai `nano-banana/edit`) repainted a render of the voxel model in the
    style of the stock cameos. It shows a realistic tank with the coil gun, a glowing electrode, blue sky and
    sand.
  - How it becomes the cameo: `make_graphics.py` crops it (`CAMEO_ART_CROP`), scales it to 60×48 and maps it to
    `cameo.pal`.
  - Without that file, the cameo falls back to a render of the model.
  - For new art, run `mod/.venv/bin/python mod/fal_cameo.py KEYFILE [N]`. It saves N candidates in
    `mod/cameo-art/` (git ignores them). Most come back still looking like voxels, so pick a painted one.
    Copy it to `liberator.png`, set the crop, and run `make_graphics.py`.
  - The key stays in the key file (see `LOCAL.md`); only fal.ai receives it.
- **Previews** in `mod/previews/`, drawn through `voxels.vpl`:
  - `liberator-model.png`: close-ups, two of them with the turret turned;
  - `liberator-ingame-scale.png`: 1 px per voxel at eight facings, enlarged 3×;
  - `liberator-cameo.png`.
- **Palette:** every colour index used is identical in all six theatre unit palettes.
- **Muzzle position:** `PrimaryFireFLH=120,0,100` (`UNIT_FLH` in `build-tesla-mod.py`) points at the
  electrode (`ELECTRODE` in `liberator_model.py`), at about 6 leptons per voxel. A screenshot on
  2026-09-26 showed the bolt starting at the tip of the coil gun.
- **First custom version** (2026-09-25): to get the brighter model back, restore
  `mod/liberator_model.py` from commit `c1980fd` and set `UNIT_FLH` in `build-tesla-mod.py` back to
  `60,0,100`. Then run `make_graphics.py` and `install`.
- **Previous version:** the earlier recolour of the stock model is kept in `mod/assets-recolor/`,
  with its previews in `recolor-*.png`. To go back to it, copy it over `mod/assets/` and run `install`.
  Its generator code was removed, so that folder is the only copy. Running `make_graphics.py`
  afterwards overwrites it with the custom model again.

Stats compared with the stock Tesla tank:
- 375 HP (stock 300; halved twice on 2026-10-01, from 1500 to 750 to 375), speed 2 (stock 6; was 9, then 4, halved again on 2026-09-30 because it was too strong), self-healing, immune to mind control and radiation.
- Weapon: 300 damage, range 7, ROF 140; elite: 450 damage, range 8, ROF 100. The bolt chains between targets.
  Fire rate halved twice on 2026-10-01 (ROF 35/25, then 70/50; ROF is the delay between shots).
- Warhead `LibertyElectric` does full damage to buildings.
- Requires an Allied War Factory and an Allied Battle Lab (`GAWEAP,GATECH`). The Battle Lab
  itself needs an Air Force Command. Cost 3000 (was 1500). Crates can't grant it.
- **AI:** an AI playing any Allied country builds teams of 3 Liberators and attacks with them once it
  owns a Battle Lab. The setup copies the German Tank Destroyer team: weight 500, the stock
  "General Vehicle Attack" script, one team at a time, on every difficulty. Soviet and Yuri
  AI never builds it.

**In-game check (2026-09-26):** screenshots taken with `spawner/showcase.py` (see the quick skirmish
launcher) show the restyled Liberator next to the Grizzly, Prism Tank, Tank Destroyer and Tesla Tank. It
reads as the same family and is closest in tone to the Prism Tank. It is a little lighter than the
Grizzly and Tank Destroyer, because vehicles are lit brighter than `VPL_LIGHT` predicts. Its bolt starts
at the gun tip and chains between targets. No user has played with it yet.

Old saves and online matches may not work while the mod is installed; uninstall it for those. `restore-stock.sh` does not
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
  colour, credits, speed and AI opponents. `MAP` is a `.mmx`/`.yro` path relative to the game directory,
  including archives under `Maps/`. The launch
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
  Players with no team are allied with nobody, including other AIs (the stock game allies all team-less AIs).
  Before this, your start was always position 0.
- **Test units** (added 2026-09-26): an optional `[Units]` section in the ini puts vehicles and buildings on the
  map once the match has loaded. Each line is `n=TYPE,COUNTRY,X,Y,FACING,MISSION`:
  - `TYPE`: a vehicle or building ID such as `ATTNK` or `GAWEAP`;
  - `COUNTRY`: the country of the house that gets it, such as `Americans` or `Russians`;
  - `X,Y`: the map cell;
  - `FACING`: 0–255 (0 north, 64 east);
  - `MISSION`: `Sleep` (never fires), `Guard`, `Area_Guard`, `Hunt`, or a number. Buildings ignore it.

  For a building, `X,Y` is the top-left cell. It goes on the nearest spot where the game's own placement
  check (`CanPlaceHere`) allows it, clear of trees, other buildings and the MCV.

  `yspawn.log` lists each vehicle and whether it could be placed. A map's own `[Units]` didn't work for this.
  Vehicles pre-placed for `<Player @ A>` or for a country name never showed up for the player in a match started
  this way; the cause wasn't investigated.
- **`python3 spawner/showcase.py [SPEED]`** (added 2026-09-26) starts Tsunami with a test line-up next to your
  start:
  - the Liberator beside the Grizzly, Prism Tank, Tank Destroyer and Tesla Tank, all facing the same way;
  - four Liberators at different facings;
  - one guarding Liberator that fires at three sleeping Apocalypse tanks.

  Speed defaults to 6 (slowest), so the bolts last long enough to catch.
- **`python3 spawner/showcase.py base [SPEED]`** (added 2026-09-26) starts you with a small Allied base that can
  build the Liberator right away, at `yspawn.ini`'s speed:
  - a Construction Yard, three Power Plants, an Ore Refinery, a War Factory and a Battle Lab;
  - three Liberators and 30000 credits;
  - your MCV is there too.

  Checked 2026-09-26: everything was placed and the match ran.
- To take screenshots remotely:
  - The display must be awake. Captures of a sleeping display are black, and KDE's idle dimming shows in them.
  - `kscreen-doctor --dpms on` wakes it, and `kde-inhibit --power --screenSaver sleep 600` keeps it on.
  - Then run `spectacle -b -n -f -o shot.png`. The game fills 2560×1440 starting at x 1280.

### Skirmish Setup window (2026-09-25)

`spawner/skirmish.py` is a GTK 4 / libadwaita window for setting up the match, then starting it through `spawn.py`.
It must run with `/usr/bin/python3`; the linuxbrew `python3` on PATH has no GTK bindings.
- **Start it:** `/usr/bin/python3 spawner/skirmish.py`, or **Skirmish Setup** in the application menu.
  `--install-desktop` writes the menu entry, `~/.local/share/applications/ra2yr-skirmish-setup.desktop`.
- **Maps:** a searchable list of archives in the game directory and its `Maps/` subfolders, with their in-game name, player count and
  preview picture. The names come from the game's string table and the pictures from the map's `[PreviewPack]`
  (decoded by `spawner/mappreview.py`). The start positions are drawn over the preview as numbered circles.
  A position someone has chosen is filled with that player's colour.
  Search by name (or archive filename) and use **Max players** to show maps with a specific capacity, from 2 to 8;
  **Any** shows all capacities. The filters work together, and **Random map** chooses from the matching maps
  that fit your current players. Loading a preset clears both filters so its map is visible.
- **Settings:**
  - your name;
  - a players table, with a row for you and one per opponent. Each row has country, colour, team (none or A–D) and
    start position (random or 1 to the map's player count), plus a level for opponents:
    **1** Easy, **2** Medium, **3** Brutal (stock), **4** Brutal with the strategy director (hover shows which).
    Level 4 writes `Director=1` to the AI's section and level 3 `Director=0`. Opponents saved as Hard
    before the levels existed open as level 4;
  - up to 7 opponents, but only as many as the map allows;
  - speed, starting base, credits, starting units and tech level;
  - the rule switches: bases, short game, superweapons, human in peace, crates, MCV repacks, build off ally, bridges.
- **Human in peace** (added 2026-09-29): under **Rules**, AI ignores your units and buildings regardless of
  teams. Target selection, firing, entry destinations, capture and mind control exclude the human. Your
  objects ignore damage, including collateral explosions, crushing, radiation and lightning; repairs still
  work. AIs keep fighting other hostile AIs, and your commands and superweapons still work. AI superweapon
  launches within 32 cells on either axis of a human object are suppressed to protect nearby bases from
  splash damage and storm scatter. Delayed Psychic Dominator capture also skips human objects. With only
  the human left as an opponent, AI superweapon targeting stops. Actual teams and alliances are unchanged.
  Team scripts that move to or attack an enemy structure (actions 46/47, finder at 0x6EEBD0) skip the
  human's buildings too (added 2026-10-01): before that, AI teams ferried terrorists and engineers into
  the human's base and left them standing there.
  The option defaults off, is saved with settings and presets, and writes `HumanInPeace=1` to the match INI.
  Install the updated DLL with `python3 spawner/spawn.py install` while the game is closed. All 35 engine
  guards must match the executable before any peace guard is installed; a mismatch refuses the match and
  reports the problem in `yspawn.log`.
  `spawner/test_human_peace.py` checks settings and executes the compiled 32-bit guards against the installed
  executable in Unicorn. Engine checks need `unicorn`, `pefile`, `capstone` and a DLL compiled without `-s`
  (use `-O2 -fno-inline` and output `spawner/yspawn-test.dll`, or set `PEACE_TEST_DLL` to its path).
  Run `python3 -m unittest discover -s spawner -p test_human_peace.py -v`. This is engine emulation;
  a full live match has not yet been verified with this option.
- **Reveal the whole map** (added 2026-10-02): under **Rules**, for watching AI matches. It writes
  `RevealMap=1`, and the DLL then reveals the map for the human player with `MapClass::Reveal`
  (0x577D90), the Spy Satellite's own call. That call is one-shot and Gap Generators shroud their
  area again, so it repeats every 150 frames. It is not tied to the benchmark and runs in every
  launch with the switch on.
  - It defaults to the Human in peace setting for settings saved before the switch existed.
  - It is better than giving the observer a Spy Satellite, radar and power plants. Those buildings
    would take room at the observer's start, and AIs could attack them; a Spy Satellite is also
    gap-shrouded.
  - The minimap shows the whole map once the observer has a radar. A tier 2 or 3 starting base
    includes one. With an MCV start the panel stays closed, but the main view is fully revealed.
  - Checked on Isolation with 7 AIs: the whole map and the minimap were visible.
- **Starting base** (added 2026-09-26): MCV, Tier 1, Tier 2 or Tier 3; see below. Anything but MCV turns the
  bases switch on and locks it.
- **Random:** Random country or colour is picked when you press Start. Random colours never repeat a colour
  that is already taken.
- **Game mode:** always Battle (`GameMode=1`). `mpmodesmd.ini` says it is the only mode that allows AI players.
  The list shows only maps made for it (`GameMode=standard` in the map's packet), which is currently all of them.
- **Start is disabled** while there is a problem:
  - too many players for the map;
  - two players with the same colour or the same start position;
  - everyone on the same team;
  - a starting base whose buildings the tech level would not allow (tier 1 needs 2, tier 2 needs 3, tier 3 needs 8);
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
  settings), and the window in dry-run mode. On 2026-09-26 its settings (`build_config`, then `spawn.prepare` and
  `spawn.launch`, the Start button's path) started an 8-player match with a starting base.

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
  - `0x501640` allies every computer player with every other one. It is called from several places, including
    `MakeAlly` (`0x4F9B70`), and returns at once only when `ScenarioClass+0x11E0` is set. `AssignHouses`
    sets that flag only when someone has a team. So in the stock game, with no teams, all the AIs are
    allied against you. The DLL makes `0x501640` return at once (added 2026-09-25), so team **None**
    means everyone for themselves. Teams still work as before, through `AllyTeams`.
  - The start list (`0x688380`) holds waypoints 0, 1, 2, … in order. All 53 maps define waypoints
    0 to max−1 with no gaps, so start k is always waypoint k.
  - The 1-based numbers in Skirmish Setup are the map's waypoints 0–7. On the preview, cell (x, y) is at
    x − y + W, (x + y − W) / 2, where W is the `[Map] Size` width. This matches the game's red markers to within a pixel.
  - **Not yet checked in game:** the build is installed but no match has been played with teams or fixed starts.
- The Cheat Defense has `SpySat=yes`, so building one reveals the whole map. That is intended and is not
  caused by the launcher.

### Starting base (2026-09-26)

Like Empire Wars in Age of Empires II, with RA2's tiers standing in for its ages. Every player, you and the AIs,
starts with the chosen tier's buildings already up, in place of the MCV:
- **Tier 1:** Construction Yard, power, barracks and ore refinery.
- **Tier 2:** tier 1 plus a war factory and radar.
- **Tier 3:** tier 2 plus a Battle Lab.

Each refinery brings its usual free miner. Yuri's also brings its slaves.

- **The buildings** come from `spawner/startbase.py`, which reads the modded `rulesmd.ini` from the game
  directory, or the stock one from `expandmd01.mix`.
  - The `[AI]` `Build*` lists give each role's candidates. A country gets the first one it may build (`Owner=`,
    `RequiredHouses=`, `ForbiddenHouses=`) that needs its own side's Construction Yard. So America gets
    `AMRADR`, the other Allies `GAAIRC`, and Yuri `NAPSIS`.
  - Prerequisites are followed through.
  - Power plants are added until `Power=` covers the drain: two for tier 3.
  - `python3 spawner/startbase.py` prints every country's list.
- **Skirmish Setup** writes the lists to a `[StartBase]` section of `yspawn.ini`, one line per country in play:
  `Americans=GACNST,GAPOWR,…`, plus `Remove=AMCV,SMCV,PCV`.
- **yspawn.dll**, once the scenario has loaded:
  - takes each house's MCV off the map;
  - puts the Construction Yard on the house's start cell (`HouseClass+0x5490`, BaseSpawnCell);
  - puts each other building on the nearest spot that `CanPlaceHere` accepts, with a free cell between buildings;
  - deletes the MCV, or puts it back if no Construction Yard fits.
- **For computer players** it also does what `UnitClass::TryToDeploy` does when an AI's MCV deploys in a skirmish
  (`0x739855`–`0x739926`):
  - builds the base plan (`0x505180`, `Base.Nodes` at `+0x5708`);
  - sets `Base.Center` (`+0x5750`);
  - sets `Production`, `AITriggersActive` and `AutoBaseBuilding` (`+0x1EE`, `+0x1F2`, `+0x1F3`);
  - calls `0x50C920`.

  Without this the AI never builds anything: the first try left both AIs mining for seven minutes, $100k in the
  bank. The starting buildings are also marked on the base plan (the node's cell and `Placed`, as the AI marks
  its own), so the AI does not build them again.
- **Checked in game (2026-09-26):**
  - **Lost Lake:** America (idle), a hard Russia and a hard Yuri, tier 3. All 24 buildings went down, and the free
    miners and slaves appeared. Both AIs expanded and built defences, armies and superweapons, and had overrun
    the idle base by game frame 15,800.
  - **Russian Roulette, 8 players, tier 3:** all 64 buildings placed. The sidebar was filled at once.
  - **An MCV-only baseline on Lost Lake** behaved as before.

## Mod: Magnetron carry (2026-09-26)

Yuri's Magnetron now keeps a grabbed vehicle in the air, and you choose where it goes down.
- **Grab:** attack a vehicle with the Magnetron as usual. The vehicle is lifted and pulled over next to the Magnetron.
  It then **stays hovering** there instead of dropping.
- **Carry:** order the Magnetron to move anywhere. The held vehicle follows it through the air and hovers in the
  nearest free cell beside it when the Magnetron stops.
- **Drop:** press **Stop** (`S`) with the Magnetron selected. The vehicle falls where it is and takes the stock falling
  damage (`FallingDamageMultiplier=1.0`, based on its current strength).
- **Drop somewhere else** (added 2026-09-26): attack the ground (`Ctrl`+click a cell) while holding a vehicle. The
  Magnetron does not fire. The vehicle flies to that cell, or the nearest free cell to it, and drops there with the
  same falling damage. If the cell is beyond weapon range (12 cells), the Magnetron first drives into range, as for any
  attack order. Its attack order ends when the throw starts: it stops attacking, and the vehicle carries on to the cell
  (whether a Magnetron still driving into range also stops is untested). Stop during the flight drops the vehicle at
  once. A new attack-ground order during the flight redirects it. A move order does not call it back.
- **Also drops it (stock):** the Magnetron dies, grabs a different vehicle, or anything else that ends the link
  in the stock game. That includes force-firing on a tree or another object that is not a cell: the vehicle falls
  where it is.
- Force-fire (`Ctrl`) on one of your own vehicles should let you move your own units (untested).
- **Only your Magnetrons carry.** Computer players' Magnetrons keep the stock pull-and-drop behaviour, so an AI
  never holds a vehicle forever.
- **Only the quick skirmish launcher has it.** It is code in `spawner/yspawn.dll` (section "Magnetron carry" in
  `yspawn.c`), so a normal Steam launch plays stock. No INI file is changed. To install or update it, run
  `python3 spawner/spawn.py install`.

How it works (addresses in this exe, traced 2026-09-26; YRpp names):
- `ImbueLocomotor` (`0x710000`) gives the victim a Jumpjet locomotor and links the two objects: `+0x2AC`
  `LocomotorTarget` on the Magnetron, `+0x2B0` `LocomotorSource` on the victim, and `+0x6AD` `IsAttackedByLocomotor`.
  It then sends the victim towards the Magnetron. `ReleaseLocomotor` (`0x70FEE0`) breaks the link; a victim in the air
  falls. The DLL changes three stock drop points so that a human player's Magnetron skips them:
  - `FootClass::SetDestination` (call at `0x4D9509`): the Magnetron was given a move destination;
  - `UnitClass::EnterIdleMode` (call at `0x7389BF`): the Magnetron went idle;
  - Jumpjet cruising state (`0x54C1B3`): the victim arrived. Instead of release + Descending, the locomotor is set to
    Hovering with `IsMoving` cleared, the state a hovering jumpjet sits in.
- **Stop** (`EventClass::Execute` at `0x4C7512`) calls `ReleaseLocomotor` on the unit.
- **Follow:** a wrapper on the `UnitClass::AI` vtable slot (`0x7F5CCC`) runs every 8 frames per Magnetron. When the
  Magnetron is more than 1.5 cells from where its victim is heading, it calls the victim locomotor's `Move_To` with the
  Magnetron's position. The locomotor itself picks the nearest free cell. Calling the victim's `SetDestination` would
  not work, because the game ignores it while `IsAttackedByLocomotor` is set.
- **Landing after a drop:** the Jumpjet `Process` (`0x54AEC0`) skips a locomotor that is Hovering and not moving
  (`0x54D0D0`), so after `ReleaseLocomotor` the fall would never run. The same per-frame wrapper therefore sets such a
  victim (hovering, no Magnetron attached) to Descending with `IsMoving` set. That is the state the stock arrival drop
  leaves, and the stock fall and landing code then runs. Every drop from the hover goes through this step.
- **Checked before patching:** at start the DLL compares the stock bytes at each site. It skips any site that differs
  and logs `magnetron carry: N of 7 patches applied` in `yspawn.log`.
- **Beam** (added 2026-09-26): the beam is a `WaveClass` (type 3). `Update_Wave` (`0x762AF0`) keeps it only while its
  owner's `Target` (`+0x2B4`) is the beam's target (check at `0x762B9B`), so a move order ended it while the vehicle was
  still held. The DLL also keeps it while a human player's Magnetron has that target as its `LocomotorTarget`
  (`+0x2AC`). The rest of the update for type 3 is only the beam animation. It therefore stays on the vehicle during
  an attack-ground throw too.
- **Attack-ground throw** (added 2026-09-26):
  - **Stock behaviour:** attack-ground is legal for the Magnetron. `CanFire` (`0x6FC0B0`) and `UnitClass::GetFireError`
    (`0x740FD0`) have no `IsLocomotor` rule for a cell, and `SelectWeapon` picks the primary `MagneticBeam`. When that
    bullet lands, it releases the old `LocomotorTarget` (`0x4695EB`), so stock drops the vehicle where it hovers.
  - **Why the link stays until arrival:** releasing first and then moving the vehicle does not work. Once released in
    the air, the Jumpjet `Process` (`0x54AF33`) resets a falling unit's destination to its own cell every frame, so it
    falls in place. The vehicle therefore flies there still linked. The stock arrival drop (`0x54C1B3`, release +
    Descending) then releases it over the cell.
  - **Starting the throw:** the DLL clears the Magnetron's `Target` and calls the vehicle locomotor's `Move_To` with the
    cell centre. `Move_To` (`0x54B1C0`) picks the nearest free cell with `MapClass::NearByLocation`.
  - **Throw record:** a small table records the vehicle as thrown. While it is recorded, the follow leaves it alone and
    the arrival hook takes the stock drop. A record counts only while the vehicle's `LocomotorSource` still points at
    its Magnetron. It is removed on arrival, on Stop, and as soon as either unit's link no longer matches it.
  - **When it starts:**
    - The per-frame wrapper throws as soon as the Magnetron's `Target` is a cell within the primary weapon's `Range`.
      Waiting for the attack mission would be worse: it fires only between `MinimumRange` (3) and `Range`, and from a
      cell inside `MinimumRange` it first drives off to full range (a stock bug).
    - A wrapper on the `UnitClass::Fire` vtable slot (`0x7F603C`, stock `0x741340`) also throws instead of firing at a
      cell. That covers stock range bonuses such as elevation, which the per-frame check does not add. The caller
      (`0x736F6D`) ignores `Fire`'s result.
- **Not yet checked in game.** Things to watch for:
  - the hover height and spacing;
  - the follow lag with the default 8-frame interval;
  - behaviour when several Magnetrons carry at once;
  - dropping onto an occupied cell or a building;
  - attack-ground: **checked in game 2026-09-26.** `Ctrl`+click on the ground while holding a vehicle threw it and
    dropped it on the clicked cell. The log showed one throw and one drop, with `7 of 7 patches applied`. Not yet
    tried: a cell within 3 cells, one beyond range, and Stop or a second click during the flight;
  - that the Magnetron goes back to guard after the throw;
  - throwing onto water or a cliff.
  - `yspawn.log` logs each throw (`magnetron: throw to cell X,Y, landing cell X,Y`) and each drop.

## Mod: Soviet Bulldozer (2026-09-29)

The shared installer now also adds **Bulldozer** (`SBDOZR`), a new Soviet tracked siege vehicle
with its own voxel model, armored cab, hydraulic push arms, broad steel blade and sidebar cameo.
All four Soviet countries can build it with a **War Factory and Radar** (`NAWEAP,NARADR`).
It costs **2000**, has **1800 HP**, heavy armor, speed **3**, and no turret.
It cannot appear in starting armies or crates. Soviet AI can build mixed assault teams of two bulldozers and four Rhinos
once it has Radar and a War Factory, on easy, medium and hard difficulties.

The blade is a ground-only, **1.5-cell** attack, based on the stock shovel's invisible projectile
and range-finding settings. Normal damage is 600 every 45 frames; elite damage is 900 every 35.
Its custom warhead applies **200%** to all infantry and building armor and **5%** to all vehicle
armor: nominal hits are **1200 versus infantry/buildings and 30 versus tanks** (elite: 1800/45),
before veterancy and other engine modifiers. It also crushes infantry by driving over them,
like stock tanks. The attack has no splash, projectile trail or anti-air capability.

- Definitions: `mod/bulldozer.py`; installer: `mod/build-tesla-mod.py` (keeps both earlier mods).
- Generate art: `mod/.venv/bin/python mod/make_bulldozer.py`.
- Install: `python3 mod/build-tesla-mod.py install` with the game closed.
- Validate: `mod/.venv/bin/python -W ignore::ResourceWarning -m unittest discover -s mod -p 'test_bulldozer.py' -v`.
- Art files: `sbdozr.vxl`, `sbdozr.hva`, `sbdzicon.shp`; previews: `mod/previews/bulldozer-*.png`.
- The existing `uninstall` command removes **all three mods**, including the Bulldozer.

Offline checks passed for stock-rule preservation, Soviet build restrictions, referenced assets,
CSF name, type registration, CRLF output, and damage calculations using actual stock target armor.
The generated voxel was decoded and compared to its source arrays; the cameo was decoded at 60×48.
Pre-install shared configuration is saved locally under `backups/before-bulldozer-*`.

In-game smoke check: the engine successfully placed three `SBDOZR` vehicles alongside a stock
Rhino, and the custom blade/cab model rendered at multiple facings. Evidence is in
`logs/bulldozer-validation/ingame.png` and `yspawn.log`. The test session was closed afterward.
Live combat damage, close-range pathfinding against large buildings, and sidebar production
still need gameplay verification; the damage figures above are checked from the generated rules.

Bulldozer cameo update (2026-09-29): replaced the procedural icon with a painted portrait
matching the stock Soviet vehicle cameos: low front three-quarter view, worn grey steel and
brick-red armor, dusty ground and a pale blue sky. Source art is `mod/cameo-art/bulldozer.png`;
the built-in image-generation prompt is saved in `mod/cameo-art/bulldozer-prompt.md`.
`make_bulldozer.py` now uses that portrait, downscales to 60×48, maps to `cameo.pal`, and adds
the existing stock frame and BULLDOZER label. The decoded SHP was inspected alongside five
stock cameos, and installed as `sbdzicon.shp`. Vehicle geometry and gameplay are unchanged.

Bulldozer AI update (2026-09-29): added one mixed team of two bulldozers and four Rhinos per Soviet house to `aimd.ini`,
with a Radar ownership trigger, tech level 5, and all three difficulties enabled. The trigger
uses Soviet side 2 and any Soviet country. It has weights 200/50/300 (initial/minimum/maximum).
The stock General Attack Buildings script gathers the mixed team and sends it against structures;
the team template is the stock Grizzly building-assault team, with neutral House and Max=1.
Existing stock AI and the Allied Liberator team are preserved. Five offline integration tests
pass, including AI registrations, side/difficulty restrictions, script/task-force references,
and preservation of every stock AI entry. Only `aimd.ini` was installed for this update.
AI production and assaults have not yet been observed in a live match; use a new skirmish.

The mixed composition replaces the earlier bulldozer-only team under the same AI IDs.
Rhinos and bulldozers are recruited into the same task force and share its assault script.
This is a shared team order, not custom per-unit targeting or an escort-distance guarantee.

## Brutal AI: oil derrick capture and defense (2026-09-29)

The mod installer now adds oil-focused teams for all three factions on **Brutal**
only. The stock oil teams and lower difficulties are preserved. The combat update
below additionally reroutes selected Brutal attack triggers.
Changes live in `mod/oil_ai.py` and are appended to `aimd.ini` by
`mod/build-tesla-mod.py`; a normal mod reinstall retains them.

- **Capture:** an additional engineer team with up to two active instances per
  AI house. Neutral oil now has initial trigger weight 900 and minimum 500, compared
  with the stock oil trigger's initial/maximum 70. A separate weight-180 trigger
  requests the same team when an enemy owns oil, sharing that two-team limit.
  Engineers target the nearest neutral/enemy derrick. They require only a barracks,
  avoid threats, and have no general attack fallback.
- **Hold:** owning at least one derrick requests one infantry guard team. Its
  script moves to the nearest friendly derrick, area-guards for 30 script time
  units, then repeats. Members cannot be recruited away by other AI teams.
- **Patrol:** owning oil also requests one vehicle team once a war factory can
  supply it. It alternates between the nearest and farthest friendly derricks,
  area-guarding at each stop. With one derrick, both destinations are the same.

| Faction | Infantry guard | Vehicle patrol |
|---|---|---|
| Allied | 3 GIs + 2 Guardian GIs | 2 Grizzlies + 2 IFVs |
| Soviet | 4 Conscripts + 2 Tesla Troopers | 2 Rhinos + 2 Flak Tracks |
| Yuri | 4 Initiates + 2 Brutes | 2 Lasher Tanks + 2 Gatling Tanks |

Capture and defense use separate teams: armed units in an engineer's action-46
capture script would fire on the derrick. Guards use action 58 to move to friendly
buildings, followed by the same area-guard/loop actions used in stock refinery
patrols. They are ordinary AI teams, so the stock base-defense quota does not
exclude them. Friendly destinations may include an ally's derricks; the production
condition specifically requires the AI house to own oil.

The vanilla trigger condition IDs and easy/normal/hard field order are verified
against [YRpp's engine definitions](https://github.com/Phobos-developers/YRpp/blob/master/GeneralDefinitions.h)
and [trigger serialization](https://github.com/Phobos-developers/YRpp/blob/master/AITriggerTypeClass.h).
Script building arguments use the zero-based BuildingTypes array index: stock
CAOILD is entry 72, index 71. Tests check this against the actual installed archives.

Validation: `python3 -m unittest discover -s mod -p 'test_*.py' -v` passes all 11
offline integration checks, including stock preservation, Brutal restrictions,
ownership conditions, faction buildability, registrations, script references and
CRLF output. Only `aimd.ini` was installed, with its previous copy backed up under
`backups/oil-ai-*/aimd.ini`. To roll back, close the game and restore that copy;
the installer source must also be reverted to keep a later reinstall from reapplying it.

Start a **new skirmish** to load the change. Live capture timing, defense response,
and balance have not been measured. Vanilla land pathfinding still limits access
to derricks on isolated islands; this update does not add engineer transports or
guarantee a separate garrison at every derrick.

### Defensive buildings at threatened oil (2026-09-29)

The **custom skirmish launcher** additionally lets Brutal AI fortify owned oil
derricks when armed enemies are within 12 cells. This is engine code in
`spawner/oil-defenses.h`, loaded by `yspawn.dll`; a normal Steam launch gets the
INI capture/guard teams above, but does not get this building-placement extension.

| Faction | First ground defense | Further ground defense, if buildable | Anti-air |
|---|---|---|---|
| Allied | Pillbox | Prism Tower | Patriot Missile |
| Soviet | Sentry Gun | Tesla Coil | Flak Cannon |
| Yuri | Gatling Cannon | Psychic Tower | Gatling Cannon |

- Requests require a working AI base with a Construction Yard and ore refinery.
  They apply only to Brutal computer players in skirmish, and only to their own
  derricks. Neutral, enemy and allied oil does not become their construction site.
- The AI selects the threatened derrick with the greatest nearby enemy pressure.
  It requests up to two ground defenses and an anti-air defense when aircraft
  approach. A Gatling Cannon counts for both roles. The cap is **three defensive
  buildings within six cells per derrick** for these requests, counting existing
  stock defenses too. Ordinary base construction can add further buildings.
- Existing construction orders finish first. Additional requests are spaced at
  least 450 game frames apart, require the normal prerequisites and enough power,
  and leave at least 1500 cash after the stock building cost. The ordinary factory
  produces the building with its normal cost and build time; no buildings are
  granted for free.
- Buildings go on valid terrain two to five cells from the oil, within the
  six-cell coverage radius, with a clear cell beside the derrick's foundation.
  Owned oil anchors this AI placement even outside the main base's build radius.
  If ownership changes or the space becomes blocked during production, the
  completed defense falls back to normal base placement.
- Destroyed defenses can be replaced while enemies remain nearby. Quiet derricks
  do not request new fortifications. The production and placement hooks check the
  expected stock 1.001 machine instructions before applying together.

Update or reinstall with `python3 spawner/spawn.py install` while the game is
closed. Start a new match from Skirmish Setup to load the updated DLL.
The previous DLL and game-directory test configuration are backed up in
`backups/oil-defenses-*/`; restoring that DLL with the game closed rolls back this
extension without removing the INI capture/guard teams.

Validation: `python3 -m unittest discover -s spawner -p 'test_*.py' -v` checks the
native decision code's difficulty exclusions, threat response, defense limits,
budget/power checks and distance boundaries, and verifies hook instruction
boundaries against the installed executable. The DLL compiles without warnings.
A controlled live match confirmed ordinary AI production and placement of both
ground defense types for all three factions; live object inspection confirmed
their ownership, positions and the game's AI-built flag. Evidence is saved in
`logs/oil-defenses-research/` (local, not committed).
Further live checks confirmed no extension requests for a quiet Brutal derrick
or threatened Normal/Easy derricks. A sleeping Kirov test fixture, explicitly set
airborne and given extra health so it survived the existing defenses, prompted
ordinary production and placement of a Flak Cannon beside the Soviet oil. The
test matches were closed and the previous game-directory launcher configuration
and log restored; only the updated DLL is retained from these tests.

## Multiplayer map pack (2026-09-29)

Installed 2,845 multiplayer maps from `Red Alert 2 maps pack 2024.rar`, skipping
campaigns, duplicate maps, missing terrain and invalid multiplayer starts.
The maps are new `.yro` archives under `<GAME>/Maps/2024/`; no stock game files,
mod files or global strings were replaced. In Skirmish Setup, search for `2024`.
Its Battle filter shows the 2,820 added maps that declare `standard` mode.

Keep large packs out of the game root. Initially placing all archives there
caused a black screen during startup: the main thread spent its time in Wine's
case-insensitive directory lookup before reaching the skirmish routine. Moving
only the new archives into `Maps/2024/` resolved this. The launcher now discovers
archives under `Maps/`, then extracts just the selected scenario into `yspawn.map`.
Old saved selections and presets resolve to the moved files automatically.
Close and reopen an already-running Skirmish Setup after this launcher update.
These subfolder maps are selected through Skirmish Setup, rather than Steam's
built-in skirmish menu.

Live checks reached gameplay on `admin` with the user's eight-player settings
and on `Acid Rain` with two players. Both test games were closed afterward,
and the user's game configuration was restored byte-for-byte. Original game
and mod file hashes remain unchanged. Five launcher regression checks passed:
`cd spawner && /usr/bin/python3 -W ignore::ResourceWarning -m unittest test_maps test_human_peace.SettingsTests -v`.
Local evidence is in `logs/maps-black-screen/`; the installation manifest is
`logs/maps-pack-2024/installed.json` (its filenames describe the original root
installation; the current location adds `Maps/2024/`). Some source maps have
unreadable previews, which the existing UI handles by showing no thumbnail.

## Larger Brutal armies, siege, navy and production (2026-09-29)

These changes apply to **Brutal skirmish AI**. Easy and Normal retain their
original trigger enable flags, compositions and team delays. Definitions live in
`mod/combat_ai.py`, applied by the existing mod builder. No Ares/Phobos installation
is needed. `spawner/combat-ai.h` adds the engine behavior to the custom launcher.

- **Assaults:** the usual armor and reinforcement teams now contain 11–18 units.
  Examples: 10 Grizzlies + 3 IFVs; 10 Rhinos + 3 Flak Tracks; 8 Lashers + 3 Gatling
  Tanks. Later teams add Prism Tanks, Magnetrons and Masterminds. Stock task forces
  remain intact; separate Brutal variants replace their hard-difficulty triggers.
  Brutal team-selection delay drops from 2000 to 900 frames.
  Armor teams target enemy vehicles first, then defenses and other buildings.
- **Siege:** Allied bombardment uses 4 Prism Tanks, 6 Grizzlies and 3 IFVs; Soviet
  bombardment uses 4 V3 launchers, 6 Rhinos and 3 Flak Tracks. This fixes the stock
  Soviet hard task force requesting unbuildable `V3ROCKET` missiles. Siege scripts
  target base defenses first, then factories and other buildings, without
  action 53's enemy-base gathering. Vanilla attack actions exhaust a target
  category before moving to the next, so later categories act as fallbacks.
  In the custom launcher, Prism Tanks, V3s, Carriers, Dreadnoughts and Boomers cancel
  forward movement once the engine considers their selected weapon in range,
  keeping their attack target. The Boomer selects its missile weapon for land
  targets. This prevents further advance; it does not add retreat/kiting logic.
- **Navy:** larger hunter fleets plus independent coastal bombardment teams:
  2 Carriers + 2 Destroyers + 2 Aegis Cruisers; 2 Dreadnoughts + 3 Sea Scorpions +
  2 Attack Subs; or 3 Boomers. These bombardment triggers require the owner's
  shipyard, without requiring an enemy shipyard. The launcher requests a first
  shipyard after a war factory and radar exist, only if native placement finds a
  valid coastal site and normal prerequisites, power and cash permit it. Brutal
  naval-hunter variants also wait for their own shipyard before forming.
- **Tech capture:** the existing two-team oil capture limit remains, with increased
  neutral-oil priority. All factions also get separate engineer teams for neutral
  airports, hospitals, outposts, machine shops and power plants. Each type has one
  team slot; scripts target the nearest building of the correct type. Engineers
  avoid threats; existing oil guards and fortifications remain.
- **Counters:** Brutal AI checks the current enemy's unit counts and requests
  one counter team per role when it sees a sizeable armor, aircraft or infantry
  force. Allied, Soviet and Yuri counters use their own buildable units. Armor
  counters hunt vehicles; infantry counters hunt infantry; anti-air teams guard
  the AI's base. Multiple enemy unit types can activate each role, but the shared
  team limit prevents duplicate counter teams. These triggers also work in
  ordinary Steam skirmishes.
- **Production and air strikes:** the custom launcher requests a second war
  factory when active teams are missing at least eight ground vehicles. It
  requests a second Allied airbase when four strike planes are already owned or
  the owned-plus-pending total exceeds one base's four docks. Requests begin
  after frame 1800, eight buildings and two refineries, with at least $6500 cash,
  $3000 left after the building cost and sufficient spare power. Normal
  construction pays for and places the building; requests stop at two. Four-aircraft building
  strikes require exactly one airbase; eight-aircraft strikes require at least two
  (eight docks) **owned by the AI**. Korea uses Black Eagles; Americans use their own airbase type.
  Existing aircraft may need to finish their teams before an eight-plane team fills.

All INI changes work in ordinary Steam skirmishes. The movement, coastal shipyard
and extra-factory/airbase hooks require the **custom Skirmish Setup launcher**.

To measure whether the larger teams actually assemble, set `TeamTelemetry=1` under
`[Settings]` in a custom-launcher `yspawn.ini`. The launcher writes
`yspawn-teams.csv` beside the game executable, sampling each Brutal AI house every
150 frames. Set `TeamTelemetry=2` to include smaller teams when investigating
production competition. Summary rows count teams and factories, shipyards and
airbases, and show the
current vehicle order; team rows show task-force composition as
`UNIT:present/wanted`, current script line (`-1` while no script is
active), and cash. The sampler only reads team state and is disabled by default.
Run `python3 spawner/team_report.py /path/to/yspawn-teams.csv` for per-team peaks
and first observed script activity. A team's disappearance from snapshots does not
by itself prove it launched: it may have been destroyed or canceled.

Two controlled fast skirmishes with tier-3 starting bases and two opposing Brutal AIs
showed that large teams are requested but vehicle production can lag. On Tsunami,
an eight-unit Soviet infantry team filled and reached script line 0; an Allied
eight-ship team stayed empty while its shipyard was still being requested. On
Country Swing, a 13-unit Soviet V3/Rhino/Flak Track team stayed at 0/13 for roughly
4,700 frames before disappearing, while stock infantry teams of 8, 12 and 15 filled
and executed. Allied Brutal teams reached only 6/13 and 6/17 before that match ended.
This indicates a further optimization opportunity in vehicle-production priority
and limiting competing team requests; the CSV does not establish which engine rule
caused the shortage.

Follow-up verbose telemetry showed many stock and new teams competing for one war
factory. Controlled 900-, 1500- and 2000-frame Brutal team intervals each filled
different teams; the longer intervals improved Soviet armor formation in those
matches but delayed Allied teams, so the installed 900-frame interval is retained.
The engine's AI trigger condition `1` means **enemy owns**, not **AI owns**. The
new naval bombardment and air-strike triggers now use condition `0`; Brutal
naval hunters and bombardment wait for the AI's shipyard. This avoids reserving
ships before a yard exists and selecting aircraft teams based on an opponent's
docks.

Reinstall both parts with the game closed:

```sh
python3 mod/build-tesla-mod.py install
python3 spawner/spawn.py install
```

Validation: 19 mod integration tests and 26 launcher tests pass. The engine
checks execute the compiled 32-bit unit-AI wrapper and building-production hook in
Unicorn, including range handling, difficulty exclusions, shipyard placement,
cash limits, demand-driven second-factory/airbase requests and the two-building cap. Existing
Magnetron, Human in peace, map and oil-defense checks also pass. For engine checks,
compile `spawner/yspawn-test.dll` with `-O2 -fno-inline` and without `-s`, and use a
Python environment with `unicorn`, `pefile`, `capstone` and system GTK bindings.

```sh
python3 -W ignore::ResourceWarning -m unittest discover -s mod -p 'test_*.py' -v
python3 -W ignore::ResourceWarning -m unittest discover -s spawner -p 'test_*.py' -v
```

The installed `rulesmd.ini`, `aimd.ini` and `yspawn.dll` are backed up under
`backups/combat-ai-*/`; restoring those three files with the game closed rolls
back this update. Revert the corresponding source edits before reinstalling to
keep it rolled back. Start a **new skirmish** to load the changes. Live attack
cohesion, capture timing, coastal access and balance have not yet been measured.
Land/water pathfinding and attack targeting still use the stock engine.

## Brutal strategy director (2026-09-30)

The custom launcher gives every **Brutal** computer player a strategy director
(`spawner/director.h`, decisions in `spawner/director-policy.h`). Easy/Normal AIs and the
ordinary Steam launch are unchanged. The INI teams from the sections above still run. The
director works alongside them and takes over what the stock AI does badly.

What it does, per Brutal house:

- **Production.** Stock AI builds units only to fill the task forces its triggers choose, so it
  sat on ~40k unspent credits with one idle factory. When the stock unit and infantry pickers leave
  the queue empty, the director orders a unit. It picks the role (main tank, anti-air, siege,
  anti-infantry) furthest below a mix taken from the enemy's current forces: more anti-air
  against aircraft, more siege against heavy defences. It adds war factories as cash piles up
  (up to 4), and queues the first war factory as soon as a refinery stands. It adds refineries
  when money runs short, but not while harvesters stand idle (ore gone or cut off). Liberators are
  capped at 4 and Masterminds at 3.
- **Army.** Team-less combat units form one army. Units are taken from attack teams once their
  script has started. Guard, oil and base-defence teams keep theirs.
  - The army gathers at a rally point on open land (never a bridge or inside the base).
  - It launches when it outvalues the target enemy's army plus half its defences.
  - It attacks the nearest structures with focus fire. Units answer anything already able to
    shoot them, and units well ahead of the group wait for the rest.
  - It retreats a losing attack, measured at the front. It defends against enemies near its
    buildings, with dwell times so it doesn't ping-pong through chokes.
- **Engineers.** It repairs the nearest broken bridge on its side, using the engine's own
  `MapClass::IsLinkedBridgeDestroyed`. It captures enemy tech buildings (derricks first) near
  its army or base once no armed enemy guards them. The army never shoots those buildings.
- **Water.** An attack that reaches no objective, with several objectives in a row going nowhere,
  marks the enemy as cut off. This covers an island map or a bridge that fell. The director then:
  - builds an amphibious transport;
  - drives the transport ashore at the rally point and loads up to 8 ground units;
  - drives it to the enemy base, unloads, and repeats;
  - switches main production to hover/air units: Robot Tanks (it queues a Robot Control Center
    for them), Kirovs and Siege Choppers, or Floating Discs.

  Units dropped nearer the enemy's base rejoin the army, and units at home still defend. The
  ground route is retried every 6000 frames, for example after engineers mend the bridge.
  - The engine's movement-zone labels were tried as a connectivity test and rejected. One
    landmass reads several labels.
- **Expansion.** With harvesters idle, it looks for rich ore 16–45 cells from home, away from its
  refineries and from enemies. It saves up for an MCV (plus a service depot if needed), drives it
  there and deploys it. The engine then re-centres the AI's base plan on the new yard, so new
  refineries follow the ore.

Added 2026-10-01, after watching a 7-AI free-for-all on *Don't Step on The Crocodile*:

- **Free-for-all launches.** The launch check used to add a third of *every* enemy's army to the
  target's. With six enemies that alone was ~55k, so no house ever attacked, and the late game
  stalled with every army at home. Now only third-party units within 25 cells of the target's base
  count. Between attacks the director re-picks its target every 3000 frames, by distance plus
  strength (army plus half of defences; one cell counts as 150 credits). It only switches when the new
  one scores 25% better. With two or more enemies alive, the edge it asks for drops from 1.2× to
  1.0× after 6000 frames without an attack, and to 0.8× after 12000.
- **Regroup.** During an attack, a unit at least 8 cells ahead of the marching body (the median unit),
  facing armed enemies worth more than twice our units around it (and over 1500), falls back to
  the body. That only happens when the group as a whole outvalues that enemy; otherwise the normal
  retreat applies.
- **Naval defence.** Enemy ships are no longer counted as a base raid, which sent the ground army
  into the base to stare at the water. While ships are within reach of our buildings (10 cells, or
  their weapon range), every armed ship we own attacks the nearest one it can hit. The yard builds
  warships until our fleet matches them: Destroyers, Typhoon subs or Sea Scorpions, Boomers. A land raid's
  defence point is now the raider nearest the base, not the average of all raiders.
- **Fleet offence.** With no enemy ships near the base, a fleet of three or more armed ships worth
  3000+, outvaluing the enemy ships around the target by 1.2×, goes on the attack:
  - **Targets:** the nearest enemy ship first, otherwise the target enemy's (then anyone's)
    structures within 6 cells of water the fleet can actually sail to. That water is a flood fill over
    water cells from a ship's own cell, so lakes and other seas are left out.
  - **Orders:** ships that can hit the target attack it; the rest (subs against buildings) escort
    and fight what they meet.
  - **Stalls and losses:** a target it can't get closer to within 1500 frames is skipped. At 40% of
    its strength at launch the fleet returns home.
  - **Tested:** six placed destroyers on *Crocodile* shelled one base's Tech Center and refineries
    (that house was eliminated), crossed to an Allied base and turned back at 2000 of 6000.
- **No more wiggling.** Units on a guard order were re-ordered every tick, about 900 times in one
  free-for-all. The check for "already guarding there" compared the unit's Focus with the goal, but
  the engine moves Focus while it guards. Each re-order restarted the unit, which shook in place
  between buildings. The director now leaves a guarding unit alone once it is within 4 cells of
  the goal, when its goal moved less than 4 cells, or when it got the same order in the last 450
  frames. Churn fell from 980 to 17 events per match.
- **Civilian buildings.** The civilian houses (`MultiplayPassive`, HouseType +0x1A6) are no
  longer enemies. The army used to shoot their garrisonable buildings as plain targets; now it
  leaves them alone, though their tech buildings stay capturable by engineers. Every 300 frames
  the director instead garrisons empty civilian buildings within 30 cells of home or the rally:
  - one soldier each (`Occupier=yes` infantry: GI, Conscript, Initiate and the like), at most two
    orders per pass and six buildings held;
  - never into a building that armed enemies are within 8 cells of;
  - the order is the enter click (`ClickedMission(Enter)` with the building as the destination),
    and the soldier stays out of the army until it is inside;
  - a building nobody got into is skipped for 9000 frames.

  Offsets read from the INI loaders: `CanBeOccupied` +0x157B, `MaxNumberOccupants` +0x1580,
  `Occupier` +0xEB4. The occupant count is BuildingClass +0x694. Tested on *Rockets Red Glare*:
  three buildings garrisoned by frame 11,856.
- **Air defence.** Enemy aircraft within 25 cells of our buildings or army front are tracked, with a
  slowly decaying memory. While they're worth 1500+ and outvalue our anti-air units, every side
  answers the same way:
  - anti-air vehicles override stock picks: IFV, Flak Track, Gatling Tank;
  - anti-air infantry too: Guardian GI, Flak Trooper;
  - an anti-air defence is queued at home every 3000 frames: Patriot, Flak Cannon, Gatling
    Cannon.

  It was added because Kirovs (10× value destroyed vs lost) and Floating Discs (3×) led the kill
  table against AIs that hardly built anti-air.
- **Bridges.**
  - **Route cut:** when an attack stalls with a fallen bridge within 25 cells of its front, the
    ground route counts as cut right away (hover/air and ferries), not after five stalled
    objectives.
  - **Left behind:** units that can't cross water are sent back to the rally, where ferries load.
  - **Never stop on a deck:** a unit on a bridge (CellClass flags +0x140 & 0x100) gets no hold,
    regroup or wait order.
  - **Walked off:** any of our units standing still on a deck for 600 frames, team members
    included, is walked to the nearest land along its own, perhaps broken, span.
  - **Stranded:** units stuck beyond a fallen bridge, far from home and the rally, guard where they
    stand instead of pathing at an impossible goal. Engineers mend the bridge nearest them first.
- **GIs deploy.** GIs and Guardian GIs in the army deploy behind sandbags when an armed ground
  enemy is within weapon range + 2, as a player's deploy click does (`ClickedMission(Unload)`,
  from `FootClass::ClickedAction` Self_Deploy at 0x4D75E1). While deployed they take no army
  orders. They pack up when nothing armed has been within range + 5 for 150 frames, at once when
  the army retreats, and never on a bridge. Deploy and pack-up are at least 300 frames apart.
  The deployed state is read from `InfantryClass::SequenceAnim` (+0x6C4, 27–30).
- **Allied infantry posts.** American (Allied) AIs keep GIs and Guardian GIs dug in at posts on
  the base edge facing the enemy, about 9 cells from the base centre toward the rally and spread
  across that line. There are two posts plus one per 8000 of the target enemy's army value, at
  most six; the centre posts fill first. Each soldier walks to its post and deploys there; a dead
  one is replaced from the army. Soldiers of posts no longer needed rejoin the army.
- **Full bunkers.** Our own garrisonable defences, such as the Soviet Battle Bunker, are filled to
  capacity with garrison infantry. The order gives the bunker as both target and destination; the
  destination alone, which works for civilian buildings, left them empty. With no garrison infantry
  free, the barracks trains some (GI, Conscript, Initiate).
  - **No friendly fire.** A soldier that reached a full bunker left the Enter mission but kept the
    bunker as its target, and shot at it. It also stayed out of the garrison order for good,
    because units with a target are skipped. Now every unit of ours that aims at one of our own
    garrisonable buildings drops the target and guards, unless it is still entering one with room
    left. Recent garrison orders are
    checked every tick, after a 90-frame grace period. All units are checked every 300 frames.
  - A soldier now counts as on the way only while it is alive, outside and still entering. The old
    600-frame window let slow walkers lapse, so extra soldiers were sent after them. A soldier
    stuck outside stops counting after 1,800 frames.
- **Islands from the start.** Every 600 frames a flood fill over land and bridge decks runs from
  our base. When no building of the target enemy stands on that land, the enemy is cut off by
  water at once. Before, it took five failed attacks (frame 18,912 in a 7-AI island game), and
  everything built until then was a land army. If another enemy can be reached on foot, the
  director fights that one instead. With no base of ours on the map yet, or no enemy buildings,
  the fill decides nothing.
- **Production while cut off.** Siege picks also go to hover/air units now, not only main picks.
  Ground vehicles are capped at 16 on our side of the water, counting both the director's own
  picks and stock team picks: enough to defend home and feed the ferry. Transports, harvesters,
  MCVs, ships, hover/air units and anti-air during a raid are always allowed. The cap is lifted
  while the base is under attack.
- **Ferry.**
  - It never takes the colonising transport. Both used to grab the same one and order it about,
    so it went nowhere: the "1 aboard" in its sailings was the colonising engineer. Nor does it
    take a transport with someone still aboard.
  - A transport recruited by a stock team is taken out of the team. Otherwise the team's area guard
    could hold it at the dock with a full load.
  - The loading clock starts at the dock, not on the way there. It sails when full, when nobody
    else is coming (after 300 frames), when nobody has boarded for 450 frames with 3 or more
    aboard (four tanks fill it by size) or for 900 frames otherwise, and after 3,000 frames at
    most. Soldiers walk over in a straggling line and board one by one.
  - Units still on their way in are released when it sails. They held it at the dock.
  - The landing spot from the engine's zone lookup is rejected when it lies on our own land. The
    fallback is the shore cell off our land nearest the enemy base. Before, a failed lookup kept
    the ferry at the dock for the whole game.
  - Each sailing logs how many units were left behind: still coming, more than 40 cells away, or
    busy.
  - **Tested** on Isolation, a 2–8 player island map, director against director. Every house
    knew it was cut off by frame 608. Ferries made 20 trips in 45,000 frames with 7 AIs; most
    carried 4–6, though some still left with one after 900 frames without boarders. Hills, Tower
    and Lostlake raised no false island.
- **Placing what the base planner can't.** On cramped islands the stock planner found no room for
  the Battle Lab (and at times a war factory, refinery or power plant). The building queue then
  waited on it for good, with no defences, factories or labs again and no Floating Discs, which
  need the lab. Yuri piled up 115k by frame 40,000. Where the planner finds nothing, the nearest
  spot to our base centre (2–20 cells) that the engine's own placement check accepts is taken.
- **Unbuildable stock picks.** A stock pick the house can't build (a construction yard, which only
  comes from an MCV) is dropped from the building or vehicle queue instead of holding it.
  - **Tested** on Isolation with 7 AIs: every side placed its Battle Lab. At frame 40,000 the Yuri
    AIs held 29.7k and 73 instead of 115k, and the houses that survived stood on 22–59 buildings
    instead of about 20. The bench log now shows each house's queues and whether they can be
    built every 1,500 frames.
- **Standing anti-air.** From frame 9000, every side keeps at least two anti-air defences at home
  (Patriot, Flak Cannon, Gatling Cannon) once it can spare the price plus 2000, besides the ones
  queued during air raids.
- **Mind control.** Targets with a mind-control capture manager (Yuri, Yuri Prime, Mastermind,
  Psychic Tower) score 80 more. Units controlled by one that is within weapon range + 6 score 60
  less: kill the controller and they switch back.
- **Dodging and kiting Kirovs.** A unit that can't shoot at a nearby Kirov or Floating Disc moves
  out from under it: toward our nearest anti-air within 20 cells, or 8 cells straight away. A unit
  that can shoot it kites. When the aircraft closes within 3 cells it steps back to just inside its
  own range (at least 5 cells), then fires again. Kirovs bomb straight down and are slower than
  the anti-air chasing them. Tested with three Kirovs placed by an American base: GIs and IFVs
  stepped out and shot all three down.
- **Order churn.** Each unit remembers the last cell it was sent to; a goal that drifts by under
  6 cells within 450 frames is the same order. A current target is swapped only for one scoring 30
  more, and engagement orders are at least 60 frames apart. Units no longer get restarted by small
  changes (churn per 6 Lostlake matches: from 350 events to under 30).
- **Stock picks vetoed:**
  - **MCVs:** at two construction yards (stock AI with MCV repacking parked extra yards side by
    side).
  - **Liberators:** beyond two (the mod's Allied AI teams order them in threes).
- **Allied tank order:** Mirage first, then Tank Destroyer, Grizzly, Liberator.
- **Ore outposts.** A captured tech building (an oil derrick, say) is our own building, so we may
  build beside it. Where rich ore lies within 10 cells of one, 15+ cells from our refineries and with
  no armed enemy within 12, the director builds a refinery beside it, on the side nearest the ore.
  Then a ground defence (Pillbox, Sentry Gun, Gatling Cannon) and an anti-air defence
  go beside the refinery.
  - **How:** the director's own placement step in the `FindBuildLocation` hook used by the oil
    defences.
  - **Limits:** one outpost at a time, checked every 3000 frames; a step that hasn't appeared after
    3000 frames ends it.
  - **Tested:** a derrick given to Korea on *Crocodile* got a refinery by the ore, pillboxes and a
    Patriot by frame 4,640.
- **Colonising islands.** A civilian tech building (oil derrick first) within 90 cells that ground
  units can't reach is taken by sea:
  - **How:** an engineer boards an amphibious transport (Amphibious Transport, Hover Transport,
    Yuri Hovercraft; one is built if needed), which drives over, unloads beside it and goes home
    while the engineer captures it.
  - **Fortifying:** the outpost logic then fortifies the holding; on an island without ore it builds
    the two defences only.
  - **Reachability:** a flood fill over land and bridge decks from the rally.
  - **Limits:** each step has 3000 frames; a target given up on is skipped later.
  - **Tested:** on Lostlake the Soviet AI shipped an engineer over at frame 6,960, held the island
    derrick by 7,952, and later fortified it with a Sentry Gun and a Flak Cannon.
- **Battle Fortresses manned.** Every American (Allied) Battle Fortress is kept full of GIs and
  Guardian GIs; the nearest idle ones within 25 cells board it, as onto a ferry.
- **Closed bridges.** Bridges that already read "destroyed" when the match starts (map-closed,
  barrier-gated ones no engineer can open) are no repair job and no sign of a cut route. A bridge
  that two engineers entered without effect is left alone too.
- **Engineer guard.** Every engineer the house owns, including stock-team ones, is checked every
  90 frames. One heading into the hut of a bridge that is whole, or that can't be mended, is
  called home. The director itself sent no engineer to a bridge in the regression suite; the ones
  seen entering barrier-gated bridges were stock AI engineers.
- **Tank Destroyers capped.** Tank Destroyers only hurt vehicles: at most a third of the house's
  armed vehicles, so a German AI without a Battle Lab builds Grizzlies alongside them instead of
  an army of Tank Destroyers.
- **Engineers called home.** When a bridge-repair job ends (mended by someone else, or timed out),
  an engineer still walking to the hut is sent home instead of entering a hut whose bridge is
  already whole.
- **Walled-in units.** A unit that hasn't moved for 3000 frames, with nothing in its sights and its goal
  far away, is checked with a flood fill of the cells within 14. When the fill can't leave that box
  past buildings, water or rock, the unit is in a pocket, and the director sells the cheapest
  building on the pocket's rim that it can spare: a wall, or a power plant while two others remain.
  Tested with a tank boxed in by eight power plants: it found the pocket and sold one.
- **Base jams.** Attack teams that were still recruiting held up to 18 units idle among the
  buildings, waiting for a unit type they never got. The director now takes those too, and marks
  every unit it takes as unrecruitable (`TechnoClass::RecruitableA`, which
  `FootClass::CanBeRecruited` returns), so no team pulls it back. The rally was also checked
  against the wrong house's buildings: it was picked before this house's scan. Base-defence and oil
  teams keep their units.
- **Refineries.** After three refinery orders in a row that never appear (nowhere to place them),
  it stops ordering refineries for 6000 frames.
- **Tank bunkers.** Yuri's base plan builds Tank Bunkers (`ThirdBaseDefenses=YAGGUN,YAPSYT,NATBNK`),
  but no AI ever drove a tank into one, so they stood empty. Between attacks the director sends the
  nearest main battle tank (Grizzly, Tank Destroyer, IFV, Rhino, Apocalypse, Flak Track, Lasher,
  Gatling) into each empty bunker, up to a third of the army. A tank in a bunker
  (`TechnoClass::BunkerLinkedItem`, +0x2E4) leaves the army pool. The order is what a player's
  click does (`FootClass::ClickedAction`, Action::Enter, 0x4D76F6): `ClickedMission(Enter)` with
  the bunker as the *destination*, not the target. A tank that doesn't get in within 900 frames goes
  back to the army and isn't picked again for 6000 frames.
- **Stranded harvesters.** Stock harvesters only look for ore near where they are. Once that runs
  out they sit at the refinery on guard, or stay in Harvest with nowhere to go. A harvester that
  hasn't moved for 900 frames, isn't docked or standing on ore, and has no destination is sent to the
  richest ore within 60 cells: ore value minus distance, away from enemy buildings. The order is a
  player's click on ore: `ClickedMission(Harvest, NULL, cell)`, from `FootClass::ClickedAction`
  at 0x4D7E8E. A field it still couldn't reach is skipped for 9000 frames, and a harvester that
  stays stuck gets the walled-in check below. Stranded harvesters also count as idle for the
  refinery and expansion decisions.
- **Production vetoes.** The stock teams' own picks are cancelled in two cases. A Kirov is cancelled
  once four are out, or while the base is being hit; Kirovs are aircraft, which the director's
  vehicle picker never buys. Infantry is cancelled when the enemy is cut off by water and the
  house already has 24, because they can only wait at home. In that mode the director itself stops
  at 16, its hover/air pick puts Siege Choppers before Kirovs, and while the base is under attack it
  builds ground units instead.
- **Stalled attacks.** An attack on a structure with no land route fails silently: the attack order
  is dropped and the army stays at home. That used to count as progress whenever any unit was
  shooting something, so a big army could sit at home "attacking" for the rest of the match. Now
  fighting only counts near the objective or at a front more than 20 cells from home.

### Strategy: plans and posture (2026-10-02)

Each director house now plays to a plan chosen for the map and setup, and bends it to the state
of the game (`DIR_F_STRATEGY`, flag 16384, on by default). Code: `dir_choose_plan`,
`dir_update_posture` and `dir_levers` in `director.h`; the numbers and rules are in
`director-policy.h`.

- **What it reads at the start:**
  - the walking distance to the nearest enemy base, a breadth-first search over land and bridge
    decks (straight-line distance lies on river maps), or "none" when every enemy is across water;
  - how many enemies there are;
  - the starting base: MCV, tier 1, 2 or 3;
  - whether our sea reaches an enemy: water within 15 cells of home whose flood fill comes within
    6 cells of an enemy building.
- **The draw:** each plan gets a weight from the setup, and the house draws one with its own random
  stream (the match seed mixed with the house index). The same setup can bring different plans,
  and a rerun with the same seed brings the same ones. `yspawn.log` has a `plan` line per house
  with the inputs and weights.

  | Plan | Fits | What changes |
  |---|---|---|
  | balanced | always (weight 30) | the director as it was |
  | rush | a short walk to the enemy (weight 35 under 70 cells, 15 under 110); a third as likely with 3+ enemies and again from a tier 2–3 base, never across water | launches at under half the usual army floor and a 1.0x edge, goes for refineries, factories and yards first, adds war factories at half the cash; ends with its first attack or at frame 14,000 |
  | boom | long walks, 3+ enemies, islands | an extra refinery, the refinery schedule 3,000 frames early, an expansion from frame 4,500 without waiting for idle harvesters, a first strike only at 1.6x the floor; hands over at frame 20,000 |
  | siege | tier 2–3 starts | 15 points more siege in the unit mix (V3, Prism Tank, Magnetron) |
  | naval | our sea reaches an enemy (weight 35 across water, 10 otherwise; half for the Allies) | warships kept at 40% of the army's value, the fleet sailing once it is worth 8,000. Escorts first: anti-air (Aegis, Sea Scorpion; Yuri has none) and anti-submarine (Destroyer, Typhoon, Boomer) ships each make up 20% of the fleet, 35% once enemy aircraft or submarines are about. The rest are capital ships: Carriers, Dreadnoughts, Boomers. A fleet of Dreadnoughts alone sat helpless under aircraft and submarines. |

- **Posture**, re-checked every 450 frames, kept for at least 900 frames unless home is threatened:
  - **press**: our army is at least 1.6x the target's army plus half its defences. Launches need
    an edge 0.2 smaller, and fresh units reinforce the front instead of waiting at the rally.
  - **hold**: armed enemy units within 35 cells of home are worth more than 1.5x our army (and over
    4,000). No launch while they are near, no expansion, and a ground defence at home every 2,400
    frames while money lasts: Prism Tower or Pillbox, Tesla Coil or Sentry Gun, Gatling Cannon.
  - **opportunity**: only with two or more enemies. The target's army is away from its base (under
    35% of it within 30 cells) and not near ours, so it is fighting someone else. The launch weighs
    what stayed at home, not its whole army. In duels this fired while the target's army was marching
    on us, and the strike went into a base while an army twice ours went into our own. The first
    A/B run lost 4 matches that way, so it is free-for-all only now.
- **Re-planning:** when a rush or boom runs out, and every 9,000 frames, the house draws again from
  the state of the game:
  - siege is weighted 40 against a target whose defences are over 4,000 and over half its army;
  - boom is weighted 30 while the strongest enemy has more refineries;
  - naval as at the start;
  - balanced 30;
  - never rush.

  A rush lasts 14,000 frames or until its first attack ends; a boom lasts 20,000 frames.
- **Watching:** with Reveal map on, each AI's plan, re-plans and posture changes show in the game's
  message list in its colour, for example "France (5): plan boom (enemies across the water, 6
  enemies)". The call is `MessageListClass::AddMessage` (0x5D3BA0, instance 0xA8BC60), with the
  arguments the engine passes at 0x4C6E9F; the house colour is `ColorSchemeIndex`, +0x16054.
  Without Reveal map nothing is shown, so a human playing for real doesn't see the AIs' plans.
- `[AIn] DirectorPlan=N` forces a plan for tests (0 balanced, 1 rush, 2 boom, 3 siege, 4 naval).
- **A/B results** (`bench.py suite … strat`, 48 matches on Arena, Hills, Tower and Lostlake,
  tier-3 bases, superweapons off, crates on; directors with the strategy layer against directors
  without it):
  - **First run:** 16 to 17. Opportunity strikes in duels cost 4 matches (see above), and the run
    was stopped at 33 matches.
  - **After that fix:** 28 to 20 for the strategy layer.
    - By plan: balanced (posture only) 14 of 21, siege 5 of 9, boom 4 of 7, rush 5 of 11.
    - 4 of the 5 lost rushes never launched. From tier-3 bases both armies grow alike, so rush is
      now a third as likely there.

### Naval yards no longer stall the war factories (2026-10-02)

Stock production keeps one vehicle order per house (`ProducingUnitTypeIndex`, +0x5650) for war
factories and naval yards alike. Each idle factory asks for it through `0x4FBD80` (called at
`0x45032D` in the factory AI). The engine then checks that the unit's `Naval` matches the factory.
The order is cleared only when the unit rolls out (`0x444119`). A ship in that order therefore
idled every war factory for the whole time the ship was being built. On Isolation, France's queue
held Carriers and Destroyers most of the game. It sat on about 40,000 credits for 20,000 frames
with an army worth 600–7,200.

Ships now have their own order for each director house:

- After the stock and director picks, a naval pick moves to the yard's order, and the war
  factories get a ground pick at once.
- The hook at `0x45032D` hands a naval yard that order first. The building type is still in EDX
  there, and the factory RTTI is `UnitType` (0x28), which `0x4FBD80` maps like `Unit` (1).
- An order that no yard takes within 3,000 frames is dropped.
- **Result,** 4 AIs on Isolation, frame 19,456: every house spent its money. Before, they held
  14k–50k. Before the RTTI fix, a first try built no ships at all.

### Convoys and landings (2026-10-02)

Cut off by water, the director used to ferry troops one transport at a time, 4–6 units per trip,
and they landed piecemeal. On Isolation, the armies grew to 50–80k at home with two launches in
20,000 frames.

- **Convoy:** up to four transports, one wanted per six ground units waiting at home. They load at
  the rally, sail together and unload on the same beach.
  - They sail when every docked transport is ready, none is still driving to the dock (wait up to
    900 frames) and nobody is still walking over to board (up to 1,800). They also sail when
    nobody else will come, or after 3,000 frames at the dock.
  - A transport is ready when full (6), when nobody has boarded for 450 frames with 3+ aboard, or
    after 900 frames.
  - Units are called nearest first, each to the docked transport with the most room it fits in.
    Room is by size: `Passengers=` (`TechnoTypeClass` +0x5E0) less what is aboard
    (`PassengerClass::GetTotalSize`, 0x473460), each unit taking its `Size=` (+0x380). A hover
    transport holds 12, so four tanks. Counting units instead called more than fit, and the rest
    crowded the dock, gave up and jammed the ramp to the beach. Yuri's slaves aren't called.
  - The rally stays on the base's level, turning 45 or 90 degrees aside if the way toward the
    enemy goes down a cliff. On islands it had been on the beach, on the dock itself.
- **Landed troops attack.** Troops already across used to be ordered to the home army's rally
  while it gathered, which they can't walk to. Now they attack the target's nearest structure.
- **Extra war factories on cramped islands:** the combat AI probes the stock base planner before
  queuing a factory. Where the planner finds no room, the probe now takes the director's own spot
  (`dir_fallback_spot`, the nearest one the engine accepts, 2–20 cells from the base centre). On
  Isolation, Yuri had one war factory all game with 40k unspent.

### Economy, defence and watching (2026-10-02, from the user's playtest notes)

- **Power ahead of need.** The combat AI adds war factories only with 50 power to spare, and stock
  plans build power late. Russia on MCV starts sat at one war factory with 25–30k unspent until
  frame 9,000: one Tesla Reactor gave 150 against a drain of 135. With under 100 spare and the build
  queue free, the director now queues a power plant (a Nuclear Reactor for the Soviets when they
  can build one). Russia then had 3 factories and 8k in hand at frame 9,000.
- **Refineries:**
  - The refinery rule waited until cash fell under 12,000, as a "money is short" test. Isolation
    starts with 50,000, so a house spent it all on units and stayed on one refinery, broke from
    frame 12,000. The cap no longer applies with a single refinery or during a boom.
  - With one refinery after frame 4,500 and under 2,500 in hand, director unit picks wait until a
    second refinery can be paid for.
- **Refineries go to the ore.** A refinery is placed on the spot nearest the best ore field within
  30 cells of home. Fields count their ore value minus 30 per cell of distance; a field one of our
  refineries already serves counts a third, and fields by enemy buildings are skipped. Where none
  fits, the stock base plan is used as before. (`dir_refinery_place`, in the `FindBuildLocation`
  hook.)
- **Yuri's Slave Miners move.** A deployed Slave Miner (`YAREFN`) that has stood 4,500 frames packs
  up when a field within 60 cells is at least 4× richer and worth 1,500. That field must be clear of
  enemies and of our other miners. Undeploying is the selling mission: `Mission_Selling` turns a
  building with `UndeploysInto` that isn't a construction yard back into its unit. The miner then
  gets a player's harvest click on the field and deploys there by itself. One move at a time.
  - **Tested** on Isolation: Yuri's miner at an ore value of 1,125 packed up and went to a field
    worth 10,600.
- **Fenced tech buildings.** Before an engineer captures a building, a flood fill checks whether it
  is fenced in. Walls and fences are overlays whose type has `Wall=yes` (`OverlayTypeClass` +0x2A8,
  types at 0xA83D80, the cell's overlay index at +0x44); they block movement by that flag, while the
  cell's land type stays clear. The walled-in unit check uses the same test now.
  - **Breakable fence** (the `Wall=yes` overlays: `CAFNCB`, `CAFNCW`, `CAFNCP`): the two nearest
    units that can open it are held out of the army for 600 frames and taken out of their team.
    - Units whose primary weapon's warhead has `Wall=yes` (`WarheadTypeClass` +0x144, via
      `WeaponTypeClass` +0xAC) shoot the panel nearest the engineer. Flak Tracks, Gattling Tanks
      and Magnetrons can't hurt walls and aren't sent; a soldier's secondary weapon doesn't count.
    - Units whose `MovementZone` (+0x5B4) paths through it are sent past it: the Crusher zones
      over a `Crushable` fence (+0x22D), the Destroyer zones (Rhino, Apocalypse, Lasher, Prism)
      through any fence they can shoot. A Normal-zone tank such as the Grizzly is refused a move
      into a fenced box, whatever its `Crusher=`.
  - **Sealed** by rock, water or other buildings: the target is given up at once. The `FENCE01–22`
    overlays are `Land=Rock` and can't be destroyed.
  - A box closed only by cliffs is "sealed" only when the whole-map land fill can't reach the
    building either. Plateau derricks whose ramp lies further out are not given up.
  - Benchmark logs list fenced and sealed tech buildings at frame 300. Rockets Red Glare has 12
    fenced derricks; the other tuning maps have none.
  - **Tested** on Rockets Red Glare: both houses opened derrick fences and captured the derricks
    (the first by frame 1,504).
- **Guards fall back.** While the base is defended and the raid outvalues the army at home by 1.2×
  (and is worth 3,000+), units of guard, oil and base-defence teams more than 15 cells out leave their
  teams and join the fight. They then stay team-less and join the army.
- **Infantry defence.** Two raids in five, a draw from the house's random stream, are met with a
  barracks flood while they last, up to 60 infantry:
  - anti-tank infantry against vehicles: Guardian GI, Tesla Trooper, Brute;
  - cheap infantry against infantry: GI, Conscript, Initiate.
- **Resource display.** With Reveal map on, every 1,500 frames each living AI gets one line in its
  colour, lasting until the next. For example: "Russia (2): $6.2k, 2 ref 4 harv, army $55.2k (82),
  def $5.5k, 20 bldg, balanced/normal". That is cash, refineries and harvesters, army value and
  unit count, defence value, buildings, plan and posture.
  - The message list holds 12 lines instead of 6 with Reveal map on. The `push 6` at 0x4A8BAF
    before `MessageListClass::Init` is patched; the engine's buffers hold 14.
- **Placement keeps passages and levels** (from the user's screenshots, Isolation):
  - A spot is rejected where the building would cut a passage, such as a ramp or a gap between
    cliffs: with its cells blocked, the open cells around it no longer all connect within 10 cells.
    Footprints come from `BuildingTypeClass::Foundation` (+0xEF0). It applies to the stock planner's
    spots for director houses, to the fallback spot and to refineries by the ore.
  - Base plan nodes that already have a cell skipped that check: the AI put the building straight on
    the node when `HouseClass` 0x50B760 accepted it (the call at 0x444FBA). That call is hooked, so a
    node cell that cuts a passage, squeezes a ramp or is off the base's level is refused, and the
    checked search runs instead.
  - Walls are checked too. The stock AI rings refineries with them, and a ring at a ramp's mouth
    squeezed the only way down to the beach to one cell. A refused wall piece moves at most 3 cells,
    or isn't built.
  - A building moved off a passage goes within 6 cells of the planner's spot first, then near the
    base centre. A tank bunker had been moved 26 cells away.
  - Factories, power plants, labs and radars stay on the base's level (within 104 leptons). A War
    Factory at the foot of the ramp below its base sent new tanks up the ramp against the army
    coming down. Shipyards are exempt from all of this. Starting bases keep to the start cell's
    level and out of passages too.
  - Fallback spots must be on the base's level and refinery spots on the ore's level, within one
    height level (104 leptons). The nearest free spot by distance had been down the cliff on the
    beach: harvesters had to go the long way round, and the refineries narrowed the ramp.
- **Convoys dock on a beach** (land type 6, water within 2 cells) near the rally that the army can
  walk to, re-picked every 3,000 frames. Docking at the rally wedged transports in among the waiting
  army on cliff-top bases. A transport that makes no headway for 900 frames loads where it stands
  when docking. At sea or homeward it gets its order again, and if it was sailing for over 1,800
  frames it unloads where it is.
- **Colonising transports come back.** One that couldn't unload by the island sat there for good
  with the engineer inside: convoys skip loaded transports. Now the unload order repeats every 300
  frames, and after 900 another landing cell is tried, up to 3. When the attempt is given up (also
  as soon as another house takes the target), the transport drives to the dock and unloads.
- **Bridge repairs:** an engineer that disappeared on its way to a hut was counted as having gone
  in, so two killed engineers marked a repairable bridge as unmendable for the rest of the match.
  The army then waited for a stock engineer to fix it. Now only an engineer last seen within 3 cells
  of the hut counts. One lost on the way is replaced, and none is sent while armed enemy units are
  within 7 cells of the hut. Repair log lines now carry frame numbers.
- **Submarines break off from aircraft.** A Typhoon or Boomer of a director house with enemy aircraft
  within 7 cells (worth 1,000+), and less friendly anti-air than that within 8 cells, drops its target
  and moves 9 cells away from the aircraft, turned 60°. Moving, it dives. For 450 frames it gets no
  fleet orders, then attacks again from the new angle. Surfaced to fire missiles, a Boomer had sat
  under Siege Choppers until it sank.
- **Air cover for Yuri's fleet.** Yuri has no anti-air ship, but the Floating Disc's laser hits
  aircraft (`AA=yes`). While enemy aircraft over Yuri's ships are worth 1,500+, up to three Discs leave
  the army to fly over the fleet; a house without one builds one first.
- **Construction yards flee.** With MCV repacks on, a director house's construction yard packs up
  and its MCV drives off to set up elsewhere, when either:
  - it is under 40% health and the armed enemy units within 10 cells outvalue ours there (and are worth
    2,000+);
  - or it is under 70% and they outvalue ours 3 to 1 plus 3,000. Packing up takes a while, and a
    yard at 40% under a big attack died in the middle of it.

  **Where to:** 20–35 cells off, in the engine's movement zone of the ground beside the yard, as far
  from armed enemies as can be (at least 15 cells). A yard boxed in by its own base, which no ground
  unit could leave, stays and fights.

  **How:**
  - Packing up is the selling mission with the yard's `Focus` set. `Mission_Selling` asks
    `0x50B730` at five places (0x449D29, 0x44A554, 0x44A802, 0x44A912, 0x44A99D) whether the owner
    may undeploy. In a multiplayer session that means "is human", so an AI yard would be sold
    instead; those calls also say yes for a director house's yard that was just told to flee.
  - `BuildingClass::Sell` does nothing unless +0x6E9 is set. Building setup sets it only when the
    type's build-up art is there (0x442CCF), so buildings the starting base puts down never had it,
    and could never be sold or packed up. The director now loads the art and sets the flag first,
    for every sale (`dir_sell`), the walled-in check's included.
  - The stock AI deploys a yardless MCV where it stands: `UnitClass::AI` puts it on Hunt (0x73645B,
    behind the 0x50B730 call at 0x736424), and Hunt deploys. While the house's MCV flees, that call
    says yes, and `UnitClass::TryToDeploy` (0x7393C0, hooked at its entry) refuses anywhere but the
    site. The move order is repeated whenever the MCV is on another mission.
  - **Tested** (bench switch `TestEscape=1`: AI house 1's yard flees at frame 600): the yard packed
    up at frame 640, the MCV drove 18 cells and the yard stood again by frame 2,080. Before the
    movement-zone check the chosen spot couldn't be driven to, and the MCV stayed put.
- **Magnetron victims left hanging** (all Magnetrons, stock AI ones included): a vehicle hovering or
  cruising in place on the jumpjet locomotor a Magnetron gave it, with no Magnetron attached, is
  set to come down. Types that fly by design are left alone (`JumpJet=` +0xD94, `BalloonHover=`
  +0xD6A on the type). The fix used to require the victim to still be flagged as attacked by a
  Magnetron, which the stock drops clear.
- **Bench:**
  - A crash report (`except.txt`) newer than the match ends it at once. Before, the hung game sat
    out the one-hour timeout.
  - So does a match with no benchmark row two minutes after launch, i.e. hung while loading.

### Settings

`[AIn]` keys in `yspawn.ini` (Skirmish Setup writes `Director` from the AI's level; 3 is 0, 4 is 1):

- `Director=0` turns the director off for that AI (stock Brutal).
- `DirectorPlan=N` forces a strategy plan: 0 balanced, 1 rush, 2 boom, 3 siege, 4 naval.
- `DirectorFlags=N` keeps only some features. Bits: 1 production, 2 army, 4 team takeover,
  8 focus fire, 16 economy, 32 answer fire, 64 defences-first objectives, 128 cohesion,
  256 engineers, 512 expansion, 1024 regroup, 2048 navy, 4096 walled-in units, 8192 tank bunkers,
  16384 strategy (plans and posture). `DirectorFlags=16319` is the director without the strategy layer.
  - The default is everything except 64. Defences-first lost 3 of 4 director-vs-director
    ablation matches.

### Benchmark (`spawner/bench.py`)

AI-vs-AI matches run unattended. An idle observer uses Human in peace, the game runs uncapped
(roughly 300–600 frames/s at speed 0), and the DLL exits when one side is left or at the frame limit.

- `bench.py run OUTDIR MAP AI... [--frames N] [--camera HOUSE] [--set KEY=VALUE]`: one match.
  - Each AI is `COUNTRY:START:DIRECTOR[:DIFFICULTY[:TEAM[:PLAN]]]`. `DIRECTOR` above 1 is a
    `DirectorFlags` mask (16319: no strategy layer), `PLAN` forces a plan.
  - `--camera` follows that house's army.
  - `--set` sets any `[Settings]` key, for example `--set StartBase=3 --set Superweapons=0`.
- `bench.py suite OUTDIR strat|strat_mcv|strat_iso`: the strategy A/B. Directors with plans play
  directors without them: America, Russia and Yuri paired on four maps, starts and the strategy
  side swapped (48 matches each), or 7-AI Isolation free-for-alls with alternate slots. Suites skip
  matches already played, so a stopped run resumes.
- `bench.py ab DIR...`: strategy on against off, per plan and per country. It reports wins and the
  mean placement: 0 for the winner, 1 for the first house out. Eliminated houses rank by when they
  fell, survivors by buildings and army value.
- `bench.py suite OUTDIR [tune|heldout|heldout2|hard]`: match sets, director vs stock Brutal.
- `bench.py summary DIR...`: one line per match, plus a win/draw/loss tally.
- `bench.py restore`: puts the user's `yspawn.ini/.log/.map` back after runs.

Bench-only `[Settings]` keys, for tests:
- `Camera=HOUSE`: what `--camera` sets. Benchmark matches always set `RevealMap=1` (see below).
- `ForceIsland=1`: treat enemies as cut off by water.
- `ForceExpand=1`: expand without waiting for idle harvesters.
- `Camera=100+N` follows house N's base; `CameraX`/`CameraY` watch one cell.
- `FleetTest=N` puts N warships for the first AI on the water nearest its base at frame 300.
- Every benchmark match also writes `yspawn-kills.csv`: per unit type, kills and the value destroyed
  (credited through `RegisterDestruction`, vtable 0xE0), deaths and the value lost.

First balance run (2026-10-01, director vs director, 24 matches, the Liberator already at speed 2,
750 HP and half fire rate):
- **Wins:** Yuri 13 of 16 (7–1 against Russia, 6–2 against America); America 7 of 16 (5–3 against
  Russia); Russia 4 of 16.
- **Value destroyed / value lost:** Floating Disc 9.1, Yuri Gatling Cannon 4.6, Apocalypse 3.7,
  Kirov 3.6, Mirage 1.8, Liberator 1.8, Initiate 1.7, Gatling Tank 1.4, Rhino 1.0, Grizzly 0.7,
  Lasher 0.7, Tesla Trooper 0.7, GI 0.3.
- These are AI-vs-AI numbers, so they mix unit strength with what the director builds for each side.
- Second run, under the user's settings (tier-3 bases, superweapons off), Liberator at 750 HP and ROF 70:
  - **Wins:** Yuri 10, America 6, Russia 6 of 16.
  - **Value destroyed / value lost:** Kirov 10.2, Floating Disc 2.9, Liberator 2.1 (also the top
    damage dealer, 592k), Rhino 0.9, Grizzly 0.4.
  - The Liberator was then halved again, to 375 HP and ROF 140/100, and the AIs got the air-defence
    answer above.
- Third run (all 24 matches): Yuri 10, Russia 9, America 3. Air defence cut the Kirov's ratio to 2.7
  and the Disc's to 1.1. America still lost: Grizzly 0.70, IFV 0.75, GI 0.29.
- Fourth run (all 24, with deploying GIs and Kirov kiting): Yuri 13, Russia 6, America 5. America
  against Russia is now even (5–3 to America); Yuri beat America 7–1 and Russia 6–2. The Kirov's
  ratio is down to 1.2. Yuri's Gatling Tank (1.65), Floating Disc (1.6), Lasher (1.25) and Gatling
  Cannon (7.2) lead.
- **Yuri made dearer** (user's call): Gatling Cannon 1000 → 1250, Gatling Tank 600 → 750, Floating
  Disc 1750 → 2000, with the refunds alike.
  - **Results** over Yuri's 16 matches: 8 wins instead of 13. Russia now leads Yuri 5–3, and
    America trails it 2–5 with 1 draw.
  - **Ratios:** the Disc fell to 0.54, the Gatling Tank to 1.47.
- Fifth run (all 24, after the Yuri price rise, infantry posts, full bunkers and standing anti-air):
  Yuri 9, America 6, Russia 5 of 16.
  - **Head to head:** America against Russia 3–3 with 2 draws, America against Yuri 4–4, Yuri
    against Russia 4–2 with 2 draws.
  - **Ratios:** Grizzly 1.11, Guardian GI 1.07.
- **Stat change:** the Grizzly got Rhino-level armour (`MTNK` Strength 300 → 400), listed in
  `BALANCE` in `mod/build-tesla-mod.py`. America's 16 matches afterwards: still 2 wins, though the
  Grizzly's ratio rose from 0.49 to 0.67. America's army is as large as the others (49k at frame
  20,000 against 45k and 41k), so it is losing trades, not economy.

Balance tools:
- `bench.py suite OUTDIR balance`: director against director for America, Russia and Yuri, every
  pairing on four 2-player maps with the starts swapped (24 matches).
- `bench.py factions DIR...`: wins per country and per pairing.
- `bench.py units DIR...`: kill statistics summed over matches, with value destroyed against
  value lost per unit type.

Each match writes `yspawn-bench.csv`: per-house snapshots every 300 frames and a result row.
- The snapshot columns `killed_units`/`killed_buildings` are the house's **losses**.
- Also recorded: cash, factories, current build orders and director state, and since 2026-10-02 the
  house's director flags, plan and posture.
- `yspawn.log` has the director's decisions.

### Results

All matches were AI vs AI, every AI on Brutal, 60,000-frame cap. Timeouts are scored on buildings
plus army value, and within 20% count as a draw.

| Set | Director record |
|---|---|
| Tuning suite, first version | 15/16 (1 timeout while far ahead) |
| Tuning suite, v4 / v8 / v9 builds | 16/16 / 14/16 / 15W 1D |
| **Tuning suite, final build** | **16/16** (15 eliminations; 1 timeout with 99k vs 46k army) |
| Held-out maps set 1 (v4 build, then used for tuning) | 11/13; director won all 3 FFAs with 4–6 AIs |
| Held-out maps set 2 (8 unused maps, Tier-2/3 starts), v8 build | 13W 1D: 12/12 duels; won the 6-AI FFA (1 director vs 5 stock) |
| **Held-out maps set 2, final build** | **10W 4L**: 10 of 12 duels; lost EB5 (economy ran dry by frame 9k); both big FFAs ended at the cap with the director behind |
| 1 director vs 2 allied stock Brutals | v4 build 1W 4L → v8 3W 2L → final 2W 1L, plus 2 where it eliminated one of the two before the cap |
| 4-AI FFAs, 2 directors vs 2 stock | won by a director every time (v4, v8, final) |

| 2026-10-01 build, tuning suite (3 runs) | 16/16, 12W 2L 2D, 12W 2L 2D; the 2026-09-30 build rerun alongside it also scored 12W 2L 2D |
| 2026-10-01, 7 directors FFA on *Don't Step on The Crocodile* (user's settings) | old build: no house ever launched an attack by frame 40k; new build: 40–54 attack launches per match, 5 of 7 houses eliminated in each of 5 full-length matches, and one match won outright at frame 48,640 |

- The tuning suite covers 8 stock maps and all three sides, with starts swapped.
- In the v1 baseline, stock Brutal kept 40–50k credits unspent all match. A director house spends
  its money by frame ~18,000.
- Live-verified: engineers repaired a fallen bridge (the log shows "bridge repaired"). An engineer
  captured an enemy derrick placed next to the director's base.
- Ferries and expansion were checked with the bench-only switches `ForceIsland=1` and
  `ForceExpand=1`:
  - A Soviet transport loaded tanks at the rally, landed them at the enemy base and made
    repeat trips.
  - An MCV was built, driven to an ore field 23 cells out and deployed.

  In the final held-out and hard runs, expansion also triggered naturally in 3 of 21 matches.
  The ferry has not.
- The director also runs in normal launches. With `Benchmark=0` and Human in peace off, the log
  shows `director=0x…` for the AI house, and the AI destroyed an idle human base.

### Known limits

- Match-to-match variance is large: the same set gave 13W 1D (v8) and 10W 4L (final), and the only
  post-v8 behaviours those matches triggered were expansions. The tuning suite alone ranges from
  12W 2L 2D to 16/16 for the same build.
- Walled-in detection was only tested on a pocket built for the test; no real match has triggered it yet.
- Ships defend only against enemy ships near the base; the fleet does not attack on its own.
- The stock AI sometimes sets an expansion MCV to Hunt, so the director keeps re-issuing the deploy.

- The water fallback relies on stall evidence, so a cut-off army first wastes some attack time.
- A bridge whose hut is across the water can't be repaired. An engineer that times out skips
  that hut for 9000 frames.
- Director-vs-director matches are dominated by start position on the tuning maps.

### Install and roll back

- Install with `python3 spawner/spawn.py install` while the game is closed.
- Tests: `spawner/test_director.py` (policy rules and hook prologues).
- The previous DLL and INIs are in `backups/director-20260930/`. Restoring `yspawn.dll` from there
  with the game closed removes the director.
