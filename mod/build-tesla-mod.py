#!/usr/bin/env python3
"""Build and install the "Liberator" mod: an overpowered, Allied-only Tesla tank with its own art.

usage: build-tesla-mod.py install     # write the mod files into the game dir
       build-tesla-mod.py uninstall   # remove them again (game falls back to stock)
       build-tesla-mod.py build DIR   # just write the mod files to DIR

Loose files in the game directory override the copies packed in the MIX archives:
  rulesmd.ini  - stock rules (expandmd01.mix) + the unit, its weapons and warhead
  artmd.ini    - stock art (ra2md.mix/localmd.mix) + the unit's art entry
  ra2md.csf    - stock strings (langmd.mix) + its name "Liberator"
  aimd.ini     - stock AI + Liberator/Bulldozer teams and enhanced Brutal skirmish AI
  attnk*.vxl/.hva, attkicon.shp - model and cameo from mod/assets (made by make_graphics.py)
  ggchdf.shp, g?chdfmk.shp, chdfglow.shp, chdftur.vxl/.hva, chdficon.shp - Cheat Defense art
                 from mod/assets (made by cheatdef_art.py)
Only Yuri's Revenge reads these *md files; base Red Alert 2 is unaffected.
"""
import os, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import csf, mixextract, bulldozer, oil_ai, combat_ai

GAME = "/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II"
ASSETS = ["attnk.vxl", "attnk.hva", "attnktur.vxl", "attnktur.hva", "attkicon.shp"]
# Cheat Defense art, made by cheatdef_art.py (same list as its FILES)
DEF_ASSETS = ["ggchdf.shp", "chdfglow.shp", "chdftur.vxl", "chdftur.hva", "chdficon.shp"] + \
    [f"g{c}chdfmk.shp" for c in "taudlng"]
ASSETS += DEF_ASSETS + bulldozer.ASSETS
MANAGED = ["rulesmd.ini", "artmd.ini", "ra2md.csf", "aimd.ini"] + ASSETS
UNIT_NAME = "Liberator"
CAMEO = "ATTKICON"

UNIT_ID = "ATTNK"
# Fire position in leptons (forward, sideways, up): the electrode tip of the model,
# liberator_model.ELECTRODE (20, 0, 16.5 voxels) at about 6 leptons per voxel. Stock TTNK: 60,0,100.
UNIT_FLH = "120,0,100"
ALLIED_COUNTRIES = "British,French,Germans,Americans,Alliance"
LIST_KEY = "85"

# Stats to put on top of a full copy of [TTNK]. Stock values in comments.
UNIT_OVERRIDES = {
    "Name": UNIT_NAME,
    "Image": UNIT_ID,                    # own voxels + art entry
    "UIName": f"Name:{UNIT_ID}",         # string added to ra2md.csf
    "Prerequisite": "GAWEAP,GATECH",     # Allied war factory + Allied battle lab
    "Owner": ALLIED_COUNTRIES,           # any Allied country
    "RequiredHouses": ALLIED_COUNTRIES,
    "CrateGoodie": "no",                 # crates must not give it to other countries
    "Primary": "ATankBolt",
    "ElitePrimary": "ATankBoltE",
    "Strength": "1500",                  # 300
    "Speed": "2",                        # 6; halved twice from the earlier 9 (Speed is an integer)
    "Sight": "10",                       # 8
    "ROT": "8",                          # 5
    "Cost": "3000",                      # 1200
    "Soylent": "3000",
    "SelfHealing": "yes",                # regenerates like it has a repair drone
    "ImmuneToPsionics": "yes",           # Yuri can't mind-control it
    "ImmuneToRadiation": "yes",
    "Weight": "5",                       # 3.5, harder to shove around / chrono
    "BuildTimeMultiplier": "1.0",        # 1.2
}

NEW_SECTIONS = f"""
; ===== Liberator mod (Allied-only Tesla tank) =====
[ATankBolt]
Damage=300
ROF=35
Range=7
Speed=100
Warhead=LibertyElectric
Report=TeslaTankAttack
Projectile=Electricbounce
IsElectricBolt=true

[ATankBoltE]
Damage=450
ROF=25
Range=8
Speed=100
Warhead=LibertyElectric
Report=TeslaTankAttack
Projectile=Electricbounce
IsElectricBolt=true

; Like [Electric], but full damage against buildings too
[LibertyElectric]
Verses=100%,100%,100%,100%,100%,100%,100%,100%,100%,200%,100%
InfDeath=5
Wood=yes
Wall=yes
AnimList=TSTIMPCT
"""

# ===== Cheat Defense: observer's building, never built by the AI =====
DEF_ID = "CHEATDEF"
DEF_NAME = "Cheat Defense"
DEF_LIST_KEY = "407"
ALL_COUNTRIES = "British,French,Germans,Americans,Alliance,Russians,Confederation,Africans,Arabs,YuriCountry"
DEF_ART = "GTCHDF"   # own art: NewTheater swaps the 2nd letter per theater (files g?chdf*), G = generic
# Put on top of a full copy of the Grand Cannon [GTGCAN], with its own art from cheatdef_art.py.
DEF_OVERRIDES = {
    "Name": DEF_NAME,
    "UIName": f"Name:{DEF_ID}",
    "Image": DEF_ART,                    # own base, build-up, glow anim and cameo (artmd.ini [GTCHDF])
    "TurretAnim": "CHDFTUR",             # own voxel turret, chdftur.vxl/.hva
    "TurretRecoil": "no",                # single-piece turret, no barrel voxel
    "TurretAnimX": "0",                  # our base and turret pivot are exactly centred (stock 3)
    "Owner": ALL_COUNTRIES,              # available whichever country you play
    "TechLevel": "1",
    "Cost": "1",
    "Points": "1",
    "Power": "0",                        # needs no power ...
    "Powered": "no",                     # ... and keeps working without it
    "Adjacent": "255",                   # can be placed (almost) any distance from your base
    "BaseNormal": "no",                  # but does not extend your base for other buildings
    "AIBuildThis": "no",                 # AI never builds it
    "SpySat": "yes",                     # reveals the whole map, like the Spy Satellite Uplink
    "Strength": "10000",
    "Immune": "yes",                     # invulnerable
    "Capturable": "false",
    "Drainable": "no",                   # Yuri's Floating Disc can't drain it
    "ImmuneToPsionics": "yes",
    "Sight": "16",
    "ROT": "60",                         # turret snaps onto targets
    "Primary": "CheatBolt",
}
# the Grand Cannon is French-only and needs radar; drop those so any country can build it with just a Con Yard
# and the Grand Cannon's barrel-recoil settings (our turret has no barrel)
DEF_DROP = ("Prerequisite", "Secondary", "AIBasePlanningSide", "RequiredHouses", "ForbiddenHouses",
            "BarrelTravel", "BarrelCompressFrames", "BarrelHoldFrames", "BarrelRecoverFrames", "TurretTravel")

# art for the Cheat Defense; the base layout follows the Grand Cannon's [GTGCAN] entry
DEF_ART_SECTIONS = f"""
[{DEF_ART}]   ; Cheat Defense (mod)
Remapable=yes
NewTheater=yes
Cameo=CHDFICON
Foundation=2x2
Height=3
Buildup={DEF_ART}MK
DemandLoadBuildup=true
FreeBuildup=true
PrimaryFireFLH=270,0,225   ; tips of the twin Tesla prongs
CanHideThings=True
CanBeHidden=False
OccupyHeight=2
ActiveAnim=CHDFGLOW
ActiveAnimZAdjust=-30
ActiveAnimPowered=no

[CHDFGLOW]   ; Cheat Defense glow ring, crystals and arcs (mod)
Normalized=yes
LoopStart=0
LoopEnd=15   ; inclusive: frames 0-15, like [NATSLA_AD] 10-19
LoopCount=-1
Rate=300
Layer=ground
Shadow=no
"""

DEF_SECTIONS = """
; ===== Cheat Defense mod =====
; one-shots anything on the ground or in the air within Grand Cannon range
[CheatBolt]
Damage=10000
ROF=10
Range=15
Speed=100
Warhead=CheatWH
Projectile=CheatProj
Report=TeslaCoilAttack
IsElectricBolt=true

[CheatProj]
Inviso=yes
Image=none
AA=yes
AG=yes
SubjectToCliffs=no
SubjectToElevation=no
SubjectToWalls=no

; full damage against every armour type, no splash
[CheatWH]
Verses=100%,100%,100%,100%,100%,100%,100%,100%,100%,100%,100%
InfDeath=5
AnimList=TSTIMPCT
"""


def section_lines(lines, name):
    """Return (start, end) indices of section `name` (header line .. line before next header)."""
    start = next(i for i, l in enumerate(lines) if l.split(";")[0].strip() == f"[{name}]")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].lstrip().startswith("[")), len(lines))
    return start, end


def clone_section(lines, src, new_id, overrides, drop=()):
    """Full copy of section `src` as `new_id`: `overrides` replace or add keys, `drop` removes keys."""
    s, e = section_lines(lines, src)
    body, seen = [], set()
    for l in lines[s + 1:e]:
        key = l.split("=", 1)[0].strip() if "=" in l and not l.lstrip().startswith(";") else None
        if key in drop:
            continue
        if key in overrides:
            body.append(f"{key}={overrides[key]}")
            seen.add(key)
        else:
            body.append(l)
    while body and (not body[-1].strip() or body[-1].lstrip().startswith(";")):
        body.pop()
    body += [f"{k}={v}" for k, v in overrides.items() if k not in seen]
    return [f"[{new_id}]"] + body


def register(lines, list_section, key, type_id):
    """Add key=type_id after the last entry of a type list such as [VehicleTypes]."""
    s, e = section_lines(lines, list_section)
    keys = [l.split("=", 1)[0].strip() for l in lines[s + 1:e] if "=" in l.split(";")[0]]
    if key in keys:
        raise SystemExit(f"[{list_section}] key {key} already used")
    last = max(i for i in range(s + 1, e) if "=" in lines[i].split(";")[0])
    lines.insert(last + 1, f"{key}={type_id}")


def patch(text):
    lines = text.split("\r\n")
    for new_id in (UNIT_ID, DEF_ID, bulldozer.UNIT_ID):
        if any(l.split(";")[0].strip() == f"[{new_id}]" for l in lines):
            raise SystemExit(f"[{new_id}] already present; refusing to patch twice")

    unit = clone_section(lines, "TTNK", UNIT_ID, UNIT_OVERRIDES)
    defense = clone_section(lines, "GTGCAN", DEF_ID, DEF_OVERRIDES, DEF_DROP)
    register(lines, "VehicleTypes", LIST_KEY, UNIT_ID)
    register(lines, "BuildingTypes", DEF_LIST_KEY, DEF_ID)
    dozer = clone_section(lines, "HTNK", bulldozer.UNIT_ID, bulldozer.OVERRIDES)
    register(lines, "VehicleTypes", "86", bulldozer.UNIT_ID)

    while lines and lines[-1] == "":
        lines.pop()
    lines += [""] + NEW_SECTIONS.strip("\n").split("\n")
    lines += [""] + unit
    lines += [""] + DEF_SECTIONS.strip("\n").split("\n")
    lines += [""] + defense
    lines += [""] + bulldozer.SECTIONS.strip().splitlines() + [""] + dozer + ["", ""]
    # Difficulty arrays are Hard/Normal/Easy. Keep the latter two stock.
    s, e = section_lines(lines, 'General')
    for i in range(s + 1, e):
        if lines[i].split('=', 1)[0].strip() == 'TeamDelays':
            lines[i] = 'TeamDelays=900,2500,3500'
    return "\r\n".join(lines)


def stock(archive, path):
    with open(os.path.join(GAME, archive), "rb") as f:
        return mixextract.extract(f.read(), path)


def patch_art(text):
    lines = text.split("\r\n")
    if any(l.split(";")[0].strip() == f"[{UNIT_ID}]" for l in lines):
        raise SystemExit(f"art [{UNIT_ID}] already present")
    s, e = section_lines(lines, "TTNK")
    body = [l for l in lines[s + 1:e] if l.strip()]
    swap = {"Cameo": CAMEO, "AltCameo": CAMEO, "PrimaryFireFLH": UNIT_FLH, "ElitePrimaryFireFLH": UNIT_FLH}
    body = [f"{l.split('=')[0]}={swap[l.split('=')[0]]}" if l.split("=")[0] in swap else l for l in body]
    while lines and lines[-1] == "":
        lines.pop()
    lines += ["", f"[{UNIT_ID}]   ; Liberator (mod)"] + body
    lines += DEF_ART_SECTIONS.rstrip("\n").split("\n")
    lines += bulldozer.ART.strip().splitlines() + ["", ""]
    return "\r\n".join(lines)


# AI: modelled on the German Tank Destroyer team ("Nation German Tank Dest 1", 0A87293C-G)
AI_TASKFORCE, AI_TEAM, AI_TRIGGER = "0F1BE800-G", "0F1BE810-G", "0F1BE820-G"
AI_TEAM_TEMPLATE = "0A87293C-G"
AI_SCRIPT = "08DA356C-G"  # stock "General Vehicle Attack"
# name, team, owner (<all>, narrowed by side), techlevel, condition 1 = owner owns >= 1 GATECH, weights 500/10/500,
# skirmish, unused, side 1 (Allied), not base defence, no 2nd team, easy/medium/hard
AI_TRIGGER_LINE = (f"{AI_TRIGGER}=Allied Liberator 1,{AI_TEAM},<all>,2,1,GATECH,"
                   "0100000003000000000000000000000000000000000000000000000000000000,"
                   "500.000000,10.000000,500.000000,1,0,1,0,<none>,1,1,1")


def append_to_list(lines, section, value):
    """Add `value` under the next free numeric key of a list section like [TaskForces]."""
    s, e = section_lines(lines, section)
    entries = [i for i in range(s + 1, e) if "=" in lines[i].split(";")[0]]
    keys = [int(lines[i].split("=", 1)[0]) for i in entries if lines[i].split("=", 1)[0].strip().isdigit()]
    lines.insert(entries[-1] + 1, f"{max(keys) + 1}={value}")


def patch_ai(text):
    lines = text.split("\r\n")
    if any(l.strip() == f"[{AI_TASKFORCE}]" for l in lines):
        raise SystemExit("AI entries already present")
    s, e = section_lines(lines, AI_TEAM_TEMPLATE)
    team = [l for l in lines[s + 1:e] if l.strip()]
    team = ["Name=Allied Liberator 1" if l.startswith("Name=") else
            f"TaskForce={AI_TASKFORCE}" if l.startswith("TaskForce=") else
            f"Script={AI_SCRIPT}" if l.startswith("Script=") else l for l in team]
    append_to_list(lines, "TaskForces", AI_TASKFORCE)
    append_to_list(lines, "TeamTypes", AI_TEAM)
    s, e = section_lines(lines, "AITriggerTypes")
    last = max(i for i in range(s + 1, e) if "=" in lines[i].split(";")[0])
    lines.insert(last + 1, AI_TRIGGER_LINE)
    while lines and lines[-1] == "":
        lines.pop()
    lines += ["", f"[{AI_TASKFORCE}]", "Name=3 Liberators", f"0=3,{UNIT_ID}", "Group=-1",
              "", f"[{AI_TEAM}]"] + team + ["", ""]
    # Register a separate Soviet siege team without changing the Allied Liberator team.
    for new_id in (bulldozer.AI_TASKFORCE, bulldozer.AI_TEAM, bulldozer.AI_TRIGGER):
        if any(new_id in line.split(";")[0] for line in lines):
            raise SystemExit(f"AI ID {new_id} already present")
    dozer_team = clone_section(lines, bulldozer.AI_TEAM_TEMPLATE, bulldozer.AI_TEAM, {
        "Name": "Soviet Bulldozer Assault", "TaskForce": bulldozer.AI_TASKFORCE,
        "Script": bulldozer.AI_SCRIPT, "Max": "1",
    })
    append_to_list(lines, "TaskForces", bulldozer.AI_TASKFORCE)
    append_to_list(lines, "TeamTypes", bulldozer.AI_TEAM)
    s, e = section_lines(lines, "AITriggerTypes")
    last = max(i for i in range(s + 1, e) if "=" in lines[i].split(";")[0])
    lines.insert(last + 1, bulldozer.AI_TRIGGER_LINE)
    lines += [f"[{bulldozer.AI_TASKFORCE}]", "Name=2 Bulldozers + 4 Rhinos",
              f"0=2,{bulldozer.UNIT_ID}", "1=4,HTNK", "Group=-1", ""] + dozer_team + ["", ""]
    add_oil_ai(lines)
    # Building indices come from stock; new types are appended to its registry.
    rules = {}
    section = None
    for line in stock('expandmd01.mix', 'rulesmd.ini').decode('latin-1').splitlines():
        line = line.split(';')[0].strip()
        if line.startswith('['):
            section = rules.setdefault(line[1:line.index(']')], {})
        elif section is not None and '=' in line:
            key, value = line.split('=', 1)
            section[key.strip()] = value.strip()
    combat_ai.patch(lines, section_lines, clone_section, append_to_list, rules)
    return "\r\n".join(lines)


def add_oil_ai(lines):
    """Append Brutal-only oil teams without editing any stock trigger or team."""
    if any('0F1BC' in line.split(';')[0] for line in lines):
        raise SystemExit('Oil AI IDs already present')
    for script_id, (name, actions) in oil_ai.SCRIPTS.items():
        append_to_list(lines, 'ScriptTypes', script_id)
        lines += [f'[{script_id}]', f'Name={name}']
        lines += [f'{i}={action}' for i, action in enumerate(actions)] + ['']

    for side, faction, template, engineer, infantry, vehicles in oil_ai.FACTIONS:
        roles = (
            ('Capture', (f'1,{engineer}',), oil_ai.CAPTURE_SCRIPT, '2'),
            ('Infantry Guard', infantry, oil_ai.NEAR_GUARD_SCRIPT, '1'),
            ('Vehicle Patrol', vehicles, oil_ai.PATROL_SCRIPT, '1'),
        )
        for role, (label, members, script, limit) in enumerate(roles):
            taskforce_id, team_id = oil_ai.ids(side, role)
            name = f'Brutal {faction} Oil {label}'
            team = clone_section(lines, template, team_id, {
                'Name': name, 'TaskForce': taskforce_id, 'Script': script,
                'House': '<none>', 'Max': limit, 'Priority': '60',
                'Autocreate': 'yes', 'Full': 'no', 'Reinforce': 'no',
                'Suicide': 'no', 'Aggressive': 'no' if role == 0 else 'yes',
                'AvoidThreats': 'yes' if role == 0 else 'no',
                'AreTeamMembersRecruitable': 'no', 'LooseRecruit': 'no',
                'IsBaseDefense': 'no', 'OnlyTargetHouseEnemy': 'no',
            })
            append_to_list(lines, 'TaskForces', taskforce_id)
            append_to_list(lines, 'TeamTypes', team_id)
            lines += [f'[{taskforce_id}]', f'Name={name}']
            lines += [f'{i}={member}' for i, member in enumerate(members)]
            lines += ['Group=-1', ''] + team + ['', '']

        capture, guard, patrol = (oil_ai.ids(side, role)[1] for role in range(3))
        triggers = [
            oil_ai.trigger(side, 0, f'Brutal {faction} Neutral Oil', capture, 7, (900, 500, 1000)),
            oil_ai.trigger(side, 1, f'Brutal {faction} Reclaim Oil', capture, 1, (180, 80, 300)),
            oil_ai.trigger(side, 2, f'Brutal {faction} Hold Oil', guard, 0, (240, 100, 350)),
            oil_ai.trigger(side, 3, f'Brutal {faction} Patrol Oil', patrol, 0, (200, 100, 300)),
        ]
        s, e = section_lines(lines, 'AITriggerTypes')
        last = max(i for i in range(s + 1, e) if '=' in lines[i].split(';')[0])
        lines[last + 1:last + 1] = triggers


def build(out_dir):
    rules = patch(stock("expandmd01.mix", "rulesmd.ini").decode("latin-1")).encode("latin-1")
    open(os.path.join(out_dir, "rulesmd.ini"), "wb").write(rules)
    art = patch_art(stock("ra2md.mix", "localmd.mix/artmd.ini").decode("latin-1")).encode("latin-1")
    open(os.path.join(out_dir, "artmd.ini"), "wb").write(art)
    ai = patch_ai(stock("ra2md.mix", "localmd.mix/aimd.ini").decode("latin-1")).encode("latin-1")
    open(os.path.join(out_dir, "aimd.ini"), "wb").write(ai)
    tmp = os.path.join(out_dir, "ra2md.csf")
    open(tmp, "wb").write(stock("langmd.mix", "ra2md.csf"))
    header, entries = csf.load(tmp)
    csf.set_string(entries, f"Name:{UNIT_ID}", UNIT_NAME)
    csf.set_string(entries, f"Name:{DEF_ID}", DEF_NAME)
    csf.set_string(entries, f"Name:{bulldozer.UNIT_ID}", bulldozer.UNIT_NAME)
    csf.save(tmp, header, entries)
    for a in ASSETS:
        src = os.path.join(HERE, "assets", a)
        if not os.path.exists(src):
            raise SystemExit(f"missing {src}; run make_graphics.py / cheatdef_art.py / make_bulldozer.py first")
        shutil.copy(src, os.path.join(out_dir, a))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "build":
        os.makedirs(sys.argv[2], exist_ok=True)
        build(sys.argv[2])
    elif cmd in ("install", "uninstall"):
        if subprocess.run(["pgrep", "-x", "gamemd.exe|gamemd-spawn.ex"], capture_output=True).returncode == 0:
            raise SystemExit("Yuri's Revenge is running; close it first.")
        if cmd == "install":
            # build everything first so a failure leaves the game dir untouched
            import tempfile
            with tempfile.TemporaryDirectory() as tmp:
                build(tmp)
                for f in MANAGED:
                    shutil.copy(os.path.join(tmp, f), os.path.join(GAME, f))
                    print("installed", f)
        else:
            for f in MANAGED:
                p = os.path.join(GAME, f)
                if os.path.exists(p):
                    os.remove(p)
                    print("removed", f)
            print("stock rules, art and strings will load")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
