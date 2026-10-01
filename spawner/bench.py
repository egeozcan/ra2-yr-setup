#!/usr/bin/env python3
"""Run unattended AI-vs-AI skirmishes and collect yspawn-bench.csv results.

usage: bench.py run OUTDIR MAP AI1 AI2 [...] [--human-start N] [--frames N] [--speed N] [--seed N]
           each AI is COUNTRY[:START[:DIRECTOR[:DIFFICULTY[:TEAM]]]], e.g. 8:0:1 9:1:0 (DIRECTOR 1 = new logic,
           0 = stock Brutal, >1 = DirectorFlags bitmask)
       bench.py restore          put back the game directory's own yspawn.ini/.log/.map after runs
       bench.py summary DIR...   print one line per match directory
       bench.py suite OUTDIR [tune|heldout|hard|MAPFILTER]   director vs stock Brutal match sets

The idle human uses Human in peace, so it never takes part and never loses. The game directory's
yspawn.ini, yspawn.log and yspawn.map are saved first and restored afterwards.
"""
import configparser, csv, glob, os, shutil, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spawn

SAVED = ["yspawn.ini", "yspawn.log", "yspawn.map"]
BACKUP = os.path.expanduser("~/.cache/ra2-bench-saved")


def restore():
    """Put the user's launcher files back and forget the backup."""
    if not os.path.isdir(BACKUP):
        return
    for f in os.listdir(BACKUP):
        shutil.copy2(os.path.join(BACKUP, f), spawn.GAME)
    shutil.rmtree(BACKUP)


def write_ini(map_file, ais, human_start, frames, speed, seed, extra=None):
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    ini["Settings"] = dict(Map=map_file, Name="Observer", Country="4", Color="7", Start=str(human_start),
                           Team="-1", Credits="10000", GameSpeed=str(speed), UnitCount="0", TechLevel="10",
                           ShortGame="1", Superweapons="1", HumanInPeace="1", TeamTelemetry="0", Crates="0",
                           Bases="1", MCVRedeploy="1", BuildOffAlly="1", GameMode="1", Benchmark="1",
                           FrameLimit=str(frames), Seed=str(seed))
    if extra:
        units = extra.pop("Units", None)   # [Units] lines: TYPE,COUNTRY,X,Y,FACING,MISSION
        ini["Settings"].update(extra)
        if units:
            ini["Units"] = {str(i): u for i, u in enumerate(units, 1)}
    if extra and extra.get("StartBase"):   # 1-3: every player starts with that tier's buildings
        import startbase
        tier = int(ini["Settings"].pop("StartBase"))
        countries = dict.fromkeys(startbase.country_name(c) for c in [4] + [a[0] for a in ais])
        ini["StartBase"] = startbase.section(countries, tier)
    for i, ai in enumerate(ais, 1):
        country, start, director, difficulty = ai[:4]
        team = ai[4] if len(ai) > 4 else -1
        ini[f"AI{i}"] = dict(Country=str(country), Color=str(i - 1), Difficulty=str(difficulty),
                             Start=str(start), Team=str(team), Director=str(int(director != 0)))
        if director > 1:   # a DirectorFlags bitmask (director-policy.h DIR_F_*)
            ini[f"AI{i}"]["DirectorFlags"] = str(director)
    return ini


def parse_ai(text):
    parts = [int(p) for p in text.split(":")]
    parts += [-1, 1, 0, -1][len(parts) - 1:]
    return tuple(parts[:5])


def run(outdir, map_file, ais, human_start=-1, frames=40000, speed=0, seed=0, timeout=3600, extra=None):
    if spawn.running():
        raise SystemExit("game already running")
    os.makedirs(outdir, exist_ok=True)
    # One backup of the user's files for all runs, kept until `bench.py restore`: a killed run
    # cannot then overwrite it with benchmark settings.
    backup = BACKUP
    os.makedirs(backup, exist_ok=True)
    if not os.listdir(backup):
        for f in SAVED:
            p = os.path.join(spawn.GAME, f)
            if os.path.exists(p):
                shutil.copy2(p, backup)
    result = os.path.join(spawn.GAME, "yspawn-bench.csv")
    for stale in (result, os.path.join(spawn.GAME, "yspawn-kills.csv")):
        if os.path.exists(stale):
            os.remove(stale)
    try:
        ini = write_ini(map_file, ais, human_start, frames, speed, seed or int(time.time()) & 0x7FFFFFFF, extra)
        with open(os.path.join(outdir, "yspawn.ini"), "w") as f:
            ini.write(f)
        spawn.prepare(ini)
        began = time.time()
        spawn.launch()
        while not spawn.running() and time.time() - began < 60:
            time.sleep(1)
        while spawn.running() and time.time() - began < timeout:
            time.sleep(2)
        if spawn.running():
            subprocess.run(["pkill", "-x", "gamemd-spawn.ex"])
            time.sleep(3)
        for f in ("yspawn-bench.csv", "yspawn.log", "except.txt", "yspawn-teams.csv", "yspawn-kills.csv"):
            p = os.path.join(spawn.GAME, f)
            if os.path.exists(p) and os.path.getmtime(p) >= began - 1:
                shutil.move(p, os.path.join(outdir, f)) if f != "yspawn.log" else shutil.copy2(p, outdir)
        with open(os.path.join(outdir, "wall.txt"), "w") as f:
            f.write(f"{time.time() - began:.0f}\n")
    finally:
        for f in SAVED:
            p = os.path.join(backup, f)
            if os.path.exists(p):
                shutil.copy2(p, spawn.GAME)
    return summary(outdir)


def outcome(outdir):
    """'win' / 'loss' / 'draw' for the director house, or None. Timeouts go to the side with more
    buildings plus army value, and count as a draw within 20%."""
    path = os.path.join(outdir, "yspawn-bench.csv")
    if not os.path.exists(path):
        return None
    rows = list(csv.reader(open(path)))
    header, body = rows[0], rows[1:]
    result = next((r for r in body if r[0] == "result"), None)
    last = {}
    for r in body:
        if r[0] != "result":
            last[r[2]] = dict(zip(header, r))
    director = [h for h, d in last.items() if d["director"] == "1" and d["human"] == "0"]
    if not result or not director:
        return None
    if result[2] == "win":
        return "win" if result[3] in director else "loss"
    score = {h: int(d["buildings"]) * 1000 + int(d["cost_infantry"]) + int(d["cost_vehicles"])
             + int(d["cost_aircraft"]) for h, d in last.items() if d["human"] == "0"}
    mine = sum(score[h] for h in director)
    theirs = sum(v for h, v in score.items() if h not in director)
    if abs(mine - theirs) * 5 < max(mine, theirs):
        return "draw"
    return "win" if mine > theirs else "loss"


def faction_result(outdir):
    """(countries in the match, winner country or 'draw') scored like outcome(), per country."""
    path = os.path.join(outdir, "yspawn-bench.csv")
    if not os.path.exists(path):
        return None
    rows = list(csv.reader(open(path)))
    header, body = rows[0], rows[1:]
    result = next((r for r in body if r[0] == "result"), None)
    last = {}
    for r in body:
        if r[0] != "result":
            last[r[2]] = dict(zip(header, r))
    ais = {h: d for h, d in last.items() if d["human"] == "0"}
    countries = sorted(d["country"] for d in ais.values())
    if result and result[2] == "win":
        return countries, ais[result[3]]["country"]
    score = {h: int(d["buildings"]) * 1000 + int(d["cost_infantry"]) + int(d["cost_vehicles"])
             + int(d["cost_aircraft"]) for h, d in ais.items()}
    ranked = sorted(score, key=score.get, reverse=True)
    if len(ranked) < 2 or (score[ranked[0]] - score[ranked[1]]) * 5 < score[ranked[0]]:
        return countries, "draw"
    return countries, ais[ranked[0]]["country"]


def factions(dirs):
    """Wins per country, and per pairing."""
    wins, played, pairs = {}, {}, {}
    for d in dirs:
        r = faction_result(d)
        if not r:
            continue
        countries, winner = r
        for c in countries:
            played[c] = played.get(c, 0) + 1
        if winner != "draw":
            wins[winner] = wins.get(winner, 0) + 1
        key = " vs ".join(countries)
        pairs.setdefault(key, []).append(winner)
    lines = [f"{c}: {wins.get(c, 0)} wins of {n}" for c, n in sorted(played.items())]
    lines += [f"{k}: " + ", ".join(v) for k, v in sorted(pairs.items())]
    return "\n".join(lines)


def units(dirs, top=40):
    """Kill statistics summed over matches: per unit type, value destroyed against value lost."""
    stats = {}
    for d in dirs:
        path = os.path.join(d, "yspawn-kills.csv")
        if not os.path.exists(path):
            continue
        for r in csv.DictReader(open(path)):
            s = stats.setdefault(r["type"], {"cost": int(r["cost"]), "kills": 0, "killed": 0, "deaths": 0, "lost": 0})
            s["kills"] += int(r["kills"])
            s["killed"] += int(r["killed_value"])
            s["deaths"] += int(r["deaths"])
            s["lost"] += int(r["lost_value"])
    rows = sorted(stats.items(), key=lambda kv: -kv[1]["killed"])[:top]
    out = [f"{'type':10} {'cost':>5} {'kills':>6} {'destroyed':>10} {'deaths':>6} {'lost':>9} {'ratio':>6}"]
    for t, s in rows:
        ratio = s["killed"] / s["lost"] if s["lost"] else float("inf")
        out.append(f"{t:10} {s['cost']:5} {s['kills']:6} {s['killed']:10} {s['deaths']:6} {s['lost']:9} {ratio:6.2f}")
    return "\n".join(out)


def tally(dirs):
    counts = {}
    for d in dirs:
        o = outcome(d)
        counts[o] = counts.get(o, 0) + 1
    return counts


def summary(outdir):
    path = os.path.join(outdir, "yspawn-bench.csv")
    if not os.path.exists(path):
        return f"{outdir}: no result"
    rows = list(csv.reader(open(path)))
    header, body = rows[0], rows[1:]
    result = next((r for r in body if r[0] == "result"), None)
    last = {}
    for r in body:
        if r[0] != "result":
            last[r[2]] = dict(zip(header, r))
    parts = []
    for h in sorted(last, key=int):
        d = last[h]
        if d["human"] == "1":
            continue
        value = int(d["cost_infantry"]) + int(d["cost_vehicles"]) + int(d["cost_aircraft"])
        parts.append(f"h{h}:{d['country']}{'*' if d['director'] == '1' else ''}"
                     f"{' DEAD' if d['defeated'] == '1' else ''} b{d['buildings']} army${value}"
                     f" k{d['killed_units']}/{d['killed_buildings']}")
    frame = result[1] if result else body[-1][0]
    outcome = f"{result[2]} h{result[3]}" if result else "unfinished"
    ms = last and max(int(d["ms"]) for d in last.values())
    return f"{os.path.basename(outdir)}: {outcome} @{frame} ({ms / 1000:.0f}s)  " + "  ".join(parts)


# (map, human start, director country, baseline country, director start, baseline start)
SUITE = [
    ("Lostlake.mmx", 3, 8, 8, 0, 1), ("Lostlake.mmx", 3, 8, 8, 1, 0),
    ("Lostlake.mmx", 3, 0, 8, 0, 1), ("Lostlake.mmx", 3, 8, 0, 1, 0),
    ("EB4.mmx", 3, 9, 0, 0, 1), ("EB4.mmx", 3, 0, 9, 1, 0),
    ("Arena.mmx", 3, 0, 0, 0, 1), ("Arena.mmx", 3, 0, 0, 1, 0),
    ("Hills.mmx", 3, 8, 9, 0, 1), ("Hills.mmx", 3, 9, 8, 1, 0),
    ("Tower.mmx", 3, 2, 5, 0, 1), ("Tower.mmx", 3, 5, 2, 1, 0),
    ("Oceansid.mmx", 3, 3, 8, 0, 1), ("Oceansid.mmx", 3, 8, 3, 1, 0),
    ("Break.mmx", 2, 9, 9, 0, 1), ("Break.mmx", 2, 9, 9, 1, 0),
]


# Held out from tuning: other maps, starting bases, more players. (map, human start, AIs, extra)
# AIs are (country, start, director, difficulty[, team]).
T3 = {"StartBase": "3"}
HELDOUT = [
    ("DeepFrze.yro", 3, [(8, 0, 1, 0), (0, 1, 0, 0)], None),
    ("DeepFrze.yro", 3, [(0, 0, 0, 0), (8, 1, 1, 0)], None),
    ("Pacific.mmx", 3, [(9, 0, 1, 0), (2, 1, 0, 0)], None),
    ("Pacific.mmx", 3, [(9, 0, 0, 0), (2, 1, 1, 0)], None),
    ("Rockets.mmx", 3, [(1, 0, 1, 0), (6, 1, 0, 0)], T3),
    ("Rockets.mmx", 3, [(1, 0, 0, 0), (6, 1, 1, 0)], T3),
    ("Valley.mmx", 3, [(3, 0, 1, 0), (9, 1, 0, 0)], T3),
    ("Valley.mmx", 3, [(3, 0, 0, 0), (9, 1, 1, 0)], T3),
    ("Kaliforn.mmx", 5, [(8, 0, 1, 0), (0, 1, 0, 0), (9, 2, 1, 0), (5, 3, 0, 0)], None),
    ("GoldSt.mmx", 5, [(0, 0, 0, 0), (8, 1, 1, 0), (5, 2, 0, 0), (9, 3, 1, 0)], T3),
    ("Death.mmx", 7, [(8, 0, 1, 0), (0, 1, 0, 0), (9, 2, 1, 0), (2, 3, 0, 0), (7, 4, 1, 0), (1, 5, 0, 0)], None),
    ("Maps/2024/2024 - 4Waterway.yro", 3, [(0, 0, 1, 0), (8, 1, 0, 0)], None),
    ("Maps/2024/2024 - 4Waterway.yro", 3, [(0, 0, 0, 0), (8, 1, 1, 0)], None),
]
# One director against two stock Brutals on the same team, and 2+2 free-for-alls.
HARD = [
    ("Lostlake.mmx", 3, [(8, 0, 1, 0, -1), (0, 1, 0, 0, 1), (9, 2, 0, 0, 1)], None),
    ("Arena.mmx", 3, [(0, 0, 1, 0, -1), (8, 1, 0, 0, 1), (8, 2, 0, 0, 1)], None),
    ("Hills.mmx", 3, [(9, 0, 1, 0, -1), (0, 1, 0, 0, 1), (8, 2, 0, 0, 1)], None),
    ("Tower.mmx", 3, [(8, 0, 1, 0, -1), (0, 1, 0, 0, 1), (9, 2, 0, 0, 1)], T3),
    ("EB4.mmx", 3, [(0, 0, 1, 0, -1), (9, 1, 0, 0, 1), (8, 2, 0, 0, 1)], T3),
    ("Kaliforn.mmx", 5, [(8, 0, 1, 0), (0, 1, 0, 0), (9, 2, 1, 0), (5, 3, 0, 0)], T3),
    ("Death.mmx", 7, [(0, 0, 1, 0), (8, 1, 0, 0), (9, 2, 1, 0), (5, 3, 0, 0)], None),
]
# Second held-out set, drawn after tuning on HELDOUT's losses: maps used nowhere else.
HELDOUT2 = [
    ("Carville.mmx", 3, [(8, 0, 1, 0), (0, 1, 0, 0)], None),
    ("Carville.mmx", 3, [(8, 0, 0, 0), (0, 1, 1, 0)], None),
    ("Disaster.mmx", 3, [(9, 0, 1, 0), (8, 1, 0, 0)], T3),
    ("Disaster.mmx", 3, [(9, 0, 0, 0), (8, 1, 1, 0)], T3),
    ("EB1.mmx", 3, [(2, 0, 1, 0), (9, 1, 0, 0)], None),
    ("EB1.mmx", 3, [(2, 0, 0, 0), (9, 1, 1, 0)], None),
    ("EB5.mmx", 3, [(5, 0, 1, 0), (4, 1, 0, 0)], {"StartBase": "2"}),
    ("EB5.mmx", 3, [(5, 0, 0, 0), (4, 1, 1, 0)], {"StartBase": "2"}),
    ("Round.mmx", 3, [(7, 0, 1, 0), (1, 1, 0, 0)], None),
    ("Round.mmx", 3, [(7, 0, 0, 0), (1, 1, 1, 0)], None),
    ("Shrapnel.mmx", 3, [(0, 0, 1, 0), (9, 1, 0, 0)], T3),
    ("Shrapnel.mmx", 3, [(0, 0, 0, 0), (9, 1, 1, 0)], T3),
    ("Potomac.mmx", 5, [(8, 0, 1, 0), (0, 1, 0, 0), (9, 2, 0, 0), (3, 3, 0, 0)], None),
    ("PowdrKeg.mmx", 7, [(0, 0, 1, 0), (8, 1, 0, 0), (9, 2, 0, 0), (6, 3, 0, 0), (2, 4, 0, 0), (1, 5, 0, 0)], None),
]
# Faction balance: director against director, so both sides play equally well. Each pairing of
# America (0), Russia (8) and Yuri (9) on four 2-player maps, with the starts swapped.
BALANCE = [(m, 3, [(a, s, 1, 0), (b, 1 - s, 1, 0)], None)
           for m in ("Arena.mmx", "Hills.mmx", "Tower.mmx", "Lostlake.mmx")
           for a, b in ((0, 8), (0, 9), (8, 9))
           for s in (0, 1)]
SUITES = {"tune": None, "heldout": HELDOUT, "heldout2": HELDOUT2, "hard": HARD, "balance": BALANCE}


def suite_list(outdir, matches, frames=60000):
    lines = []
    for i, (m, human, ais, extra) in enumerate(matches):
        line = run(os.path.join(outdir, f"{i:02d}-{os.path.splitext(os.path.basename(m))[0].replace(' ', '_')}"),
                   m, ais, human, frames, extra=extra)
        print(line, flush=True)
        lines.append(line)
    return lines


def suite(outdir, filt=None, frames=60000):
    lines = []
    for i, (m, human, dc, bc, ds, bs) in enumerate(SUITE):
        if filt and filt not in m:
            continue
        # director is AI1 in even runs, AI2 in odd ones, so slot order doesn't favour it
        ais = [(dc, ds, 1, 0), (bc, bs, 0, 0)]
        if i % 2:
            ais.reverse()
        line = run(os.path.join(outdir, f"{i:02d}-{os.path.splitext(m)[0]}"), m, ais, human, frames)
        print(line, flush=True)
        lines.append(line)
    return lines


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] not in ("run", "summary", "suite", "restore", "factions", "units"):
        raise SystemExit(__doc__)
    if args[0] == "restore":
        restore()
        raise SystemExit
    if args[0] == "suite":
        name = args[2] if len(args) > 2 else "tune"
        if SUITES.get(name):
            suite_list(args[1], SUITES[name])
        else:
            suite(args[1], None if name == "tune" else name)
        raise SystemExit
    if args[0] == "units":
        print(units(args[1:]))
        raise SystemExit
    if args[0] == "factions":
        print(factions(args[1:]))
        raise SystemExit
    if args[0] == "summary":
        for d in args[1:]:
            print(summary(d), "->", outcome(d))
        print(tally(args[1:]))
        raise SystemExit
    opts = {"--human-start": -1, "--frames": 40000, "--speed": 0, "--seed": 0, "--camera": -1}
    rest = []
    it = iter(args[1:])
    for a in it:
        if a in opts:
            opts[a] = int(next(it))
        else:
            rest.append(a)
    outdir, map_file, *ai_args = rest
    extra = {"RevealMap": "1", "Camera": str(opts["--camera"])} if opts["--camera"] >= 0 else None
    print(run(outdir, map_file, [parse_ai(a) for a in ai_args], opts["--human-start"], opts["--frames"],
              opts["--speed"], opts["--seed"], extra=extra))
