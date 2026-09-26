#!/usr/bin/env python3
"""Start a skirmish with test vehicles (and buildings) next to your start, to look at mod art in game.

usage: showcase.py [GAMESPEED]         line-up (0 fastest ... 6 slowest, default 6)
       showcase.py base [GAMESPEED]    a small Allied base that can build Liberators (default: yspawn.ini's speed)

Uses spawner/yspawn.ini with you at start 0 and AI1 at start 1, on MAP below, and adds a [Units]
section that yspawn.dll reads once the match has loaded (see yspawn.c, "test units").
- line-up: you in blue; the Liberator next to stock tanks, and one Liberator firing at sleeping enemy tanks.
- base: a Construction Yard, three Power Plants, an Ore Refinery, a War Factory and a Battle Lab (the
  Liberator's prerequisites), three Liberators and BASE_CREDITS. Buildings go on the nearest free spot.
Take screenshots with `spectacle -b -n -f -o FILE.png` (the display must be awake: `kscreen-doctor --dpms on`).
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spawn, mixextract

MAP = "Tsunami.mmx"
# (owner: "you" or "ai", vehicle, cells towards the screen bottom, cells to the right, facing 0-255, mission)
UNITS = [
    ("you", "MTNK", 3, -6, 112, "Sleep"),     # Grizzly; top row: all facing the same way
    ("you", "SREF", 3, -3, 112, "Sleep"),     # Prism Tank
    ("you", "ATTNK", 3, 0, 112, "Sleep"),
    ("you", "TNKD", 3, 3, 112, "Sleep"),      # Tank Destroyer
    ("you", "TTNK", 3, 6, 112, "Sleep"),      # Tesla Tank
    ("you", "ATTNK", 6, -7, 0, "Sleep"),      # middle row: four facings
    ("you", "ATTNK", 6, -3, 64, "Sleep"),
    ("you", "ATTNK", 6, 1, 128, "Sleep"),
    ("you", "ATTNK", 6, 5, 192, "Sleep"),
    ("you", "ATTNK", 9, -2, 64, "Guard"),     # bottom: fires at the Apocalypse tanks
    ("ai", "APOC", 8, 2, 192, "Sleep"),
    ("ai", "APOC", 9, 2, 192, "Sleep"),
    ("ai", "APOC", 10, 2, 192, "Sleep"),
]
BASE = [
    ("you", "GACNST", -5, -5, 0, "-"),       # buildings: the cell is the top-left corner
    ("you", "GAPOWR", -7, 0, 0, "-"),
    ("you", "GAPOWR", -7, 2, 0, "-"),
    ("you", "GAPOWR", -7, 4, 0, "-"),
    ("you", "GAREFN", 3, -8, 0, "-"),
    ("you", "GAWEAP", 2, 2, 0, "-"),
    ("you", "GATECH", -3, 6, 0, "-"),
    ("you", "ATTNK", 6, -2, 96, "Guard"),
    ("you", "ATTNK", 6, 0, 96, "Guard"),
    ("you", "ATTNK", 6, 2, 96, "Guard"),
]
BASE_CREDITS = "30000"
COUNTRIES = ["Americans", "Alliance", "French", "Germans", "British", "Africans", "Arabs", "Confederation",
             "Russians", "YuriCountry"]   # yspawn.ini Country= numbers, rulesmd.ini [Countries]


def start_cell(archive, start):
    info = spawn.map_info(archive)
    text = mixextract.extract(info["data"], info["map"].lower() + ".map").decode("latin-1")
    v = int(spawn._ini_sections(text)["Waypoints"][str(start)])
    return v % 1000, v // 1000


def main():
    if spawn.running():
        raise SystemExit("Yuri's Revenge is running; close it first.")
    if not spawn.installed():
        raise SystemExit("not installed; run: spawn.py install")
    args = sys.argv[1:]
    base = bool(args) and args[0] == "base"
    if base:
        args = args[1:]
    ini = spawn.read_config()
    me, ai = ini["Settings"], ini["AI1"]
    me.update(Map=MAP, Start="0")
    if base:
        me["Credits"] = BASE_CREDITS
    else:
        me.update(Color="2", GameSpeed="6")
    if args:
        me["GameSpeed"] = args[0]
    ai["Start"] = "1"
    owners = {"you": COUNTRIES[int(me["Country"])], "ai": COUNTRIES[int(ai["Country"])]}
    x0, y0 = start_cell(MAP, 0)
    # a cell step in x moves right and down on screen, a step in y left and down
    ini["Units"] = {str(i): f"{unit},{owners[who]},{x0 + k + d},{y0 - k + d},{facing},{mission}"
                    for i, (who, unit, d, k, facing, mission) in enumerate(BASE if base else UNITS)}
    spawn.prepare(ini)
    spawn.launch()
    print(f"started {MAP} with {len(ini['Units'])} test objects; yspawn.log in the game directory lists them")


if __name__ == "__main__":
    main()
