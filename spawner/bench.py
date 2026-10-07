#!/usr/bin/env python3
"""Run unattended AI-vs-AI skirmishes and collect yspawn-bench.csv results.

usage: bench.py run OUTDIR MAP AI1 AI2 [...] [--human-start N] [--frames N] [--speed N] [--seed N] [--set KEY=VALUE]
           [--res WxH|native]
           each AI is COUNTRY[:START[:DIRECTOR[:DIFFICULTY[:TEAM[:PLAN]]]]], e.g. 8:0:1 9:1:0 (DIRECTOR 1 = new logic,
           0 = stock Brutal, >1 = DirectorFlags bitmask; 16319 = everything but the strategy layer;
           PLAN 0-3 forces balanced, rush, boom or siege)
       bench.py restore          put back the game directory's own yspawn.ini/.log/.map and RA2MD.INI after runs
       bench.py summary DIR...   print one line per match directory
       bench.py suite OUTDIR [tune|heldout|hard|MAPFILTER] [--frames N] [--games N] [--res WxH|native] [--jobs N]
                                 director vs stock Brutal match sets; --jobs N plays N at once, without
                                 windows, each in its own slot under FARM
       bench.py ab DIR...        strategy on against off, per plan and country: wins and mean placement

The idle human uses Human in peace, so it never takes part and never loses. The game directory's
yspawn.ini, yspawn.log, yspawn.map and RA2MD.INI are saved first and restored afterwards. Games run
at 800x600 (--res): drawing is most of a frame's cost at the user's resolution; --res native keeps
RA2MD.INI's own, to watch a game.
"""
import configparser, csv, glob, os, shutil, signal, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spawn

SAVED = ["yspawn.ini", "yspawn.log", "yspawn.map", "RA2MD.INI"]
BACKUP = os.path.expanduser("~/.cache/ra2-bench-saved")
ABSENT = "absent.txt"   # in BACKUP: the SAVED files the game dir did not have
# Benchmark games' resolution (--res WxH, --res native for the user's own). The game draws on the
# CPU: at 2560x1440 a frame's drawing cost several times its logic (one 1v1 game ran 360-410
# frames a second, at 800x600 2200-3100).
RES = (800, 600)


def set_resolution(game, width, height):
    """ScreenWidth/ScreenHeight in the game dir's RA2MD.INI [Video], the rest of the file as it was
    (put_back restores it with the other SAVED files)."""
    path = os.path.join(game, "RA2MD.INI")
    with open(path, newline="", encoding="latin-1") as f:
        lines = f.read().split("\r\n")
    want = {"ScreenWidth": str(width), "ScreenHeight": str(height)}
    video = next((i for i, l in enumerate(lines) if l.strip().lower() == "[video]"), None)
    if video is None:
        lines[-1:-1] = ["[Video]"] + [f"{k}={v}" for k, v in want.items()]
    else:
        end = next((i for i in range(video + 1, len(lines)) if lines[i].startswith("[")), len(lines))
        for i in range(video + 1, end):
            key = lines[i].split("=", 1)[0].strip()
            if key in want:
                lines[i] = f"{key}={want.pop(key)}"
        lines[video + 1:video + 1] = [f"{k}={v}" for k, v in want.items()]
    spawn.atomic_write(path, "w", "\r\n".join(lines), newline="", encoding="latin-1")


def bench_leftover():
    """True when the game-dir yspawn.ini is a benchmark's (a killed run's), not the user's."""
    try:
        with open(os.path.join(spawn.GAME, "yspawn.ini"), encoding="latin-1") as f:
            return any(line.strip() == "Benchmark=1" for line in f)
    except FileNotFoundError:
        return False


def snapshot():
    """Save the user's launcher files, kept until `bench.py restore`. A killed run leaves its own
    yspawn.ini (Benchmark=1) behind: keep the older backup, never save a benchmark's files as the
    user's. Otherwise the game dir holds the user's files, newer than any backup (they played in
    between, and the restore would overwrite their last yspawn.log): save them afresh."""
    if bench_leftover():
        return
    shutil.rmtree(BACKUP, ignore_errors=True)
    os.makedirs(BACKUP)
    absent = []
    for f in SAVED:
        p = os.path.join(spawn.GAME, f)
        if os.path.exists(p):
            shutil.copy2(p, BACKUP)
        else:
            absent.append(f)
    with open(os.path.join(BACKUP, ABSENT), "w") as out:
        out.write("".join(f + "\n" for f in absent))


def put_back():
    """Copy the saved files over the game dir's, and delete the ones the user did not have."""
    try:
        with open(os.path.join(BACKUP, ABSENT)) as f:
            absent = f.read().split()
    except FileNotFoundError:   # a backup from before the list: nothing known to be absent
        absent = []
    for f in SAVED:
        p = os.path.join(BACKUP, f)
        if os.path.exists(p):
            shutil.copy2(p, spawn.GAME)
        elif f in absent and os.path.exists(os.path.join(spawn.GAME, f)):
            os.remove(os.path.join(spawn.GAME, f))


def restore():
    """Put the user's launcher files back and forget the backup."""
    if not os.path.isdir(BACKUP):
        return
    put_back()
    shutil.rmtree(BACKUP)


def bench_rows(path):
    """(header, house rows, result row or None) of a yspawn-bench.csv, or None when it holds no
    house row: the game never reached its first sample (it hung or crashed while loading, or the
    DLL refused the match). The header is buffered until that first sample, so the file is empty."""
    try:
        with open(path, newline="") as f:
            rows = [r for r in csv.reader(f) if r]
    except FileNotFoundError:
        return None
    if len(rows) < 2:
        return None
    header, body = rows[0], rows[1:]
    result = next((r for r in body if r[0] == "result"), None)
    houses = [r for r in body if r[0] != "result"]
    return (header, houses, result) if houses else None


def write_ini(map_file, ais, human_start, frames, speed, seed, extra=None):
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    ini["Settings"] = dict(Map=map_file, Name="Observer", Country="4", Color="7", Start=str(human_start),
                           Team="-1", Credits="10000", GameSpeed=str(speed), UnitCount="0", TechLevel="10",
                           ShortGame="1", Superweapons="1", HumanInPeace="1", RevealMap="1", TeamTelemetry="0", Crates="0",
                           Bases="1", MCVRedeploy="1", BuildOffAlly="1", GameMode="1", Benchmark="1",
                           FrameLimit=str(frames), Seed=str(seed))
    if extra:
        units = extra.pop("Units", None)   # [Units] lines: TYPE,COUNTRY,X,Y,FACING,MISSION
        ini["Settings"].update(extra)
        if units:
            ini["Units"] = {str(i): u for i, u in enumerate(units, 1)}
    tier = int(ini["Settings"].pop("StartBase", 0) or 0)
    if tier:   # 1-3: every player starts with that tier's buildings (0: the usual MCV)
        import startbase
        countries = dict.fromkeys(startbase.country_name(c) for c in [4] + [a[0] for a in ais])
        ini["StartBase"] = startbase.section(countries, tier)
    for i, ai in enumerate(ais, 1):
        country, start, director, difficulty = ai[:4]
        team = ai[4] if len(ai) > 4 else -1
        ini[f"AI{i}"] = dict(Country=str(country), Color=str(i - 1), Difficulty=str(difficulty),
                             Start=str(start), Team=str(team), Director=str(int(director != 0)))
        if len(ai) > 5 and ai[5] >= 0:   # force a strategy plan (director-policy.h PLAN_*)
            ini[f"AI{i}"]["DirectorPlan"] = str(ai[5])
        if director > 1:   # a DirectorFlags bitmask (director-policy.h DIR_F_*)
            ini[f"AI{i}"]["DirectorFlags"] = str(director)
    return ini


def parse_ai(text):
    parts = [int(p) for p in text.split(":")]
    parts += [-1, 1, 0, -1, -1][len(parts) - 1:]
    return tuple(parts[:6])


# Slots for games side by side (--jobs N): slot K has a game dir of links to the game's files, with
# its own copies of what a game writes or we set, and its own Proton prefix (Proton waits for the
# prefix's wineserver to end before it starts a game). On the prefix's btrfs, the copy shares blocks.
FARM = "/mnt/data/SteamLibrary/ra2-bench-farm"
SLOT_OWN = {"yspawn.ini", "yspawn.log", "yspawn.map", "yspawn-bench.csv", "yspawn-kills.csv", "yspawn-teams.csv",
            "except.txt", "RA2MD.INI", "ddraw.ini", "gamemd-spawn.exe", "yspawn.dll"}


def make_slot(k):
    """Slot k's (game dir, prefix), brought up to date: links for whatever the game dir gained, and
    fresh copies of the exe, the DLL (an install since the last run counts) and the user's settings.
    The exe is a copy so that its directory, which the game and the DLL work in, is the slot's."""
    game = os.path.join(FARM, f"slot{k}", "game")
    prefix = os.path.join(FARM, f"slot{k}", "prefix")
    os.makedirs(game, exist_ok=True)
    for name in os.listdir(spawn.GAME):
        if name not in SLOT_OWN and not name.endswith(".tmp") and not os.path.lexists(os.path.join(game, name)):
            os.symlink(os.path.join(spawn.GAME, name), os.path.join(game, name))
    for name in ("gamemd-spawn.exe", "yspawn.dll", "RA2MD.INI", "ddraw.ini"):   # (ddraw.ini synced by play())
        shutil.copy2(os.path.join(spawn.GAME, name), os.path.join(game, name + ".tmp"))
        os.replace(os.path.join(game, name + ".tmp"), os.path.join(game, name))
    unpin(os.path.join(game, "ddraw.ini"))
    if not os.path.isdir(os.path.join(prefix, "pfx")):
        shutil.rmtree(prefix, ignore_errors=True)
        subprocess.run(["cp", "-a", "--reflink=auto", spawn.PREFIX, prefix + ".tmp"], check=True)
        os.rename(prefix + ".tmp", prefix)
    return game, prefix


def unpin(path):
    """singlecpu=false in a slot's ddraw.ini [gamemd-spawn]: the [ddraw] default, true, pins the game to
    CPU 0, and copies side by side shared that one core (two took twice as long as one)."""
    with open(path, newline="", encoding="latin-1") as f:
        lines = f.read().split("\r\n")
    s = lines.index("[gamemd-spawn]")
    e = next((i for i in range(s + 1, len(lines)) if lines[i].startswith("[")), len(lines))
    lines = [l for i, l in enumerate(lines) if not (s < i < e and l.split("=", 1)[0].strip() == "singlecpu")]
    lines.insert(s + 1, "singlecpu=false")
    spawn.atomic_write(path, "w", "\r\n".join(lines), newline="", encoding="latin-1")


def run(outdir, map_file, ais, human_start=-1, frames=40000, speed=0, seed=0, timeout=3600, extra=None, slot=None):
    """One match in the game dir (slot None: the user's files are saved first and put back after), or
    in benchmark slot `slot`, beside other slots' games."""
    if slot is None:
        if spawn.running():
            raise SystemExit("game already running")
        game, prefix = spawn.GAME, spawn.PREFIX
    else:
        game, prefix = make_slot(slot)
        stop(game)   # a game a killed run left there
    os.makedirs(outdir, exist_ok=True)
    if slot is None:
        snapshot()
    result = os.path.join(game, "yspawn-bench.csv")
    # (the game dir's telemetry and crash report may be the user's: run() only takes ones from after the launch)
    for stale in ("yspawn-bench.csv", "yspawn-kills.csv") + (("yspawn-teams.csv", "except.txt") if slot else ()):
        if os.path.exists(os.path.join(game, stale)):
            os.remove(os.path.join(game, stale))
    launcher = None
    try:
        ini = write_ini(map_file, ais, human_start, frames, speed, seed or int(time.time()) & 0x7FFFFFFF, extra)
        with open(os.path.join(outdir, "yspawn.ini"), "w") as f:
            ini.write(f)
        spawn.prepare(ini, game)
        if RES:
            set_resolution(game, *RES)
        began = time.time()
        launcher = spawn.launch(game, prefix, headless=None if slot is None else RES or (800, 600))
        while not spawn.running(game) and time.time() - began < 60:
            time.sleep(1)
        crash = os.path.join(game, "except.txt")
        sampled = False
        while spawn.running(game) and time.time() - began < timeout:
            time.sleep(2)
            # a crash leaves the game hung on its error report: note it and move on
            # hung while loading: no benchmark row two minutes after the launch. The DLL creates
            # the CSV when it loads, but it stays empty until the first sample.
            sampled = sampled or bench_rows(result) is not None
            if time.time() - began > 120 and not sampled:
                print(f"{outdir}: no game frames after 120 s, stopping", flush=True)
                break
            if os.path.exists(crash) and os.path.getmtime(crash) >= began - 1:
                time.sleep(3)
                print(f"{outdir}: crashed, see except.txt", flush=True)
                break
        stop(game)
        for f in ("yspawn-bench.csv", "yspawn.log", "except.txt", "yspawn-teams.csv", "yspawn-kills.csv"):
            p = os.path.join(game, f)
            if os.path.exists(p) and os.path.getmtime(p) >= began - 1:
                shutil.move(p, os.path.join(outdir, f)) if f != "yspawn.log" else shutil.copy2(p, outdir)
        played = os.path.join(outdir, "yspawn-bench.csv")
        if os.path.exists(played) and not bench_rows(played):
            os.remove(played)   # never started: a rerun of the suite plays it again
        with open(os.path.join(outdir, "wall.txt"), "w") as f:
            f.write(f"{time.time() - began:.0f}\n")
    finally:
        if slot is None:
            put_back()
        if launcher and launcher.poll() is None:   # the Gamescope and Proton around the game
            try:
                os.killpg(launcher.pid, signal.SIGTERM)
                launcher.wait(20)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                pass
    return summary(outdir)


def stop(game=None, wait=20):
    """End the game (that dir's, or every copy), and wait for it to go: the next run() refuses to
    start beside it."""
    for sig in (signal.SIGTERM, signal.SIGKILL):
        pids = spawn.game_pids(game)
        if not pids:
            return
        for pid in pids:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
        for _ in range(wait):
            time.sleep(1)
            if not spawn.game_pids(game):
                return
    raise SystemExit("gamemd-spawn.ex survived SIGKILL: stopping")


def outcome(outdir):
    """'win' / 'loss' / 'draw' for the director house, or None. Timeouts go to the side with more
    buildings plus army value, and count as a draw within 20%."""
    rows = bench_rows(os.path.join(outdir, "yspawn-bench.csv"))
    if not rows:
        return None
    header, houses, result = rows
    last = {r[2]: dict(zip(header, r)) for r in houses}
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
    rows = bench_rows(os.path.join(outdir, "yspawn-bench.csv"))
    if not rows:
        return None
    header, houses, result = rows
    last = {r[2]: dict(zip(header, r)) for r in houses}
    ais = {h: d for h, d in last.items() if d["human"] == "0"}
    countries = sorted(d["country"] for d in ais.values())
    if result and result[2] == "win":
        # house -1: no AI survived (the last ones fell in the same sample interval)
        return countries, ais[result[3]]["country"] if result[3] in ais else "draw"
    score = {h: int(d["buildings"]) * 1000 + int(d["cost_infantry"]) + int(d["cost_vehicles"])
             + int(d["cost_aircraft"]) for h, d in ais.items()}
    ranked = sorted(score, key=score.get, reverse=True)
    if len(ranked) < 2 or (score[ranked[0]] - score[ranked[1]]) * 5 < score[ranked[0]]:
        return countries, "draw"
    return countries, ais[ranked[0]]["country"]


def placements(outdir):
    """[(house row, placement 0..1)] for one match: 0 is the winner, 1 the first one out.
    Eliminated houses rank by when they fell; survivors by buildings*1000 plus army value."""
    rows = bench_rows(os.path.join(outdir, "yspawn-bench.csv"))
    if not rows:
        return []
    header, houses, _ = rows
    last, fell = {}, {}
    for r in houses:
        d = dict(zip(header, r))
        if d["human"] == "1":
            continue
        last[d["house"]] = d
        if d["defeated"] == "1" and d["house"] not in fell:
            fell[d["house"]] = int(d["frame"])
    score = lambda d: (fell.get(d["house"], 10 ** 9), int(d["buildings"]) * 1000 + int(d["cost_infantry"])
                       + int(d["cost_vehicles"]) + int(d["cost_aircraft"]))
    ranked = sorted(last.values(), key=score, reverse=True)
    n = len(ranked)
    return [(d, i / (n - 1) if n > 1 else 0.0) for i, d in enumerate(ranked)]


def strategy_ab(dirs):
    """Strategy on against off, and per plan: houses, wins, mean placement (0 best, 1 worst)."""
    groups = {}
    for d in dirs:
        for row, place in placements(d):
            if "flags" not in row:
                continue
            on = int(row["flags"]) & 16384 != 0
            keys = ["strategy " + ("on" if on else "off")]
            if on:
                keys.append("plan " + row["plan"])
            keys.append("country " + row["country"])
            for k in keys:
                g = groups.setdefault(k, [0, 0, 0.0])
                g[0] += 1
                g[1] += place == 0.0
                g[2] += place
    out = [f"{'group':24} {'houses':>6} {'wins':>5} {'placement':>9}"]
    for k in sorted(groups):
        n, w, p = groups[k]
        out.append(f"{k:24} {n:6} {w:5} {p / n:9.2f}")
    return "\n".join(out)


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
    rows = bench_rows(os.path.join(outdir, "yspawn-bench.csv"))
    if not rows:   # suite_list counts these as games that never started
        return f"{outdir}: no result"
    header, houses, result = rows
    last = {r[2]: dict(zip(header, r)) for r in houses}
    parts = []
    for h in sorted(last, key=int):
        d = last[h]
        if d["human"] == "1":
            continue
        value = int(d["cost_infantry"]) + int(d["cost_vehicles"]) + int(d["cost_aircraft"])
        parts.append(f"h{h}:{d['country']}{'*' if d['director'] == '1' else ''}"
                     f"{' DEAD' if d['defeated'] == '1' else ''} b{d['buildings']} army${value}"
                     f" k{d['killed_units']}/{d['killed_buildings']}")
    frame = result[1] if result else houses[-1][0]
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
# Played as the user plays: tier-3 starting bases, superweapons off.
BALANCE = [(m, 3, [(a, s, 1, 0), (b, 1 - s, 1, 0)], {"StartBase": "3", "Superweapons": "0"})
           for m in ("Arena.mmx", "Hills.mmx", "Tower.mmx", "Lostlake.mmx")
           for a, b in ((0, 8), (0, 9), (8, 9))
           for s in (0, 1)]
# Director=1 plays director-policy.h DIR_F_DEFAULT: every feature but defenses-first (64), with
# postures off (DIR_F_NO_POSTURE, 65536). Masks for ablations start from it.
DIR_DEFAULT = 98239
# Strategy layer A/B: director with plans (1) against the director without them (16319: every
# default feature but DIR_F_STRATEGY), each pairing and start both ways round, on the user's settings.
NOSTRAT = 16319
USER = {"StartBase": "3", "Superweapons": "0", "Crates": "1"}
STRAT = [(m, 3, [(a, s, 1 if on == 0 else NOSTRAT, 0), (b, 1 - s, NOSTRAT if on == 0 else 1, 0)], dict(USER))
         for m in ("Arena.mmx", "Hills.mmx", "Tower.mmx", "Lostlake.mmx")
         for a, b in ((0, 8), (0, 9), (8, 9))
         for s in (0, 1)
         for on in (0, 1)]
# The same on held-out maps, from a bare MCV.
STRAT_MCV = [(m, 3, [(a, s, 1 if on == 0 else NOSTRAT, 0), (b, 1 - s, NOSTRAT if on == 0 else 1, 0)],
              {"Superweapons": "0", "Crates": "1"})
             for m in ("DeepFrze.yro", "Rockets.mmx", "Carville.mmx", "Round.mmx")
             for a, b in ((0, 8), (0, 9), (8, 9))
             for s in (0, 1)
             for on in (0, 1)]
# Island free-for-all on the user's map and settings: seven directors, strategy on in alternate
# slots, then the other way round.
ISO = "Maps/2024/2024 - (2-8) Isolation 1.2 NP.yro"
STRAT_ISO = [(ISO, 0, [(c, k + 1, 1 if (k + flip) % 2 == 0 else NOSTRAT, 0) for k, c in enumerate(order)], dict(USER))
             for order in ((0, 8, 9, 1, 2, 5, 7), (9, 0, 8, 7, 1, 2, 5))
             for flip in (0, 1)]
# Against mind control: Russia with its hunters, Bulldozers and blackouts (1) or without them
# (DIR_F_DEFAULT | DIR_F_NO_ANTIMIND), against Yuri, on the MCV maps, starts both ways round, twice.
NOMIND = DIR_DEFAULT | 32768
MIND = [(m, 3, [(8, s, 1 if on else NOMIND, 0), (9, 1 - s, 1, 0)], {"Superweapons": "0", "Crates": "1"})
        for m in ("DeepFrze.yro", "Rockets.mmx", "Carville.mmx", "Round.mmx")
        for s in (0, 1)
        for on in (1, 0)
        for rep in (0, 1)]
# Ablation of the strategy layer's postures: plans without postures (DIR_F_DEFAULT | DIR_F_NO_POSTURE)
# against no plans, as STRAT_MCV. Postures are off by default now, so this is DIR_DEFAULT itself.
NOPOSTURE = DIR_DEFAULT | 65536
POSTURE = [(m, 3, [(a, s, NOPOSTURE if on == 0 else NOSTRAT, 0), (b, 1 - s, NOSTRAT if on == 0 else NOPOSTURE, 0)],
            {"Superweapons": "0", "Crates": "1"})
           for m in ("DeepFrze.yro", "Rockets.mmx", "Carville.mmx", "Round.mmx")
           for a, b in ((0, 8), (0, 9), (8, 9))
           for s in (0, 1)
           for on in (0, 1)]
SUITES = {"posture": POSTURE, "mind": MIND, "strat": STRAT, "strat_iso": STRAT_ISO, "strat_mcv": STRAT_MCV, "tune": None, "heldout": HELDOUT, "heldout2": HELDOUT2, "hard": HARD, "balance": BALANCE}


JOBS = 1   # --jobs N: games side by side, each in a benchmark slot (1: one at a time, in the game dir)


def play(todo, frames):
    """Play (out, map, ais, human, extra[, seed]) matches, JOBS at once, printing a line per match as
    it ends.
    Three in a row that never started stop it: the launch is broken (no monitor, ...)."""
    lines, failed = [], 0

    def ended(line, out):
        nonlocal failed
        print(line, flush=True)
        lines.append(line)
        failed = failed + 1 if line.endswith("no result") else 0
        if failed >= 3:
            shutil.rmtree(out, ignore_errors=True)   # so a rerun plays them
            raise SystemExit("3 games in a row never started: stopping the suite")

    if JOBS <= 1:
        for out, m, ais, human, extra, *seed in todo:
            ended(run(out, m, ais, human, frames, seed=seed[0] if seed else 0, extra=dict(extra) if extra else None), out)
        return lines
    import concurrent.futures, queue
    spawn.sync_ddraw_ini()   # once, before the slots copy it
    free = queue.Queue()
    for k in range(1, JOBS + 1):
        free.put(k)

    def one(item):
        out, m, ais, human, extra, *seed = item
        k = free.get()
        try:
            return run(out, m, ais, human, frames, seed=seed[0] if seed else 0, extra=dict(extra) if extra else None,
                       slot=k), out
        finally:
            free.put(k)

    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        futures = [pool.submit(one, item) for item in todo]
        try:
            for f in concurrent.futures.as_completed(futures):
                ended(*f.result())
        except BaseException:   # stopped (^C, or the launch is broken): end the games still running
            for f in futures:
                f.cancel()
            for k in range(1, JOBS + 1):
                stop(os.path.join(FARM, f"slot{k}", "game"))
            concurrent.futures.wait(futures)
            for item in todo:   # cut short, with no result row: kept, a resumed suite counted it played
                played = os.path.join(item[0], "yspawn-bench.csv")
                rows = bench_rows(played)
                if rows and rows[2] is None:
                    os.remove(played)
            raise
    return lines


def suite_list(outdir, matches, frames=60000):
    todo = []
    for i, (m, human, ais, extra) in enumerate(matches):
        out = os.path.join(outdir, f"{i:02d}-{os.path.splitext(os.path.basename(m))[0].replace(' ', '_')}")
        if not bench_rows(os.path.join(out, "yspawn-bench.csv")):   # resumable: played already
            todo.append((out, m, ais, human, extra))
    return play(todo, frames)


def suite(outdir, filt=None, frames=60000, games=None):
    todo = []
    for i, (m, human, dc, bc, ds, bs) in enumerate(SUITE):
        if filt and filt not in m:
            continue
        if games is not None and len(todo) >= games:
            break
        # director is AI1 in even runs, AI2 in odd ones, so slot order doesn't favour it
        ais = [(dc, ds, 1, 0), (bc, bs, 0, 0)]
        if i % 2:
            ais.reverse()
        todo.append((os.path.join(outdir, f"{i:02d}-{os.path.splitext(m)[0]}"), m, ais, human, None))
    return play(todo, frames)


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] not in ("run", "summary", "suite", "restore", "factions", "units", "ab"):
        raise SystemExit(__doc__)
    if args[0] == "restore":
        restore()
        raise SystemExit
    if "--res" in args:   # WxH, or native: RA2MD.INI's own resolution
        i = args.index("--res")
        value = args[i + 1].lower()
        del args[i:i + 2]
        RES = None if value == "native" else tuple(int(v) for v in value.split("x"))
    if "--jobs" in args:   # N games side by side (suites)
        i = args.index("--jobs")
        JOBS = int(args[i + 1])
        del args[i:i + 2]
    if args[0] == "suite":
        # --frames N and --games N: a short smoke run of a suite's first games
        frames, games = 60000, None
        for flag in ("--frames", "--games"):
            if flag in args:
                i = args.index(flag)
                value = int(args[i + 1])
                del args[i:i + 2]
                frames, games = (value, games) if flag == "--frames" else (frames, value)
        name = args[2] if len(args) > 2 else "tune"
        if SUITES.get(name):
            suite_list(args[1], SUITES[name][:games], frames)
        else:
            suite(args[1], None if name == "tune" else name, frames, games)
        raise SystemExit
    if args[0] == "ab":
        print(strategy_ab(args[1:]))
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
    rest, sets = [], {}
    it = iter(args[1:])
    for a in it:
        if a == "--set":   # any [Settings] key, e.g. --set StartBase=3 --set Superweapons=0
            k, _, v = next(it).partition("=")
            sets[k] = v
        elif a in opts:
            opts[a] = int(next(it))
        else:
            rest.append(a)
    outdir, map_file, *ai_args = rest
    extra = {"Camera": str(opts["--camera"])} if opts["--camera"] >= 0 else {}
    extra = {**extra, **sets} or None
    print(run(outdir, map_file, [parse_ai(a) for a in ai_args], opts["--human-start"], opts["--frames"],
              opts["--speed"], opts["--seed"], extra=extra))
