# Yuri's Revenge modding notes

This file records what we learned while building the Liberator tank and the Cheat Defense building
(2026-09-25). It covers facts about the game's files and engine, how to make art that fits in, and the
mistakes we made along the way.

`README.md` describes what the mods contain and how to install them, `docs/DEVLOG.md` in full detail. This file explains why they are built
the way they are.

Each fact is marked with how it was established:
- **[stock]:** read from the game's own files.
- **[measured]:** computed or checked offline.
- **[in game]:** seen in the running game, either by the user or on a screenshot of it.
- **[assumed]:** a reasoned guess that has not been verified.

---

## 1. Files and archives

- **Loose files override MIX archives [in game].** A file placed in the game directory replaces the copy in the
  archives, so no MIX file is ever edited. Only Yuri's Revenge reads the `*md` files (`rulesmd.ini`,
  `artmd.ini`, `aimd.ini`, `ra2md.csf`); base Red Alert 2 is unaffected.
- **MIX archives are nested and some headers are Blowfish-encrypted [stock].** `mixextract.py` reads them.
  Files are looked up by the CRC ID of their name, so a file you can't name can't be found. To find where
  a file lives, walk every nested MIX and compare IDs; don't guess the sub-archive.
- **Where the files we needed live [stock]:**

  | File | Archive path |
  |---|---|
  | `rulesmd.ini` | `expandmd01.mix` |
  | `artmd.ini`, `aimd.ini` | `ra2md.mix/localmd.mix` |
  | `ra2md.csf` | `langmd.mix` |
  | `unittem/sno/urb.pal`, `cameo.pal` | `ra2.mix/cache.mix` |
  | `unitdes/lun/ubn.pal` | `ra2md.mix/cachemd.mix` |
  | `voxels.vpl` (voxel lighting table) | `ra2md.mix/localmd.mix` |
  | Stock voxels (`gtgcantur.vxl`, `ttnk.vxl`, …) | `ra2.mix/local.mix` |
  | Stock cameos (`gcanicon.shp`, …) | `language.mix/cameo.mix` |
  | `gagcan.shp`, `natsla_a.shp` (snow theater) | `ra2.mix/snow.mix` |
  | `gggcan.shp`, `ngtsla_a.shp` (generic G) | a nested MIX inside `ra2.mix` |
  | Build-ups `g?gcanmk.shp` | the per-theater `iso*.mix` archives |

- **INI files use CRLF line endings and Latin-1 encoding [stock].** Keep both when patching. The installer builds
  every file from the stock copy each time, so it never patches an already-patched file.
- **Close the game before installing [assumed].** Files seem to be read at startup, so install only with the game closed.
  The installer refuses while `gamemd.exe` is running.

## 2. Rules and art settings

- **Theater-specific art (`NewTheater=yes`) [stock].** The engine replaces the second letter of the art
  name with the theater letter:

  | Theater | Letter |
  |---|---|
  | temperate | T |
  | snow | A |
  | urban | U |
  | desert | D |
  | lunar | L |
  | new urban | N |

  If that file doesn't exist, it falls back to the letter **G** (generic). Stock relies on this, for example
  with `gggcan.shp` and `ngtsla_a.shp`. The Cheat Defense ships the base and anim as `G` only, and ships
  the build-up once per letter plus `G`, which is the same way stock ships build-ups.
- **Name new art so it can't collide with stock [measured].** Our IDs are `GTCHDF`, `CHDF*` and `ATTNK`. Never ship
  a file named like a stock one (`gtgcan*`): it would silently reskin the stock unit too.
- **Voxel turret barrels are optional [stock].** Only the Grand Cannon has one, `gtgcanbarl.vxl`, named from the
  building ID plus `BARL`. The Patriot, Flak, Laser and Outpost turrets have none. Our turret is a single piece
  with `TurretRecoil=no`.
- **Turret position [measured, in game].** On the Grand Cannon's 232×168 canvas, the centre of the 2×2 foundation
  sits at pixel (116, 112), which is the canvas centre plus 28. That is exactly the stock `TurretAnimY=28`.
  With `TurretAnimX=0`, our turret sat exactly centred in both in-game screenshots, which the alignment fits
  confirmed. Stock uses 3, but its plate is not quite centred.
- **`ActiveAnim` alignment [stock, in game].** The anim uses the same canvas size as the building and lines up
  canvas to canvas. It is drawn with the building's palette, so house-colour indices 16–31 work
  (`natsla_a.shp` relies on this).
- **`LoopEnd` is inclusive [stock].** `[NATSLA_AD]` loops frames 10–19. A 16-frame loop therefore needs
  `LoopStart=0`, `LoopEnd=15`.
- **Fire position `PrimaryFireFLH` [assumed].** It is in leptons (256 per cell, which is about 6 leptons per voxel
  unit). Our values are estimates from the model; whether the bolt starts at the prong tips hasn't been
  checked in game.
- **Cloning a stock unit or building [measured].** `build-tesla-mod.py` copies the whole stock section, overrides
  some keys and drops others, then registers the new ID in the type list. Settings you don't list are
  inherited, so the Grand Cannon's barrel settings had to be dropped explicitly.
- **Engine lookups that failed [stock].** Stock `rulesmd.ini` has no voxel lighting settings; `Voxel` keys there are
  only about debris. No stock Yuri's Revenge building uses an SHP (non-voxel) turret, so that engine path is untested here.

## 3. Screen geometry

- **Scale [measured].** One voxel unit is 1 screen pixel horizontally, and a cell (256 leptons) is 42.43 units on a side.
  This was calibrated by overlaying the stock turret on the Grand Cannon build-up art.
- **Projection [measured].** The game uses a 2:1 dimetric view (elevation 30°, 45° yaw):

  ```
  sx = cx + (x - y) / √2
  sy = cy + (x + y) / (2√2) - z · cos30
  ```

  Here x and y are the world ground axes, z is up, and the origin is the foundation centre. A cell diamond is
  60×30 px.
- **Shadows fall to the upper right [stock].** This was read from the stock shadow frames. The pre-rendered art
  therefore uses a light from the lower left and above.
- **Building footprint [measured].** A 2×2 building spans x 59–172 (113 px wide) on the 232 px canvas. Our first
  base was only 82 px wide, so always compare bounding boxes against stock numerically.

## 4. File formats

- **SHP (TS/RA2) building frames [stock]:**
  - The frame list is the normal frame, the damaged frames, then one shadow frame for each of those in the
    second half.
  - Shadow pixels are index 1. Index 0 is transparent.
  - Frames are cropped to their bounding box and use compression 3. Each line is a u16 length followed by bytes,
    where zero runs are written as `0, count`.
  - Each frame header carries an average "radar" colour.
  - Westwood's own encoder over-counts the trailing zero run by 1. The decoder clips the run, so writing the exact
    width is also valid.
  - Every SHP we write is decoded again and compared (`check_shp`).
- **Build-up SHP [stock].** It has 12 frames plus 12 shadows. The last frame should match what the real building
  looks like, or there is a visible pop when the game swaps to it. Our last frames draw the turret through the
  voxel lighting emulation described in section 6.
- **VXL/HVA [stock].** A section scale of 1/12 is standard, and the section bounds set where voxels sit in world units.
  An identity HVA (one frame, one section) works. The normal table is mode 2 (TS, 36 normals; used by the Grand
  Cannon and Patriot) or mode 4 (RA2's larger table); FinalAlert2's `voxel_normal_tables.bin` has both.
- **Our normal convention is right [measured].** Normals computed from the stock voxels' shapes match their stored
  normals with about 0.75 correlation on every axis, using the matching table. Comparing against the wrong table
  gave about 0, which was a false alarm at first.

## 5. Palettes and colour

- **Buildings and units use the theater's unit palette [stock]** (`unittem`, `unitsno`, …). House colour is
  indices 16–31. **221 indices are identical in all six** unit palettes:
  - usable: 2–207 and 240–255, except 0 and 1;
  - avoid: 208–239, which are theater-specific;
  - special: 0 is transparent, 1 is shadow.

  If you stay on the shared indices, one file works in every theater.
- **Stock Allied building colours [measured].** Share of pixels across eight stock building SHPs by index range:

  | Range | Colour | Share |
  |---|---|---|
  | 48–63 | dark greys | 23% |
  | 16–31 | house colour | 15% |
  | 80–95 | blue-grey steel | 15% |
  | 112–127 | olive grime | 14% |
  | 64–79 | khaki concrete | 10% |
  | 32–47 | white | 7% |

  Every building also stands on a dirt-and-concrete ground pad.
- **Our first base looked "toy-like" [in game].** It used flat faces, bright lavender steel and neon cyan (index 9).
  The fix was:
  - the stock material mix and ramps;
  - low ambient light, a strong key light and ambient occlusion;
  - grime noise and chrome highlight bands;
  - a taller form and a concrete foundation;
  - small icy-blue lights instead of neon cyan.

  The user then called the base "perfect".
- **Stock Allied vehicle colours [stock, measured].** Measured on the Grizzly (`gtnk*`, the image of rules
  `[MTNK]`), Prism Tank (`sref*`), Tank Destroyer (`tnkd`) and IFV (`fv*`):
  - Steel 88/90/92/94 makes up 50–67% of the voxels, dark greys 55–59 about 30%, and house colour
    (mostly 23, with some 26/27) 5–20%. They have almost no white and no saturated colours; the Prism
    Tank's crystal is a few voxels of 15.
  - Tops are mostly 88, sides 92/94/55/59 and undersides 94/92. That up/side/down choice is the only
    painted-in shading.
  - The Tesla tank (`ttnk*`) draws its coils as grey discs (13, 49–51) with dark gaps (53–56), with no glow.
  - Drawn through `voxels.vpl` at 1 px per voxel over 12 facings, the average luminance is 69–77
    (standard deviation 30–35). The first Liberator measured 112 and looked toy-like next to them.
    The restyled one measures 80 (standard deviation 29).
  - Their voxels are hollow shells: interior voxels are not stored (the Grizzly hull is 41×30×12
    with 4508 voxels).
  - The unit IDs don't match the file names. Rules `[MTNK]` is the Grizzly, but it uses `Image=GTNK`. The
    `mtnk*` voxels belong to the Apocalypse (`[APOC]` has `Image=MTNK`).
- **Cameo [stock].** 60×48 in `cameo.pal`. The stock 2 px frame and the label strip style are reused; the label is
  hand-drawn in a 3×5 font.
- **Stock cameos are painted close-ups [stock].** They show the vehicle filling the frame at a low three-quarter
  angle, against blue sky and warm ground, with strong contrast. A clean render of the voxel model never looked like
  them.
  - **What worked [measured]:** an image-edit model repainted a 5:4 render of the model framed like a cameo
    (`fal_cameo.py`).
  - **How to judge it:** always at 60×48 next to stock cameos. A crop that looks good at full size can cut off
    the gun at cameo size.
  - **The prompt:** asking to keep "pose and framing" made 3 of 4 results stay voxel-like; one came out painted.

## 6. Voxel lighting (the important one)

The engine does not shade voxels with real 3D light. It uses **`voxels.vpl`**, a lookup table.

- **Format [stock].** A header of `remap_start, remap_end, n_sections, ?` (16, 31, 32), then a 768-byte palette,
  then `n_sections × 256` bytes. The output index is `vpl[light_level][voxel_colour]`.
- **Some colour indices go wrong through the table [in game].** Index 251 (pale blue in the palette) maps to
  204–207, which are **magenta**. Index 15 (white) maps to grey at low light. Safe icy-blue glow indices are
  192, 81 and 193. Always push a candidate colour through the table before using it
  (`vpl_draw` in `cheatdef_art.py`).
- **Light indices clip to their brightest value early [stock].** Index 86 reaches white by about level 20, and
  remap 19 reaches its brightest red by level 12. Dark indices such as 92–94, 23 and 55–57 cover a wide
  range, so they keep contrast.
- **Fitted light model [measured, in game].** `level ≈ 6.17 + n · (0.63, 6.14, -2.35)`, using world axes
  (+x screen lower-right, +y screen lower-left, z up).
  - **How it was fitted:** from two in-game screenshots of our own turrets at different facings. The base SHP was
    aligned to find the pixel offset, then the voxel was rasterised with the same projection. For each pixel, the
    table level whose output colour best matched the screenshot was found, and a linear model was fitted to those
    levels (error of about 5 out of 32 levels).
  - **What it means:** faces pointing screen lower-left are lit, and up-facing tops only reach about level 4, so
    they come out dim. On building turrets the engine uses roughly levels 0–13.
  - **The emulator reproduces screenshots well:** rendering the installed turret through the model closely
    matched the second screenshot. Recheck it whenever a new screenshot comes in.
- **How stock turrets get their depth [measured].** We analysed the Grand Cannon, Patriot, Flak, Laser and Outpost
  turret voxels:
  - They use flat colours with no painted-in shading. House colour is always 23, steel 92–94 and greys 55–57;
    within each ramp the spread is ±1.5 and doesn't correlate with height, facing or sky.
  - Their depth comes from large faces at distinct angles. The light level varies with a standard deviation of
    about 3.2 across the model; our round turret had 2.2 and looked flat.
- **What worked for our turret [measured].**
  - A tall angular housing with big faceted faces: a sloped front plate, side armour behind dark gaps and a
    stepped rear block.
  - Stock-dark base colours: remap 22, steel 91, grey 55.
  - Lighter indices on tops, to make up for the dim top light, and darker ones on undersides.
  - Only mild painted-in shading: sky visibility and edges.
- **Painted-in shading must be rotationally symmetric [measured].** The turret rotates but the light doesn't. So only
  bake terms that don't depend on facing: up/down orientation, sky visibility, crevices and edges. Never bake a
  directional light.

## 7. Workflow and tools

| File | Purpose |
|---|---|
| `mixextract.py` | Read-only MIX reader, with encrypted-header support. |
| `vxl.py` | VXL/HVA read/write and the normal table. |
| `csf.py` | String table (`ra2md.csf`) read/write. |
| `render.py` | Palette loading and SHP reading. Its `render` voxel preview is **mirrored in y** compared with our world axes, and it does **not** apply `voxels.vpl`, so don't use it to judge voxel colours. `draw_vpl` does apply `voxels.vpl` (with `VPL_LIGHT`) at any zoom, with a depth test; use it for voxel previews and for side-by-side checks against stock voxels. |
| `liberator_model.py` / `make_graphics.py` | The Liberator's voxel model and cameo. |
| `cheatdef_art.py` | Cheat Defense art. It ray-marches one signed-distance-function scene into the SHPs and cameo, and voxelises it into the turret. `vpl_draw` emulates in-game voxel drawing. It runs in about 30 s. |
| `build-tesla-mod.py install/uninstall/build DIR` | Builds the INI, CSF and asset set and installs or removes it. |

Working practices:
- **Verify offline before asking for an in-game test:**
  - compare bounding boxes against stock;
  - round-trip every file written;
  - check that each file named by the rules and art settings exists in the build output;
  - compare side by side with stock art at 1:1 on grass;
  - emulate voxels through `voxels.vpl`.
- **In-game screenshots are the ground truth.** Crop and zoom them. To measure, align the base SHP against the
  screenshot, which gives the offset, then compare or fit.
- **Drawing voxels larger than 1 px needs a depth test [measured].** Splatting each voxel as a k×k block in
  one pass per offset lets a farther voxel's later pass paint over a nearer one, which leaves stripes.
  Depth towards the viewer is `0.612·(x + y) + 0.5·z`. `cheatdef_art.vpl_draw` uses 2×2 passes and
  a different depth weighting. At 1 px its output differs from `render.draw_vpl` only in single-shade
  texture, so the Cheat Defense art is unaffected.
- **Keep a copy of every voxel that was shown in a screenshot.** The first fit needed the exact installed turret and
  had to be regenerated from a backup of the generator.

## 8. Still open

Checked on screenshots on 2026-09-26 (`spawner/showcase.py`):
- **The restyled Liberator matches the stock vehicles [in game].** It is closest in tone to the Prism Tank
  and a little lighter than the Grizzly and Tank Destroyer. Because it uses the stock indices, it matches
  even though `VPL_LIGHT` doesn't predict vehicles well. Faces towards the viewer come out brighter on
  vehicles than the building-turret fit says. Refit it from `showcase.py` screenshots if vehicle art ever
  needs tuning.
- **The Liberator's fire position is right [in game].** `PrimaryFireFLH=120,0,100` was aimed at the electrode
  at (20, 0, 16.5) voxels, using about 6 leptons per voxel, and the bolt starts at the gun tip. That fits
  the 6 leptons per voxel figure, though it's a single check.
- **Pre-placed map units didn't show up [in game].** Vehicles added to a map's `[Units]` (appended after
  `[Digest]`), owned by `<Player @ A>` or by a country name, never appeared for the player in a match started
  by `yspawn`. Nobody checked whether they existed under the shroud for some other house. Use the launcher's
  own `[Units]` (docs/DEVLOG.md, Quick skirmish launcher).
- **Evidence:** the full screenshots and the voxels they show are in
  `logs/liberator-ingame-2026-09-26/` (not in git).

Not yet confirmed in game:
- where the Cheat Defense bolt starts (`PrimaryFireFLH=270,0,225`);
- whether the glow animation draws over the turret (`ActiveAnimZAdjust=-30`);
- whether the build-up hands over to the turret smoothly;
- how the latest turret looks.

A third screenshot, from a different facing, would tighten the voxel light fit.
