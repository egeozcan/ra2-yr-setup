/* AI behavior for the unmodified 1.001 engine. Uses the existing Unit::AI
 * wrapper and building-production hook; do not install a second hook on them.
 * Offsets/vtable slots: YRpp, verified against the installed executable. */
#include "combat-ai-policy.h"
#include "director-policy.h"

/* base defences and wall pieces: a pending one gives way to a wanted war factory */
#define COMBAT_DEFENSES "GAPILL,NASAM,ATESLA,GTGCAN,NALASR,NAFLAK,TESLA,NABNKR,YAGGUN,YAPSYT,NATBNK"
#define COMBAT_WALLS "GAWALL,NAWALL,YAWALL,GAFWLL"

#define COMBAT_SELECT_WEAPON 0x2E4
#define COMBAT_CLOSE_ENOUGH 0x3A8
#define COMBAT_SET_DESTINATION 0x480
#define COMBAT_STOP_MOVING 0x500
#define COMBAT_U_TYPE 0x6C4
#define COMBAT_DESTINATION 0x5A4
#define COMBAT_MISSION 0xAC
#define COMBAT_TEAM_ARRAY ((DynVec *)0x8B40E8)
#define COMBAT_AIRCRAFT_ARRAY ((DynVec *)0xA8E390)

static int combat_siege_candidate(BYTE *unit)
{
    BYTE *house = FIELD(unit, O_OWNER, BYTE *);
    if (!house || SESSION->GameMode != 5 || house[H_ISHUMAN] || house[0x1ED]
        || FIELD(house, OIL_H_DIFFICULTY, int) != 0 || !oil_live(unit))
        return 0;
    BYTE *type = FIELD(unit, COMBAT_U_TYPE, BYTE *);
    return type && in_list("SREF,V3,DRED,CARRIER,BSUB", (char *)type + T_ID);
}

static int combat_unit_exists(BYTE *unit)
{
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++)
        if (v->Items[i] == unit)
            return 1;
    return 0;
}

static void combat_siege_update(BYTE *unit)
{
    if (!combat_siege_candidate(unit)
        || !combat_siege_mission(FIELD(unit, COMBAT_MISSION, int)))
        return;
    BYTE *target = FIELD(unit, O_TARGET, BYTE *);
    if (!target || !FIELD(unit, COMBAT_DESTINATION, BYTE *))
        return;
    int weapon = ((int (GTHISCALL *)(BYTE *, BYTE *))VFUNC(unit, COMBAT_SELECT_WEAPON))(unit, target);
    if (weapon < 0 || !((char (GTHISCALL *)(BYTE *, BYTE *, int))
                       VFUNC(unit, COMBAT_CLOSE_ENOUGH))(unit, target, weapon))
        return;
    /* The engine checks range, minimum range, elevation and building footprint.
     * Retain the attack target and cancel only the march toward its centre. */
    ((void (GTHISCALL *)(BYTE *, BYTE *, char))VFUNC(unit, COMBAT_SET_DESTINATION))(unit, NULL, 1);
    ((char (GTHISCALL *)(BYTE *))VFUNC(unit, COMBAT_STOP_MOVING))(unit);
    ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(unit, VT_QUEUEMISSION))(unit, 1, 1);
    ((void (GTHISCALL *)(BYTE *, BYTE *))VFUNC(unit, VT_SETTARGET))(unit, target);
}

static int combat_building_count(BYTE *house, const char *ids)
{
    int count = 0;
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *b = v->Items[i];
        if (oil_live(b) && FIELD(b, O_OWNER, BYTE *) == house
            && in_list(ids, (char *)FIELD(b, B_TYPE, BYTE *) + T_ID))
            count++;
    }
    return count;
}

static int combat_is_ground_vehicle(BYTE *type)
{
    if (!type || in_list("SAPC,CARRIER,DEST,SUB,AEGIS,LCRF,DRED,SQD,DLPH,HYD,VLAD,CRUISE,TUG,CDEST,YHVR,BSUB",
                         (char *)type + T_ID))
        return 0;
    DynVec *units = UNITTYPE_ARRAY;
    if (!units->Items || units->Count < 0 || units->Count > 512)
        return 0;
    for (int i = 0; i < units->Count; i++)
        if (units->Items[i] == type)
            return 1;
    return 0;
}

static void combat_pending(BYTE *house, int *ground, int *planes)
{
    *ground = *planes = 0;
    DynVec *teams = COMBAT_TEAM_ARRAY;
    if (!teams->Items || teams->Count < 0 || teams->Count > 512)
        return;
    for (int i = 0; i < teams->Count; i++) {
        BYTE *team = teams->Items[i];
        if (!team || FIELD(team, 0x2C, BYTE *) != house)
            continue;
        BYTE *team_type = FIELD(team, 0x24, BYTE *);
        BYTE *force = team_type ? FIELD(team_type, 0xE4, BYTE *) : NULL;
        int entries = force ? FIELD(force, 0x9C, int) : 0;
        if (!force || entries < 0 || entries > 6)
            continue;
        for (int j = 0; j < entries; j++) {
            BYTE *entry = force + 0xA4 + j * 8;
            BYTE *member = FIELD(entry, 4, BYTE *);
            int missing = FIELD(entry, 0, int) - FIELD(team, 0x88 + j * 4, int);
            if (!member || missing <= 0 || missing > 32)
                continue;
            if (combat_is_ground_vehicle(member))
                *ground += missing;
            else if (in_list("ORCA,BEAG", (char *)member + T_ID))
                *planes += missing;
        }
    }
}

static int combat_owned_planes(BYTE *house)
{
    int count = 0;
    DynVec *aircraft = COMBAT_AIRCRAFT_ARRAY;
    if (!aircraft->Items || aircraft->Count < 0 || aircraft->Count > 512)
        return 0;
    for (int i = 0; i < aircraft->Count; i++) {
        BYTE *plane = aircraft->Items[i];
        if (oil_live(plane) && FIELD(plane, O_OWNER, BYTE *) == house) {
            BYTE *type = FIELD(plane, COMBAT_U_TYPE, BYTE *);
            if (type && in_list("ORCA,BEAG", (char *)type + T_ID))
                count++;
        }
    }
    return count;
}

static struct {
    BYTE *house;
    int next_scan;
} combat_expansions[32];

static void combat_queue_expansion(BYTE *house)
{
    int idx = FIELD(house, 0x30, int), side = FIELD(house, OIL_H_SIDE, int);
    /* The building queue holds one pick. A war factory may take the place of a pending defence or
     * wall: picks are checked every 900 frames, and a Yuri rush with 45000 unspent got its second
     * factory at frame 7000 behind a wall, gattling cannons and psychic towers (its opponent had
     * three factories by 6400). */
    int pending = FIELD(house, OIL_H_PRODUCING, int);
    DynVec *bts = BUILDINGTYPE_ARRAY;
    int displaceable = pending >= 0 && pending < bts->Count && (director_enabled(house) & DIR_F_ECONOMY)
        && in_list(COMBAT_DEFENSES "," COMBAT_WALLS, (char *)bts->Items[pending] + T_ID);
    if (!oil_eligible(house) || idx < 0 || idx >= 32 || side < 0 || side > 2 || (pending != -1 && !displaceable))
        return;
    if (combat_expansions[idx].house != house) {
        combat_expansions[idx].house = house;
        combat_expansions[idx].next_scan = 0;
    }
    if (CURRENT_FRAME < combat_expansions[idx].next_scan)
        return;
    combat_expansions[idx].next_scan = CURRENT_FRAME + 900;
    static const char *factories[] = { "GAWEAP", "NAWEAP", "YAWEAP" };
    static const char *yards[] = { "GAYARD", "NAYARD", "YAYARD" };
    const char *candidates[] = { yards[side], factories[side], "GAAIRC", "AMRADR" };
    int ground_pending, plane_pending;
    combat_pending(house, &ground_pending, &plane_pending);
    int owned_planes = side == 0 ? combat_owned_planes(house) : 0;
    int cash = FIELD(house, OIL_H_CASH, int);
    int power = FIELD(house, OIL_H_POWER, int) - FIELD(house, OIL_H_DRAIN, int);
    for (int role = 0; role < 4; role++) {
        if ((role >= 2 && side != 0) || (displaceable && role != 1))
            continue;
        const char *id = candidates[role];
        BYTE *type = find_type(BUILDINGTYPE_ARRAY, id);
        if (!type)
            continue;
        int count = combat_building_count(house, role >= 2 ? "GAAIRC,AMRADR" : id);
        int cost = ((int (GTHISCALL *)(BYTE *))VFUNC(type, 0xAC))(type); /* GetCost */
        int drain = FIELD(type, OIL_BT_DRAIN, int);
        if (role == 0) {
            if (CURRENT_FRAME < 900 || count || cash - cost < 3000 || power < drain + 50
                || !combat_building_count(house, factories[side])
                || !combat_building_count(house, "GAAIRC,AMRADR,NARADR,NAPSIS"))
                continue;
        } else if (role == 1 && (director_enabled(house) & DIR_F_ECONOMY)) {
            /* Director: production, not money, limits the army. Add factories as cash piles up. */
            if (count < 1 || count >= dir_wanted_factories(cash * 100 / dir_factory_cash_pct(house), CURRENT_FRAME,
                                                       FIELD(house, OIL_H_REFINERIES, int))
                || cash - cost < 2000 || power < drain + 50)
                continue;
        } else if (!combat_expand(CURRENT_FRAME, FIELD(house, 0x2F0, int),
                     FIELD(house, OIL_H_REFINERIES, int), cash, power, cost, drain,
                     count, role == 1 ? combat_need_factory(ground_pending)
                                      : combat_need_airbase(owned_planes, plane_pending))) {
            continue;
        }
        if (((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) <= 0)
            continue;
        /* Native placement rejects inland yards and crowded/invalid terrain.
         * Probe before requesting production, so impossible yards don't stall it. */
        CellXY at;
        oil_place_original(house, &at, type, (void *)0x505F80, (DWORD)-1);
        /* the stock planner found no room (cramped islands): the director places it where the
         * engine allows, near the base centre (dir_note_placement does the same at build time) */
        if ((at.X <= 0 || at.Y <= 0) && (director_enabled(house) & DIR_F_ECONOMY))
            dir_fallback_spot(house, type, &at);
        if (at.X <= 0 || at.Y <= 0
            || !((char (GTHISCALL *)(BYTE *, CellXY *, BYTE *))BTYPE_CAN_PLACE)(type, &at, house))
            continue;
        FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
        logmsg("combat AI: house %d queued %s (ground pending %d, planes %d+%d)%s%.24s",
               idx, id, ground_pending, owned_planes, plane_pending, displaceable ? " ahead of " : "",
               displaceable ? (char *)bts->Items[pending] + T_ID : "");
        return;
    }
}
