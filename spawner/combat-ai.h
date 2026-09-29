/* AI behavior for the unmodified 1.001 engine. Uses the existing Unit::AI
 * wrapper and building-production hook; do not install a second hook on them.
 * Offsets/vtable slots: YRpp, verified against the installed executable. */
#include "combat-ai-policy.h"

#define COMBAT_SELECT_WEAPON 0x2E4
#define COMBAT_CLOSE_ENOUGH 0x3A8
#define COMBAT_SET_DESTINATION 0x480
#define COMBAT_STOP_MOVING 0x500
#define COMBAT_U_TYPE 0x6C4
#define COMBAT_DESTINATION 0x5A4
#define COMBAT_MISSION 0xAC

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

static struct {
    BYTE *house;
    int next_scan;
} combat_expansions[32];

static void combat_queue_expansion(BYTE *house)
{
    int idx = FIELD(house, 0x30, int), side = FIELD(house, OIL_H_SIDE, int);
    if (!oil_eligible(house) || idx < 0 || idx >= 32 || side < 0 || side > 2
        || FIELD(house, OIL_H_PRODUCING, int) != -1)
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
    unsigned choices = combat_choices((unsigned)*GAME_SEED, (unsigned)idx);
    int cash = FIELD(house, OIL_H_CASH, int);
    int power = FIELD(house, OIL_H_POWER, int) - FIELD(house, OIL_H_DRAIN, int);
    for (int role = 0; role < 4; role++) {
        if (role >= 2 && side != 0)
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
        } else if (!combat_expand(CURRENT_FRAME, FIELD(house, 0x2F0, int),
                     FIELD(house, OIL_H_REFINERIES, int), cash, power, cost, drain,
                     count, !!(choices & (role == 1 ? 1u : 2u)))) {
            continue;
        }
        if (((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) <= 0)
            continue;
        /* Native placement rejects inland yards and crowded/invalid terrain.
         * Probe before requesting production, so impossible yards don't stall it. */
        CellXY at;
        oil_place_original(house, &at, type, (void *)0x505F80, (DWORD)-1);
        if (at.X <= 0 || at.Y <= 0
            || !((char (GTHISCALL *)(BYTE *, CellXY *, BYTE *))BTYPE_CAN_PLACE)(type, &at, house))
            continue;
        FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
        logmsg("combat AI: house %d queued %s (choices %u)", idx, id, choices);
        return;
    }
}
