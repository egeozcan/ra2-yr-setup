/* Brutal skirmish oil fortifications. Included after yspawn's engine helpers.
 * Queue real buildings via ProducingBuildingTypeIndex, then redirect their
 * normal placement search. Factory AI still pays, builds and places them.
 * ABI and offsets: YRpp headers, verified against gamemd.exe 1.001. */
#include "oil-defense-policy.h"

#define OIL_BUILD_UPDATE 0x4FE3E0
#define OIL_FIND_BUILD_LOCATION 0x5060B0
#define OIL_TECHNO_ARRAY ((DynVec *)0xA8EC78)
#define OIL_BUILDING_ARRAY ((DynVec *)0xA8EB40)
#define OIL_H_DIFFICULTY 0x184
#define OIL_H_SIDE 0x1E8
#define OIL_H_DEFEATED 0x1F5
#define OIL_H_PRODUCING 0x564C
#define OIL_H_CASH 0x30C
#define OIL_H_POWER 0x53A4
#define OIL_H_DRAIN 0x53A8
#define OIL_H_REFINERIES 0x15C
#define OIL_H_CONYARDS 0x60
#define OIL_H_CAN_BUILD 0x4F7870
#define OIL_H_ALLIED 0x4F9A50
#define OIL_BT_DRAIN 0xEE4

typedef int (GTHISCALL *oil_build_fn)(BYTE *);
typedef CellXY *(GTHISCALL *oil_place_fn)(BYTE *, CellXY *, BYTE *, void *, DWORD);
static oil_build_fn oil_build_original;
static oil_place_fn oil_place_original;
static struct {
    BYTE *house, *oil, *type;
    int next_scan, next_build, previous_total;
} oil_orders[32];

static int oil_live(BYTE *obj)
{
    return obj && FIELD(obj, 0x6C, int) > 0 && obj[0x74] && !obj[0x81] && obj[0x90];
}

static int oil_eligible(BYTE *house)
{
    return house && oil_ai_active(SESSION->GameMode, house[H_ISHUMAN] || house[0x1ED],
        FIELD(house, OIL_H_DIFFICULTY, int), house[OIL_H_DEFEATED], house[H_PRODUCTION],
        FIELD(house, OIL_H_CONYARDS, int), FIELD(house, OIL_H_REFINERIES, int));
}

static int oil_owned(BYTE *oil, BYTE *house)
{
    /* Check membership before dereferencing saved object pointers after a death. */
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++)
        if (v->Items[i] == oil)
            return oil_live(oil) && FIELD(oil, O_OWNER, BYTE *) == house
                && !_stricmp((char *)FIELD(oil, B_TYPE, BYTE *) + T_ID, "CAOILD");
    return 0;
}

/* 1 ground, 2 air, 3 both. Count all stock defenses, including bunkers. */
static int oil_defense_kind(const char *id)
{
    if (in_list("GAPILL,ATESLA,GTGCAN,NALASR,TESLA,NABNKR,YAPSYT,NATBNK", id))
        return 1;
    if (in_list("NASAM,NAFLAK", id))
        return 2;
    return !_stricmp(id, "YAGGUN") ? 3 : 0;
}

static void oil_defense_counts(BYTE *house, CellXY at, int *ground, int *air, int *total)
{
    *ground = *air = *total = 0;
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *b = v->Items[i];
        if (!oil_live(b) || FIELD(b, O_OWNER, BYTE *) != house)
            continue;
        CellXY cell = object_cell(b);
        if (!oil_near(at.X, at.Y, cell.X, cell.Y, OIL_DEFENSE_RADIUS))
            continue;
        int kind = oil_defense_kind((char *)FIELD(b, B_TYPE, BYTE *) + T_ID);
        *ground += !!(kind & 1);
        *air += !!(kind & 2);
        *total += !!kind;
    }
}

static void oil_threats(BYTE *house, CellXY at, int *ground, int *air)
{
    *ground = *air = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *obj = v->Items[i], *owner;
        if (!oil_live(obj) || !(owner = FIELD(obj, O_OWNER, BYTE *))
            || owner[OIL_H_DEFEATED]
            || ((char (GTHISCALL *)(BYTE *, BYTE *))OIL_H_ALLIED)(house, owner))
            continue;
        int side = FIELD(owner, OIL_H_SIDE, int);
        if (side < 0 || side > 2)  /* civilians and Special are not hostile players */
            continue;
        CellXY cell = object_cell(obj);
        if (!oil_near(at.X, at.Y, cell.X, cell.Y, OIL_THREAT_RADIUS))
            continue;
        BYTE *weapon = ((BYTE *(GTHISCALL *)(BYTE *, int))VFUNC(obj, VT_GETWEAPON))(obj, 0);
        if (!weapon || !FIELD(weapon, 0, BYTE *))
            continue;  /* unarmed engineers, transports and miners */
        /* ObjectClass::GetHeight (vtable 0x1C8): Location.Z less the floor read a unit on a high
         * bridge's deck (416 up) as flying */
        int flying = ((int (GTHISCALL *)(BYTE *))VFUNC(obj, VT_WHATAMI))(obj) == 2
            || ((int (GTHISCALL *)(BYTE *))VFUNC(obj, 0x1C8))(obj) > 128;
        if (flying)
            (*air)++;
        else
            (*ground)++;
    }
}

/* A derrick anchors these AI defenses. Terrain/foundation checks still apply,
 * with a clear cell beside the derrick so engineers can reach its entrance. */
static int oil_spot(BYTE *type, BYTE *house, BYTE *oil, CellXY *out)
{
    CellXY at = object_cell(oil);
    int w = ((int (GTHISCALL *)(BYTE *))BTYPE_WIDTH)(type);
    int h = ((int (GTHISCALL *)(BYTE *, char))BTYPE_HEIGHT)(type, 1);
    BYTE *ot = FIELD(oil, B_TYPE, BYTE *);
    int ow = ((int (GTHISCALL *)(BYTE *))BTYPE_WIDTH)(ot);
    int oh = ((int (GTHISCALL *)(BYTE *, char))BTYPE_HEIGHT)(ot, 1);
    for (int r = 2; r <= 5; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;
                CellXY cell = { at.X + dx, at.Y + dy };
                if (!oil_near(at.X, at.Y, cell.X, cell.Y, OIL_DEFENSE_RADIUS))
                    continue;
                if (cell.X - 1 < at.X + ow && at.X < cell.X + w + 1
                    && cell.Y - 1 < at.Y + oh && at.Y < cell.Y + h + 1)
                    continue;
                if (((char (GTHISCALL *)(BYTE *, CellXY *, BYTE *))BTYPE_CAN_PLACE)(type, &cell, house)) {
                    *out = cell;
                    return 1;
                }
            }
    return 0;
}

/* TechnoTypeClass::Prerequisite (a list of building type indices at +0x638, as LoadFromINI fills it
 * at 0x714190; negative values are the [General] groups). HouseClass::CanBuild lets computer
 * players skip them: Apocalypse Tanks were picked at frame 2776, long before any Battle Lab. */
#define TT_PREREQUISITE 0x638
static int ai_owns_any(BYTE *house, const char *ids)
{
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *b = v->Items[i], *t;
        if (oil_live(b) && FIELD(b, O_OWNER, BYTE *) == house && (t = FIELD(b, B_TYPE, BYTE *)) && in_list(ids, (char *)t + T_ID))
            return 1;
    }
    return 0;
}

static int ai_has_prereqs(BYTE *house, BYTE *type)
{
    static const char *groups[7] = { "", "GAPOWR,NAPOWR,NANRCT,YAPOWR", "GAWEAP,NAWEAP,YAWEAP", "GAPILE,NAHAND,YABRCK",
                                     "GAAIRC,AMRADR,NARADR,NAPSIS", "GATECH,NATECH,YATECH", "GAREFN,NAREFN,YAREFN" };
    int count = FIELD(type, TT_PREREQUISITE + 0x10, int);
    int *items = FIELD(type, TT_PREREQUISITE + 4, int *);
    DynVec *bts = BUILDINGTYPE_ARRAY;
    for (int i = 0; i < count && items; i++) {
        int p = items[i];
        if (p >= 0 && p < bts->Count) {
            if (!ai_owns_any(house, (char *)bts->Items[p] + T_ID))
                return 0;
        } else if (p < 0 && p >= -6 && !ai_owns_any(house, groups[-p]))
            return 0;
    }
    return 1;
}

/* HouseClass::CanBuild, and the prerequisites it skips for computer players */
static int ai_can_build(BYTE *house, BYTE *type)
{
    return ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0
        && ai_has_prereqs(house, type);
}

static BYTE *oil_buildable(BYTE *house, const char *id, int cost)
{
    BYTE *type = find_type(BUILDINGTYPE_ARRAY, id);
    if (!type)
        return NULL;
    int can = ai_can_build(house, type);
    return oil_can_budget(FIELD(house, OIL_H_CASH, int), cost,
        FIELD(house, OIL_H_POWER, int), FIELD(house, OIL_H_DRAIN, int),
        FIELD(type, OIL_BT_DRAIN, int), can) ? type : NULL;
}

static int GFASTCALL oil_build_update(BYTE *house, void *unused)
{
    (void)unused;
    team_telemetry_sample(house);
    observer_reveal();
    dir_observer_report();
    bench_sample();
    dir_update(house);
    int idx = FIELD(house, 0x30, int);
    if (!oil_eligible(house) || idx < 0 || idx >= 32)
        return oil_build_original(house);
    if (oil_orders[idx].house != house) {
        memset(&oil_orders[idx], 0, sizeof oil_orders[idx]);
        oil_orders[idx].house = house;
    }
    if (oil_orders[idx].type) {
        if (!oil_owned(oil_orders[idx].oil, house))
            oil_orders[idx].type = NULL;
        else {
            int gd, ad, total;
            oil_defense_counts(house, object_cell(oil_orders[idx].oil), &gd, &ad, &total);
            if (total > oil_orders[idx].previous_total)
                oil_orders[idx].type = NULL;  /* completed: don't redirect subsequent stock orders */
        }
    }
    if (FIELD(house, OIL_H_PRODUCING, int) != -1)
        return oil_build_original(house);
    combat_queue_factory(house);   /* a wanted war factory takes the free queue first */
    if (FIELD(house, OIL_H_PRODUCING, int) != -1 || CURRENT_FRAME < oil_orders[idx].next_build
        || CURRENT_FRAME < oil_orders[idx].next_scan)
        return oil_build_original(house);
    oil_orders[idx].next_scan = CURRENT_FRAME + 90;

    static const char *light[] = { "GAPILL", "NALASR", "YAGGUN" };
    static const char *heavy[] = { "ATESLA", "TESLA", "YAPSYT" };
    static const char *aa[] = { "NASAM", "NAFLAK", "YAGGUN" };
    int side = FIELD(house, OIL_H_SIDE, int), best = 0;
    BYTE *best_oil = NULL, *best_type = NULL;
    if (side < 0 || side > 2)
        return oil_build_original(house);
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *oil = v->Items[i];
        if (!oil_live(oil) || FIELD(oil, O_OWNER, BYTE *) != house
            || _stricmp((char *)FIELD(oil, B_TYPE, BYTE *) + T_ID, "CAOILD"))
            continue;
        CellXY at = object_cell(oil), spot;
        int ground, air, gd, ad, total;
        oil_threats(house, at, &ground, &air);
        oil_defense_counts(house, at, &gd, &ad, &total);
        int need = oil_defense_need(ground, air, gd, ad, total);
        if (!need)
            continue;
        BYTE *type = need == OIL_AIR ? oil_buildable(house, aa[side], 1000) : NULL;
        if (need == OIL_GROUND && gd)
            type = oil_buildable(house, heavy[side], 1500);
        if (need == OIL_GROUND && !type)
            type = oil_buildable(house, light[side], side == 2 ? 1000 : 500);
        int score = (ground + air * 2) * 10 - total * 5;
        if (type && score > best && oil_spot(type, house, oil, &spot)) {
            best = score;
            best_oil = oil;
            best_type = type;
        }
    }
    if (best_type) {
        oil_orders[idx].oil = best_oil;
        oil_orders[idx].type = best_type;
        int gd, ad;
        oil_defense_counts(house, object_cell(best_oil), &gd, &ad, &oil_orders[idx].previous_total);
        oil_orders[idx].next_build = CURRENT_FRAME + OIL_BUILD_INTERVAL;
        FIELD(house, OIL_H_PRODUCING, int) = building_type_index(best_type);
        CellXY at = object_cell(best_oil);
        logmsg("oil defense: house %d queued %s for oil at %d,%d", idx,
            (char *)best_type + T_ID, at.X, at.Y);
    } else
        combat_queue_expansion(house);
    return oil_build_original(house);
}

static int dir_outpost_place(BYTE *house, BYTE *type, CellXY *out);   /* director.h */
static void dir_note_placement(BYTE *house, BYTE *type, CellXY *out);   /* director.h */
static void dir_check_passage(BYTE *house, BYTE *type, CellXY *out);   /* director.h */

static CellXY *GFASTCALL oil_find_location(BYTE *house, void *unused, CellXY *out,
                                          BYTE *type, void *callback, DWORD extra)
{
    (void)unused;
    if (dir_outpost_place(house, type, out))   /* the director's ore outpost by a captured building */
        return out;
    int idx = FIELD(house, 0x30, int);
    if (oil_eligible(house) && idx >= 0 && idx < 32 && oil_orders[idx].house == house
        && oil_orders[idx].type == type && oil_owned(oil_orders[idx].oil, house)) {
        int gd, ad, total;
        oil_defense_counts(house, object_cell(oil_orders[idx].oil), &gd, &ad, &total);
        if (total < OIL_DEFENSE_LIMIT && total <= oil_orders[idx].previous_total
            && oil_spot(type, house, oil_orders[idx].oil, out)) {
            logmsg("oil defense: house %d placing %s at %d,%d", idx,
                (char *)type + T_ID, out->X, out->Y);
            return out;
        }
        oil_orders[idx].type = NULL;
    }
    /* Lost oil or blocked terrain: finish at a normal base location. */
    CellXY *r = oil_place_original(house, out, type, callback, extra);
    dir_check_passage(house, type, out);   /* a spot that cuts a passage: another one */
    dir_note_placement(house, type, out);
    return r;
}

/* Nonzero once installed. The director, benchmark rows and exit, RevealMap and team telemetry all run from
 * oil_build_update: without it they are off. */
static int patch_oil_defenses(void)
{
    const BYTE build[] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x8B, 0xE9 };
    const BYTE place[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
    if (!patch_checked("oil building production", OIL_BUILD_UPDATE, build, sizeof build)
        || !patch_checked("oil building placement", OIL_FIND_BUILD_LOCATION, place, sizeof place)) {
        logmsg("oil defense: not patched; the director, benchmark, RevealMap and team telemetry are off");
        return 0;
    }
    BYTE *tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        logmsg("oil defense: could not allocate trampolines, not patched; the director, benchmark, RevealMap and "
               "team telemetry are off");
        return 0;
    }
    memcpy(tramp, build, sizeof build);
    patch_rel((DWORD)tramp + sizeof build, 0xE9, OIL_BUILD_UPDATE + sizeof build);
    memcpy(tramp + 16, place, sizeof place);
    patch_rel((DWORD)tramp + 16 + sizeof place, 0xE9, OIL_FIND_BUILD_LOCATION + sizeof place);
    oil_build_original = (oil_build_fn)tramp;
    oil_place_original = (oil_place_fn)(tramp + 16);
    patch(OIL_BUILD_UPDATE, (const BYTE[]){ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, sizeof build);
    patch_rel(OIL_BUILD_UPDATE, 0xE9, (DWORD)oil_build_update);
    patch(OIL_FIND_BUILD_LOCATION, (const BYTE[]){ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, sizeof place);
    patch_rel(OIL_FIND_BUILD_LOCATION, 0xE9, (DWORD)oil_find_location);
    logmsg("oil defense: production and placement patches applied (Brutal skirmish only)");
    return 1;
}
