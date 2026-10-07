#!/usr/bin/env python3
"""Unit duels: equal-cost groups of two unit types fight on open ground, and the value each side
destroyed says which counters which.

usage: arena.py run OUTDIR ATTACKERS OPPONENTS [--budget N] [--seeds N] [--jobs N] [--frames N] [--gap N]
                  [--shift DX] [--sides A|B|AB] [--defenders MISSION] [--start N] [--base-army PERCENT]
           --gap: cells between the groups; --shift moves the whole set-up east (+) or west (-) on the map;
           --sides A plays only the attackers on the west side; --defenders Hunt sends a base's army out
           (it guards the base by default); --start: the map start the fight is on (0 or 2: the AIs
           play from 0 and 1 with no units of their own, the observer from 3); --base-army: a base's
           army as a share of the budget (BASE_ARMY)
       arena.py matrix OUTDIR...            print the exchange matrix of finished duels

ATTACKERS and OPPONENTS are comma-separated type IDs (vehicles, infantry, aircraft) or the names of
groups below (allied, soviet, yuri, infantry). An opponent can also be a base from BASES (base_allied,
base_soviet, base_yuri, defended by its side's defences, and base_bare, with none): a siege, scored
the same way, so a unit that razes buildings gains from it. Each pairing is played with the sides
swapped and with --seeds seeds. A side gets round(budget / cost) units, at least one.

Each game is a benchmark match (bench.py) on MAP with yspawn.ini [Settings] Arena=1: the two
computer players lose their MCVs, so they own only their [Units] and never build, and ShortGame=0,
so the game ends when one side has nothing left (or at --frames). Both groups hunt.

Score of a duel, from yspawn-duels.csv: (share of the opponent's value destroyed) minus (share of
our own value lost), from -1 (wiped out, killed nothing) to +1. The matrix averages it over the
games of a pairing; > 0 means the row unit wins the trade against the column at equal cost.
"""
import csv, glob, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench, spawn

MAP = "Arena.mmx"        # 4 starts: the AIs at 0 and 1 (their MCVs are removed), the observer at 3,
ARENA_START = 2          # and the fight on the open ground of start 2
COUNTRIES = {"A": (0, "Americans"), "B": (8, "Russians")}   # yspawn.ini Country numbers, house IDs
GROUPS = {
    "allied": "MTNK,TNKD,SREF,FV,MGTK,ATTNK",
    "soviet": "HTNK,APOC,TTNK,HTK,V3,SBDOZR",
    "yuri": "LTNK,YTNK,MIND",
    "infantry": "E1,GGI,E2,SHK,INIT,BRUTE",
}
INFANTRY_CAP = 60
# Sieges: a small base of the opponent's, worth about what an army of the budget is. Its defences are
# powered, and its War Factory and Refinery stand behind them.
BASES = {
    "base_allied": "GAPOWR,GAPOWR,GAPOWR,GAWEAP,GAREFN,GAPILL,GAPILL,ATESLA,ATESLA",
    "base_soviet": "NAPOWR,NAPOWR,NAPOWR,NAWEAP,NAREFN,NALASR,NALASR,TESLA,TESLA",
    "base_yuri": "YAPOWR,YAPOWR,YAPOWR,YAWEAP,YAREFN,YAGGUN,YAGGUN,YAPSYT",
    "base_bare": "GAPOWR,NAPOWR,YAPOWR,GAWEAP,NAWEAP,YAREFN",
}
BASE_ARMY = 0.5   # a base's army is worth half the attackers' budget: the base's defences are the rest
DEFENCES = {"GAPILL", "ATESLA", "NALASR", "TESLA", "YAGGUN", "YAPSYT"}
BUILDINGS = {"YAPSYT", "NALASR", "GAPILL", "YAGGUN", "NATBNK", "GAPOWR", "NAPOWR", "YAPOWR", "GAWEAP", "NAWEAP",
             "YAWEAP", "TESLA", "NASAM", "NAFLAK", "GACNST", "NACNST", "YACNST", "GATECH", "NATECH", "YATECH"}


def rules():
    """Section -> keys of the installed rulesmd.ini (the mod's, with the stock game under it)."""
    secs, cur = {}, None
    for line in open(os.path.join(spawn.GAME, "rulesmd.ini"), encoding="latin-1"):
        line = line.split(";", 1)[0].strip()
        if line.startswith("[") and "]" in line:
            cur = secs.setdefault(line[1:line.index("]")], {})
        elif "=" in line and cur is not None:
            k, v = line.split("=", 1)
            cur[k.strip()] = v.strip()
    return secs


def expand(text):
    out = []
    for part in text.split(","):   # (a "+" spec stays whole)
        out += GROUPS[part].split(",") if part in GROUPS else [part]
    return list(dict.fromkeys(p for p in out if p))


def block(kind, n, cx, cy, toward):
    """n cells for a group around cx,cy, in columns of 8 running away from the enemy (toward +1: the
    enemy is east). Infantry share a cell three to one in the game, but one per cell keeps it simple."""
    cells = []
    for i in range(n):
        col, row = divmod(i, 8)
        cells.append((cx - toward * col, cy - 4 + row))
    return cells


def duel(a, b, cost, budget, side_a, gap, shift=0):
    """The [Units] lines for spec a (side side_a) against spec b (the other side). A spec is one type,
    a base, or several joined by "+", which share the budget equally (a base comes on top)."""
    sx, sy = showcase_start()
    sx += shift
    lines, start_value = [], {}
    for spec, side in ((a, side_a), (b, "B" if side_a == "A" else "A")):
        toward = +1 if side == "A" else -1
        parts = spec.split("+")
        based = any(t in BASES for t in parts)
        cx = sx - toward * (gap // 2 + (6 if based else 0))   # a base's army stands 5 cells in front of it
        facing = 64 if toward > 0 else 192
        country = COUNTRIES[side][1]
        armies = [t for t in parts if t not in BASES]
        value, built, cells = 0, 0, []
        for t in parts:
            if t in BASES:   # defences in front, facing the attackers, the rest behind them
                ids = BASES[t].split(",")
                lines += [f"{i},{country},{cx + toward * (2 if i in DEFENCES else -4)},{sy - 1},0,-" for i in ids]
                built += sum(cost[i] for i in ids)
                continue
            n = max(1, round(budget * (BASE_ARMY if based else 1) / len(armies) / cost[t]))
            if t in INFANTRY:
                n = min(n, INFANTRY_CAP)
            if t in BUILDINGS:
                lines.append(f"{t},{country},{cx},{sy - 1},0,-")
                built += cost[t]
                continue
            # a defended base's army waits in front of it; mixed groups are interleaved
            k = len(cells)
            cells = block(t, k + n, cx + toward * 5 if based else cx, sy, toward)
            lines += [f"{t},{country},{x},{y},{facing},{'Area_Guard' if based else 'Hunt'}" for x, y in cells[k:]]
            value += n * cost[t]
        start_value[side] = (value, built)
    return lines, start_value


_start = None


def showcase_start():
    global _start
    if _start is None:
        import showcase
        _start = showcase.start_cell(MAP, ARENA_START)
    return _start


def unplaced(dirs):
    """Duels with a unit the map had no room for (left out of the matrix)."""
    return [d for root in dirs for d in sorted(glob.glob(os.path.join(root, "*-vs-*")))
            if os.path.exists(os.path.join(d, "yspawn.log"))
            and "could not be placed" in open(os.path.join(d, "yspawn.log"), encoding="latin-1").read()]


INFANTRY = set()


def run(outdir, attackers, opponents, budget=9000, seeds=2, jobs=16, frames=6000, gap=12, shift=0, sides="AB",
        mission=None):
    r = rules()
    INFANTRY.update(t for t in r.get("InfantryTypes", {}).values())
    cost = {}
    types = {t for spec in attackers + opponents for t in spec.split("+")}
    for t in types | {i for o in types if o in BASES for i in BASES[o].split(",")}:
        if t in BASES:
            continue
        if t not in r:
            raise SystemExit(f"{t}: not in rulesmd.ini")
        cost[t] = int(r[t].get("Cost", "0") or 0) or 1
    todo = []
    for a in attackers:
        for b in opponents:
            if a == b:
                continue
            for side_a in sides:
                for s in range(seeds):
                    units, start_value = duel(a, b, cost, budget, side_a, gap, shift)
                    if mission:   # a base's army hunts too
                        units = [u.replace(",Area_Guard", f",{mission}") for u in units]
                    out = os.path.join(outdir, f"{a}-vs-{b}-{side_a}{s}")
                    if os.path.exists(os.path.join(out, "yspawn-bench.csv")):
                        continue
                    os.makedirs(out, exist_ok=True)
                    with open(os.path.join(out, "arena.txt"), "w") as f:
                        f.write(f"{a},{b},{side_a},{start_value['A'][0]},{start_value['A'][1]},"
                                f"{start_value['B'][0]},{start_value['B'][1]}\n")
                    extra = {"Units": units, "Arena": "1", "ShortGame": "0", "Credits": "0", "Superweapons": "0",
                             "Crates": "0", "HumanInPeace": "1"}
                    ais = [(COUNTRIES["A"][0], 0, 0, 0), (COUNTRIES["B"][0], 1, 0, 0)]
                    todo.append((out, MAP, ais, 3, extra, 1000 + s))
    print(f"{len(todo)} duels", flush=True)
    bench.JOBS = jobs
    try:
        bench.play(todo, frames)
    finally:
        if jobs <= 1:
            bench.restore()


_buildings = None


def result(d):
    """(a, b, score for a, frames) of a finished duel, or None. A side's loss: its units and buildings
    destroyed (yspawn-duels.csv), or for units its army's fall in value from the start to the end of
    the game (bench rows) if that is more, as it is when units are mind-controlled away."""
    global _buildings
    try:
        a, b, side_a, *v = open(os.path.join(d, "arena.txt")).read().strip().split(",")
    except OSError:
        return None
    rows = bench.bench_rows(os.path.join(d, "yspawn-bench.csv"))
    if not rows:
        return None
    try:   # a unit the map had no room for: not the duel that was meant
        if "could not be placed" in open(os.path.join(d, "yspawn.log"), encoding="latin-1").read():
            return None
    except OSError:
        return None
    if _buildings is None:
        _buildings = set(rules().get("BuildingTypes", {}).values())
    header, houses, res = rows
    house_side, army = {}, {}
    for r in houses:   # the last row of each house is from the end of the game
        rec = dict(zip(header, r))
        if rec["human"] == "0":
            side = "A" if rec["country"] == COUNTRIES["A"][1] else "B"
            house_side[rec["house"]] = side
            army[side] = int(rec["cost_infantry"]) + int(rec["cost_vehicles"]) + int(rec["cost_aircraft"])
    start = {"A": (int(v[0]), int(v[1])), "B": (int(v[2]), int(v[3]))}
    killed = {"A": 0, "B": 0}   # units destroyed
    lost = {"A": 0, "B": 0}     # buildings destroyed
    path = os.path.join(d, "yspawn-duels.csv")
    if os.path.exists(path):
        for r in csv.DictReader(open(path)):
            side = house_side.get(r["victim_house"])
            if side:
                (lost if r["victim_type"] in _buildings else killed)[side] += int(r["value"])
    # Units lost: those destroyed, or the fall in army value if that is more (units mind-controlled away).
    # The fall alone would miss what a base adds (a Refinery's free miner, survivors of a building).
    for k in start:
        lost[k] += max(killed[k], start[k][0] - army.get(k, 0))
    side_b = "B" if side_a == "A" else "A"
    share = {k: lost[k] / max(1, sum(start[k])) for k in start}
    score = max(-1, min(1, share[side_b] - share[side_a]))
    frame = int(res[1]) if res else int(houses[-1][0])
    return a, b, score, frame


def matrix(dirs):
    cells = {}
    for root in dirs:
        for d in sorted(glob.glob(os.path.join(root, "*-vs-*"))):
            r = result(d)
            if r:
                cells.setdefault((r[0], r[1]), []).append(r[2:])
    rows = list(dict.fromkeys(k[0] for k in cells))
    cols = list(dict.fromkeys(k[1] for k in cells))
    out = ["row unit's exchange against the column at equal cost (+1 wipes it out unharmed, -1 the reverse)",
           f"{'':14}" + "".join(f"{c:>{max(7, len(c) + 1)}}" for c in cols)]
    for a in rows:
        line = f"{a:14}"
        for b in cols:
            v, w = cells.get((a, b)), max(7, len(b) + 1)
            line += f"{sum(x[0] for x in v) / len(v):{w}.2f}" if v else " " * w
        out.append(line)
    return "\n".join(out)


def main(argv):
    if len(argv) >= 4 and argv[1] == "run":
        opts = {"--budget": 9000, "--seeds": 2, "--jobs": 16, "--frames": 6000, "--gap": 12, "--shift": 0,
                "--sides": "AB", "--defenders": "", "--start": ARENA_START, "--base-army": 50}
        args = argv[2:]
        for k in list(opts):
            if k in args:
                i = args.index(k)
                opts[k] = args[i + 1] if isinstance(opts[k], str) else int(args[i + 1])
                del args[i:i + 2]
        outdir, att, opp = args
        globals()["ARENA_START"] = opts["--start"]
        globals()["BASE_ARMY"] = opts["--base-army"] / 100
        run(outdir, expand(att), expand(opp), opts["--budget"], opts["--seeds"], opts["--jobs"], opts["--frames"],
            opts["--gap"], opts["--shift"], opts["--sides"], opts["--defenders"] or None)
        print(matrix([outdir]))
        bad = unplaced([outdir])
        if bad:
            print(f"{len(bad)} duels left out, a unit could not be placed")
    elif len(argv) >= 3 and argv[1] == "matrix":
        print(matrix(argv[2:]))
        bad = unplaced(argv[2:])
        if bad:
            print(f"{len(bad)} duels left out, a unit could not be placed: {', '.join(map(os.path.basename, bad[:5]))}")
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv)
