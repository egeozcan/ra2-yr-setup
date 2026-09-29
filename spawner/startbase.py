#!/usr/bin/env python3
"""Starting bases: the buildings a country starts with in place of its MCV, by tier, taken from the game's rules.

usage: startbase.py [TIER]    print every country's buildings at TIER (1-3, default all)

Tier 1 is barracks and an ore refinery, tier 2 adds the war factory and radar, tier 3 the Battle Lab. Each base also
has the Construction Yard, whatever those need (Prerequisite=, followed through), and enough power plants to run it.
The rules are rulesmd.ini as the game reads it: the loose file in the game directory (the mod's) or else the stock one
in expandmd01.mix. The [AI] Build* lists name the candidates in the order the AI picks from them; a country
gets the first one it may build (Owner=, RequiredHouses=, ForbiddenHouses=) that belongs to its side, meaning one
that needs its own Construction Yard. yspawn.dll puts the bases down (see yspawn.c, "starting bases").
"""
import functools, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spawn, mixextract

TIERS = ["MCV", "Tier 1", "Tier 2", "Tier 3"]
TIER_TEXT = ["Everyone starts with an MCV", "Construction Yard, power, barracks and ore refinery",
             "Tier 1 and a war factory and radar", "Tier 2 and a Battle Lab"]
GATES = [[], ["BuildBarracks", "BuildRefinery"], ["BuildWeapons", "BuildRadar"], ["BuildTech"]]   # added per tier
ORDER = ["BuildConst", "BuildPower", "BuildRefinery", "BuildBarracks", "BuildWeapons", "BuildRadar", "BuildTech"]
GENERIC = {"POWER": "PrerequisitePower", "PROC": "PrerequisiteProc", "RADAR": "PrerequisiteRadar",
           "TECH": "PrerequisiteTech", "BARRACKS": "PrerequisiteBarracks", "FACTORY": "PrerequisiteFactory"}


@functools.lru_cache(maxsize=1)
def rules():
    path = os.path.join(spawn.GAME, "rulesmd.ini")
    if os.path.exists(path):
        data = open(path, "rb").read()
    else:
        data = mixextract.extract(open(os.path.join(spawn.GAME, "expandmd01.mix"), "rb").read(), "rulesmd.ini")
    return spawn._ini_sections(data.decode("latin-1"))


def ids(section, key):
    return [x.strip() for x in section.get(key, "").split(",") if x.strip()]


def country_name(index):
    """rulesmd [Countries] ID for a yspawn.ini Country= number, such as Americans for 0."""
    return rules()["Countries"][str(index)]


def base(country, tier):
    """Building IDs for COUNTRY's base at TIER, in the order yspawn.dll places them: the Construction Yard first."""
    r = rules()
    g, ai = r["General"], r["AI"]
    has = lambda b, key: country.lower() in (x.lower() for x in ids(r.get(b, {}), key))
    allowed = lambda b: (has(b, "Owner") and (has(b, "RequiredHouses") or not ids(r.get(b, {}), "RequiredHouses"))
                         and not has(b, "ForbiddenHouses"))
    cy = next(b for b in ids(ai, "BuildConst") if allowed(b))
    first = lambda names: next((b for b in names if allowed(b) and cy in ids(r.get(b, {}), "Prerequisite")), None)
    plant = first(ids(ai, "BuildPower"))
    have, todo = [cy], [first(ids(ai, key)) for t in range(1, tier + 1) for key in GATES[t]]
    while todo:
        b = todo.pop(0)
        if b is None or b in have:
            continue
        have.append(b)
        for p in ids(r.get(b, {}), "Prerequisite"):
            if p.upper() == "POWER":
                continue   # the plants are counted below
            options = ids(g, GENERIC[p.upper()]) if p.upper() in GENERIC else [p]
            if not any(o in have or o in todo for o in options):
                todo.append(first(options) if p.upper() in GENERIC else p)
    power = lambda b: int(r.get(b, {}).get("Power", 0))
    need = -sum(min(power(b), 0) for b in have)
    plants = [plant] * max(1, math.ceil(need / power(plant))) if tier and plant else []
    rank = lambda b: next((i for i, key in enumerate(ORDER) if b in ids(ai, key)), len(ORDER))
    return [cy] + plants + sorted(have[1:], key=rank)


def tech_level(countries, tier):
    """The lowest Tech level setting that lets every one of COUNTRIES build all of its TIER base (the Construction
    Yard's TechLevel=-1 does not count: nobody builds that one)."""
    r = rules()
    return max([int(r.get(b, {}).get("TechLevel", 0)) for c in countries for b in base(c, tier)] + [1])


def section(countries, tier):
    """The [StartBase] section for yspawn.ini: the MCVs to take away, and each country's base."""
    out = {"Remove": ",".join(ids(rules()["General"], "BaseUnit"))}
    for c in countries:
        out[c] = ",".join(base(c, tier))
    return out


if __name__ == "__main__":
    tiers = [int(sys.argv[1])] if len(sys.argv) > 1 else range(1, len(TIERS))
    names = [country_name(i) for i in range(10)]
    for t in tiers:
        print(f"{TIERS[t]} (tech level {tech_level(names, t)}):")
        for c in names:
            print(f"  {c}: {', '.join(base(c, t))}")
