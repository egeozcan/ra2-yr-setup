/* Strategy director for Brutal skirmish AI (custom launcher only).
 *
 * Stock AI builds units only to fill the task forces its triggers pick, so a Brutal house can sit on
 * tens of thousands of credits with one factory idle. The director:
 *  - production: when the stock unit/infantry pickers leave the queue empty, orders a unit whose role
 *    (main, anti-air, siege, anti-infantry) is furthest below a mix derived from the enemy's forces;
 *    combat-ai.h adds war factories when cash piles up;
 *  - army: pools the house's team-less combat units, gathers them at a rally point toward the enemy,
 *    launches when the pool can win, attacks the nearest enemy structures with focus fire, returns
 *    to defend the base against raids, and retreats a beaten attack.
 * Teams created by triggers keep their own scripts; units only join the pool once they have no team.
 * Offsets: YRpp field order, anchored as noted; vtable slots checked against gamemd.exe 1.001. */
#include "director-policy.h"

#define DIR_UNIT_PRODUCTION 0x4FEA60   /* HouseClass: pick ProducingUnitTypeIndex from team demand */
#define DIR_INF_PRODUCTION 0x4FEEE0    /* HouseClass: same for ProducingInfantryTypeIndex */
#define H_PRODUCING_UNIT 0x5650
#define H_PRODUCING_INF 0x5654
#define INFANTRYTYPE_ARRAY ((DynVec *)0xA8E348)
#define F_TEAM 0x5D4                   /* FootClass::Team (Destination 0x5A4 + 0x30) */
#define O_HEALTH 0x6C
#define VT_GETTYPE 0x88                /* ObjectClass::GetType; ObjectTypeClass::Strength at +0xA0 */
#define OT_STRENGTH 0xA0
#define VT_TYPE_COST 0xAC              /* TechnoTypeClass cost, as used by combat-ai.h */
#define VT_CLICKEDMISSION 0x378        /* TechnoClass::ClickedMission(Mission, target, cell, cell) */
#define VT_FIREERROR 0x3C0             /* TechnoClass::GetFireError(target, weapon, ignore range) */
#define MAP_CELLS (FIELD(MAP_INSTANCE, 0x13C, BYTE **))
#define MISSION_ATTACK 1
#define MISSION_MOVE 2
#define MISSION_AREA_GUARD 11
#define T_FOCUS 0x218                  /* TechnoClass::Focus: Area_Guard guards around it when it is a cell */
#define MAX_POOL 160
#define MAP_ZONE 0x56D230          /* MapClass::GetMovementZoneType(cell, MovementZone, bridge) */
#define T_PASSENGERS 0x114         /* TechnoClass::Passengers.NumPassengers */
#define MAX_ENEMY 1024

typedef int (GTHISCALL *dir_prod_fn)(BYTE *);
static const char *dir_mcvs = "AMCV,SMCV,PCV";
static const char *dir_transports[3] = { "LCRF", "SAPC", "YHVR" };   /* amphibious ferries */
static dir_prod_fn dir_unit_original, dir_inf_original;

typedef struct {
    BYTE *house;
    int state, next_think, launch_value, launch_frame, last_log;
    int army_value, army_count, enemy_army, target_army, local_enemy, local_ours, threat_value;
    int enemy_air, enemy_inf, enemy_armor, enemy_def;
    int unit_request, unit_request_frame;
    int blocked, blocked_frame, best_dist, progress_frame, state_frame;   /* ground army cannot reach the enemy: island map */
    int reached;                              /* this attack got within 15 cells of an objective */
    BYTE *unreachable[8];                     /* objectives the army made no progress toward */
    int unreachable_count;
    CellXY base, rally, threat_at, objective_at, centroid, front;
    BYTE *objective, *enemy, *rally_enemy;
    BYTE *repair_hut, *repair_engineer;       /* bridge repair job */
    int repair_frame, repair_mode, want_engineer, failed_frame;
    BYTE *failed_job;   /* mode 0 bridge, 1 capture */
    int idle_harvesters, next_economy;
    int want_mcv, next_site;                  /* expansion: build an MCV and deploy it by fresh ore */
    CellXY site;
    int island, next_island, ferry_state, ferry_frame, home_zone;   /* enemy only reachable by water: ferry troops */
    BYTE *ferry;
    CellXY landing, dock;
    int rally_frame;
} DirState;
static DirState dir_state[32];

static int dir_active(BYTE *house)
{
    return house && SESSION->GameMode == 5 && !house[H_ISHUMAN] && !house[0x1ED]
        && FIELD(house, OIL_H_DIFFICULTY, int) == 0 && !house[OIL_H_DEFEATED] && director_enabled(house);
}

static DirState *dir_get(BYTE *house)
{
    int idx = FIELD(house, 0x30, int);
    if (idx < 0 || idx >= 32)
        return NULL;
    DirState *d = &dir_state[idx];
    if (d->house != house) {
        memset(d, 0, sizeof *d);
        d->house = house;
        d->unit_request = -1;
    }
    return d;
}

static int dir_whatami(BYTE *obj)
{
    return ((int (GTHISCALL *)(BYTE *))VFUNC(obj, VT_WHATAMI))(obj);
}

static BYTE *dir_type(BYTE *obj)
{
    return ((BYTE *(GTHISCALL *)(BYTE *))VFUNC(obj, VT_GETTYPE))(obj);
}

static int dir_cost(BYTE *type)
{
    return ((int (GTHISCALL *)(BYTE *))VFUNC(type, VT_TYPE_COST))(type);
}

static int dir_armed(BYTE *obj)
{
    BYTE **w = ((BYTE **(GTHISCALL *)(BYTE *, int))VFUNC(obj, VT_GETWEAPON))(obj, 0);
    return w && *w;
}

static int dir_dist2(CellXY a, CellXY b)
{
    int dx = a.X - b.X, dy = a.Y - b.Y;
    return dx * dx + dy * dy;
}

static BYTE *dir_cell(CellXY c)
{
    if (c.X <= 0 || c.Y <= 0 || c.X >= 512 || c.Y >= 512)
        return NULL;
    BYTE **cells = MAP_CELLS;
    return cells ? cells[c.Y * 512 + c.X] : NULL;
}

static int dir_hostile(BYTE *house, BYTE *owner)
{
    if (!owner || owner == house || owner[OIL_H_DEFEATED])
        return 0;
    int side = FIELD(owner, OIL_H_SIDE, int);
    if (side < 0 || side > 2 || (human_in_peace && owner[H_ISHUMAN]))
        return 0;
    return !((char (GTHISCALL *)(BYTE *, BYTE *))OIL_H_ALLIED)(house, owner);
}

static int dir_crosses(BYTE *obj)
{
    BYTE *type = dir_type(obj);
    return type && in_list("ROBO,ZEP,SCHP,DISK", (char *)type + T_ID);
}

/* Attack teams hand their members to the director once their script has started. Guards stay:
 * base-defense teams, the oil guard/patrol/capture teams (0F1BC...) and the anti-air base guard. */
#define TEAM_LIBERATE 0x6EA870         /* TeamClass::LiberateMember(FootClass*, int, bool) */
#define TT_BASE_DEFENSE 0xF6           /* TeamTypeClass::IsBaseDefense (TaskForce 0xE4 + 0x12) */
#define TT_SCRIPT 0xE0

static int dir_take_from_team(BYTE *obj)
{
    BYTE *team = FIELD(obj, F_TEAM, BYTE *);
    if (!team)
        return 1;
    if (!(director_enabled(FIELD(obj, O_OWNER, BYTE *)) & DIR_F_TAKEOVER))
        return 0;
    BYTE *type = FIELD(team, TEAM_TYPE, BYTE *), *script = FIELD(team, TEAM_SCRIPT, BYTE *);
    if (!type || !script || FIELD(script, SCRIPT_LINE, int) < 0 || type[TT_BASE_DEFENSE])
        return 0;
    BYTE *script_type = FIELD(type, TT_SCRIPT, BYTE *);
    const char *id = script_type ? (char *)script_type + T_ID : "";
    if (!_strnicmp(id, "0F1BC", 5) || !_stricmp(id, "0F1BF007-G"))
        return 0;
    ((char (GTHISCALL *)(BYTE *, BYTE *, int, char))TEAM_LIBERATE)(team, obj, -1, 0);
    return FIELD(obj, F_TEAM, BYTE *) == NULL;
}

/* Units the director may command: armed ground combat units, not harvesters, builders, infiltrators
 * or boats, and never while a trigger team owns them. */
static int dir_poolable(BYTE *obj, int what)
{
    if (what != 1 && what != 15)
        return 0;
    if (!dir_armed(obj))
        return 0;
    BYTE *type = dir_type(obj);
    if (!type || in_list("HARV,CMIN,SMIN,AMCV,SMCV,PCV,SBDOZR,ENGINEER,SENGINEER,YENGINEER,SPY,"
                         "IVAN,CIVAN,TERROR,SHAD,JUMPJET,CCOMAND,TANY,BORIS,GHOST,YURIPR,"
                         "CARRIER,DEST,SUB,AEGIS,LCRF,DRED,SQD,DLPH,HYD,BSUB,SAPC,YHVR,VIRUS,DTRUCK",
                         (char *)type + T_ID))
        return 0;
    int mission = FIELD(obj, COMBAT_MISSION, int);
    /* 7 Enter, 8 Capture, 16 Unload, 10 Harvest: busy with something the AI chose deliberately */
    return mission != 7 && mission != 8 && mission != 10 && mission != 16 && dir_take_from_team(obj);
}

/* ---- production ---- */

static const char *dir_vehicle_roles[3][ROLE_COUNT] = {
    { "ATTNK,TNKD,MGTK,MTNK", "FV", "SREF", "MGTK,FV" },
    { "APOC,TTNK,HTNK", "HTK", "V3", "HTK" },
    { "MIND,LTNK", "YTNK", "TELE", "YTNK" },
};
/* Enemy out of reach by land: Robot Tanks hover over water, Kirovs, Siege Choppers and Discs fly. */
static const char *dir_blocked_main[3] = { "ROBO", "ZEP,SCHP", "DISK" };
static const char *dir_infantry[3] = { "GGI,E1", "SHK,E2", "BRUTE,INIT" };

static int dir_type_index(DynVec *v, BYTE *type)
{
    for (int i = 0; i < v->Count; i++)
        if (v->Items[i] == type)
            return i;
    return -1;
}

static int dir_owned_of(BYTE *house, BYTE *type)
{
    int n = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i];
        if (oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_type(o) == type)
            n++;
    }
    return n;
}

/* First type in the list the house can build now, within budget and caps. */
static BYTE *dir_first_buildable(BYTE *house, DynVec *types, const char *list, int reserve)
{
    char id[32];
    const char *p = list;
    while (*p) {
        size_t n = strcspn(p, ",");
        if (n && n < sizeof id) {
            memcpy(id, p, n);
            id[n] = 0;
            BYTE *type = find_type(types, id);
            /* CanBuild accepts a type whose factory is not built yet; such a request would sit in
             * the queue and block every other vehicle. */
            int naval = in_list("SAPC,LCRF,YHVR,DEST,AEGIS,CARRIER,DLPH,SUB,DRED,HYD,SQD,BSUB", id);
            if (type && types == UNITTYPE_ARRAY
                && !combat_building_count(house, naval ? "GAYARD,NAYARD,YAYARD" : "GAWEAP,NAWEAP,YAWEAP"))
                type = NULL;
            if (type && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0
                && dir_can_spend(FIELD(house, OIL_H_CASH, int), dir_cost(type), reserve)
                && (_stricmp(id, "MIND") || dir_owned_of(house, type) < 3)
                && (_stricmp(id, "ATTNK") || dir_owned_of(house, type) < 4))   /* slow: a few, not the army */
                return type;
        }
        p += n + (p[n] == ',');
    }
    return NULL;
}

static int dir_role_of(int side, const char *id)
{
    for (int r = ROLE_COUNT - 1; r >= 0; r--)
        if (in_list(dir_vehicle_roles[side][r], id) && r != ROLE_SUPPORT)
            return r;
    return in_list(dir_vehicle_roles[side][ROLE_SUPPORT], id) ? ROLE_SUPPORT : ROLE_MAIN;
}

static void dir_choose_vehicle(BYTE *house, DirState *d)
{
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2)
        return;
    int shares[ROLE_COUNT], have[ROLE_COUNT] = { 0 }, available[ROLE_COUNT];
    BYTE *pick[ROLE_COUNT];
    dir_role_shares(d->enemy_air, d->enemy_inf, d->enemy_armor, d->enemy_def, shares);
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i];
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 1)
            continue;
        BYTE *type = dir_type(o);
        if (type && dir_armed(o))
            have[dir_role_of(side, (char *)type + T_ID)] += dir_cost(type);
    }
    for (int r = 0; r < ROLE_COUNT; r++) {
        pick[r] = dir_first_buildable(house, UNITTYPE_ARRAY, (d->blocked || d->island) && r == ROLE_MAIN
                                      ? dir_blocked_main[side] : dir_vehicle_roles[side][r], 2500);
        available[r] = pick[r] != NULL;
    }
    int role = dir_pick_role(shares, have, available);
    if (role < 0)
        return;
    int index = dir_type_index(UNITTYPE_ARRAY, pick[role]);
    if (index < 0)
        return;
    FIELD(house, H_PRODUCING_UNIT, int) = index;
    d->unit_request = index;
    d->unit_request_frame = CURRENT_FRAME;
}

static int GFASTCALL dir_unit_production(BYTE *house, void *unused)
{
    (void)unused;
    int result = dir_unit_original(house);
    if (!dir_active(house) || !(director_enabled(house) & DIR_F_PRODUCTION))
        return result;
    DirState *d = dir_get(house);
    if (!d)
        return result;
    int current = FIELD(house, H_PRODUCING_UNIT, int);
    /* A request the factory never picked up would block the queue; drop it after a while. */
    if (current != -1 && current == d->unit_request && CURRENT_FRAME - d->unit_request_frame > 3000) {
        FIELD(house, H_PRODUCING_UNIT, int) = -1;
        current = -1;
    }
    /* One-off orders outrank the stock team picks, which otherwise never leave the queue free. */
    int side0 = FIELD(house, OIL_H_SIDE, int);
    BYTE *urgent = d->want_mcv ? dir_first_buildable(house, UNITTYPE_ARRAY, dir_mcvs, 0)
        : (d->island || d->blocked) && !d->ferry && side0 >= 0 && side0 <= 2
        ? dir_first_buildable(house, UNITTYPE_ARRAY, dir_transports[side0], 1000) : NULL;
    if (urgent && current != -1 && current != d->unit_request)
        current = -1;   /* only when the one-off unit can actually be built now */
    if (current == -1 && d->want_mcv) {
        BYTE *type = dir_first_buildable(house, UNITTYPE_ARRAY, dir_mcvs, 0);
        int index = type ? dir_type_index(UNITTYPE_ARRAY, type) : -1;
        if (index >= 0) {
            FIELD(house, H_PRODUCING_UNIT, int) = current = d->unit_request = index;
            d->unit_request_frame = CURRENT_FRAME;
            d->want_mcv = 0;
        }
    }
    int side = FIELD(house, OIL_H_SIDE, int);
    if (current == -1 && (d->island || d->blocked) && !d->ferry && side >= 0 && side <= 2) {
        BYTE *type = dir_first_buildable(house, UNITTYPE_ARRAY, dir_transports[side], 1000);
        int index = type ? dir_type_index(UNITTYPE_ARRAY, type) : -1;
        if (index >= 0) {
            FIELD(house, H_PRODUCING_UNIT, int) = current = d->unit_request = index;
            d->unit_request_frame = CURRENT_FRAME;
        }
    }
    if (current == -1 && !d->want_mcv)   /* saving up for the expansion MCV */
        dir_choose_vehicle(house, d);
    return result;
}

static int GFASTCALL dir_inf_production(BYTE *house, void *unused)
{
    (void)unused;
    int result = dir_inf_original(house);
    if (!dir_active(house) || !(director_enabled(house) & DIR_F_PRODUCTION) || FIELD(house, H_PRODUCING_INF, int) != -1
        || (FIELD(house, H_OWNED_INFANTRY, int) >= 30 && !(dir_get(house) && dir_get(house)->want_engineer)))
        return result;
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2)
        return result;
    DirState *d = dir_get(house);
    static const char *engineers[3] = { "ENGINEER", "SENGINEER", "YENGINEER" };
    BYTE *type = d && d->want_engineer ? dir_first_buildable(house, INFANTRYTYPE_ARRAY, engineers[side], 0) : NULL;
    if (!type)
        type = dir_first_buildable(house, INFANTRYTYPE_ARRAY, dir_infantry[side], 4000);
    int index = type ? dir_type_index(INFANTRYTYPE_ARRAY, type) : -1;
    if (index >= 0)
        FIELD(house, H_PRODUCING_INF, int) = index;
    return result;
}

/* ---- army ---- */

typedef struct {
    BYTE *obj;
    CellXY at;
    int value, armed, building, air, infantry, focus, capturable, range;
} DirEnemy;
static DirEnemy dir_enemies[MAX_ENEMY];
static int dir_enemy_count;

static int dir_weapon_cells(BYTE *unit)
{
    BYTE **w = ((BYTE **(GTHISCALL *)(BYTE *, int))VFUNC(unit, VT_GETWEAPON))(unit, 0);
    int range = w && *w ? FIELD(*w, W_RANGE, int) : 0;
    return range < 0 ? 20 : range / 256;
}

/* Our structures, collected once per scan: base threats are enemies close to one of them, not
 * anything within a radius of the base centre (which reaches across rivers and lakes). */
static CellXY dir_own_buildings[256];
static int dir_own_building_count;

static int dir_near_own_building(BYTE *house, CellXY at, int r)
{
    (void)house;
    for (int i = 0; i < dir_own_building_count; i++)
        if (dir_dist2(dir_own_buildings[i], at) <= r * r)
            return 1;
    return 0;
}

static void dir_scan_enemies(BYTE *house, DirState *d)
{
    dir_own_building_count = 0;
    DynVec *bv = OIL_BUILDING_ARRAY;
    for (int i = 0; i < bv->Count && dir_own_building_count < 256; i++) {
        BYTE *b = bv->Items[i];
        if (oil_live(b) && FIELD(b, O_OWNER, BYTE *) == house)
            dir_own_buildings[dir_own_building_count++] = object_cell(b);
    }
    dir_enemy_count = 0;
    d->enemy_army = d->enemy_air = d->enemy_inf = d->enemy_armor = d->enemy_def = d->target_army = 0;
    d->threat_value = 0;
    int tx = 0, ty = 0, tn = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count && dir_enemy_count < MAX_ENEMY; i++) {
        BYTE *o = v->Items[i];
        if (!oil_live(o) || !dir_hostile(house, FIELD(o, O_OWNER, BYTE *)))
            continue;
        int what = dir_whatami(o);
        BYTE *type = dir_type(o);
        if (!type)
            continue;
        DirEnemy *e = &dir_enemies[dir_enemy_count++];
        e->obj = o;
        e->at = object_cell(o);
        e->value = dir_cost(type);
        e->armed = dir_armed(o);
        e->range = e->armed ? dir_weapon_cells(o) : 0;
        e->building = what == 6;
        /* tech buildings are worth more captured: derricks pay forever, the others give units/repair */
        e->capturable = e->building && in_list("CAOILD,CAAIRP,CATHOSP,CAOUTP,CAMACH,CAPOWR", (char *)type + T_ID);
        e->infantry = what == 15;
        e->focus = 0;
        Coord c = FIELD(o, O_LOCATION, Coord);
        int floor = ((int (GTHISCALL *)(void *, Coord *))MAP_FLOOR_HEIGHT)(MAP_INSTANCE, &c);
        e->air = what == 2 || c.Z > floor + 128;
        if (!e->armed)
            continue;
        if (e->building) {
            if (FIELD(o, O_OWNER, BYTE *) == d->enemy)   /* the target's defenses, for launch and siege share */
                d->enemy_def += e->value;
            continue;
        }
        d->enemy_army += e->value;
        if (FIELD(o, O_OWNER, BYTE *) == d->enemy)
            d->target_army += e->value;
        if (e->air)
            d->enemy_air += e->value;
        else if (e->infantry)
            d->enemy_inf += e->value;
        else
            d->enemy_armor += e->value;
        if (!e->air && dir_near_own_building(house, e->at, 10)) {
            d->threat_value += e->value;
            tx += e->at.X;
            ty += e->at.Y;
            tn++;
        }
    }
    if (tn)
        d->threat_at = (CellXY){ (short)(tx / tn), (short)(ty / tn) };
}

static CellXY dir_house_center(BYTE *house)
{
    CellXY c = FIELD(house, H_BASE_CENTER, CellXY);
    if (c.X <= 0 || c.Y <= 0)
        c = FIELD(house, H_BASESPAWNCELL, CellXY);
    return c;
}

/* A player still in the game: structures, or units such as an undeployed MCV. */
static int dir_house_alive(BYTE *h)
{
    return FIELD(h, H_OWNED_BUILDINGS, int) || FIELD(h, H_OWNED_UNITS, int) || FIELD(h, H_OWNED_INFANTRY, int);
}

/* The nearest hostile player house to our base. */
static BYTE *dir_pick_enemy(BYTE *house, CellXY base)
{
    DynVec *v = HOUSE_ARRAY;
    BYTE *best = NULL;
    int best_d = 0x7FFFFFFF;
    for (int i = 0; i < v->Count; i++) {
        BYTE *h = v->Items[i];
        if (!dir_hostile(house, h) || !dir_house_alive(h))
            continue;
        int dd = dir_dist2(base, dir_house_center(h));
        if (dd < best_d) {
            best_d = dd;
            best = h;
        }
    }
    return best;
}

/* The enemy structure closest to the army: attack the edge of the base first. */
static BYTE *dir_pick_objective(DirState *d, CellXY from)
{
    BYTE *best = NULL;
    int best_score = 0x7FFFFFFF, buildings = 0;
    for (int i = 0; i < dir_enemy_count; i++)
        buildings += dir_enemies[i].building && !dir_enemies[i].capturable
                     && FIELD(dir_enemies[i].obj, O_OWNER, BYTE *) == d->enemy;
    for (int i = 0; i < dir_enemy_count; i++) {
        DirEnemy *e = &dir_enemies[i];
        /* structures first; with none left, whatever keeps the player alive (an MCV, a last unit) */
        if ((buildings && (!e->building || e->capturable)) || e->air || FIELD(e->obj, O_OWNER, BYTE *) != d->enemy)
            continue;
        int skip = 0;
        for (int k = 0; k < d->unreachable_count; k++)
            skip |= d->unreachable[k] == e->obj;
        if (skip)
            continue;
        /* defenses at the edge first: an army that walks past them to a building deeper in
         * gets shot all the way there */
        int score = e->armed || !(director_enabled(d->house) & DIR_F_DEFENSES_FIRST) ? dir_dist2(from, e->at)
                                                                                : dir_dist2(from, e->at) * 3 / 2 + 8 * 8;
        if (score < best_score) {
            best_score = score;
            best = e->obj;
            d->objective_at = e->at;
        }
    }
    return best;
}

static int dir_can_fire_at(BYTE *unit, BYTE *target)
{
    int weapon = ((int (GTHISCALL *)(BYTE *, BYTE *))VFUNC(unit, COMBAT_SELECT_WEAPON))(unit, target);
    if (weapon < 0)
        return 0;
    int err = ((int (GTHISCALL *)(BYTE *, BYTE *, int, char))VFUNC(unit, VT_FIREERROR))(unit, target, weapon, 1);
    return err != 5 && err != 6;   /* ILLEGAL, CANT */
}


/* Orders the director gave, so a unit is not re-ordered (and re-pathed) every tick. */
#define DIR_ORDERS 4096
static struct { BYTE *unit, *what; int frame; } dir_orders[DIR_ORDERS];

static int dir_recent_order(BYTE *unit, BYTE *what, int hold)
{
    unsigned h = ((DWORD)unit >> 3) % DIR_ORDERS;
    for (int i = 0; i < 16; i++) {
        unsigned k = (h + i) % DIR_ORDERS;
        if (dir_orders[k].unit == unit) {
            if (dir_orders[k].what == what && CURRENT_FRAME - dir_orders[k].frame < hold)
                return 1;
            dir_orders[k].what = what;
            dir_orders[k].frame = CURRENT_FRAME;
            return 0;
        }
    }
    for (int i = 0; i < 16; i++) {
        unsigned k = (h + i) % DIR_ORDERS;
        if (!dir_orders[k].unit || CURRENT_FRAME - dir_orders[k].frame > 3000) {
            dir_orders[k].unit = unit;
            dir_orders[k].what = what;
            dir_orders[k].frame = CURRENT_FRAME;
            return 0;
        }
    }
    unsigned k = h;   /* table crowded: overwrite */
    dir_orders[k].unit = unit;
    dir_orders[k].what = what;
    dir_orders[k].frame = CURRENT_FRAME;
    return 0;
}

static void dir_order(BYTE *unit, int mission, BYTE *target, BYTE *cell)
{
    ((char (GTHISCALL *)(BYTE *, int, BYTE *, BYTE *, BYTE *))VFUNC(unit, VT_CLICKEDMISSION))(
        unit, mission, target, cell, NULL);
}

/* Attack-move: fight whatever is worth fighting near the unit, else keep heading for the goal. */
static int dir_flags;   /* the commanding house's DirectorFlags, set by dir_army */

static void dir_command(BYTE *unit, CellXY goal, BYTE *goal_obj, int engage)
{
    CellXY at = object_cell(unit);
    BYTE *current = FIELD(unit, O_TARGET, BYTE *);
    int reach = dir_weapon_cells(unit) + 3, best_score = -1000000;
    DirEnemy *best = NULL, *keep = NULL;
    if (engage) {
        for (int i = 0; i < dir_enemy_count; i++) {
            DirEnemy *e = &dir_enemies[i];
            int dd = dir_dist2(at, e->at);
            /* in our reach, or already able to shoot us: never walk through fire without answering */
            int answer = (dir_flags & DIR_F_ANSWER) && e->range + 1 > reach ? e->range + 1 : reach;
            if (dd > answer * answer || (dd > reach * reach && !e->armed))
                continue;
            if (e->obj == current && !e->capturable)
                keep = e;
            int priority = e->armed ? (e->building ? 70 : 100) : (e->building ? 25 : 45);
            int strength = FIELD(dir_type(e->obj), OT_STRENGTH, int);
            int hurt = strength > 0 ? 40 - 40 * FIELD(e->obj, O_HEALTH, int) / strength : 0;
            int score = priority + hurt + (e->focus < 5 ? e->focus * 8 : -40) - dd / 4;
            if (e->capturable)
                continue;   /* an engineer will take it; don't shoot what we want to own */
            if (score > best_score && dir_can_fire_at(unit, e->obj)) {
                best_score = score;
                best = e;
            }
        }
    }
    /* Stay on a live armed target; switch from buildings or harmless targets to a real threat. */
    if (keep && ((keep->armed && !keep->building) || !best || !best->armed || best->building)) {
        keep->focus++;
        return;
    }
    if (best) {
        best->focus++;
        if (!dir_recent_order(unit, best->obj, 90))
            dir_order(unit, MISSION_ATTACK, best->obj, NULL);
        return;
    }
    /* Marching on an enemy structure: attack it outright. The engine paths to firing range, which
     * works where a move to the structure's own (occupied) cell or a long detour silently fails. */
    if (goal_obj) {
        if (current == goal_obj && FIELD(unit, COMBAT_MISSION, int) == MISSION_ATTACK)
            return;
        if (!dir_recent_order(unit, goal_obj, 300))
            dir_order(unit, MISSION_ATTACK, goal_obj, NULL);
        return;
    }
    BYTE *cell = dir_cell(goal);
    if (!cell)
        return;
    /* Area guard around a cell is the engine's own attack-move: the unit heads for its Focus and
     * fights what it meets there. A plain move order gets reset by the AI's idle-unit handling. */
    if (FIELD(unit, T_FOCUS, BYTE *) == cell && FIELD(unit, COMBAT_MISSION, int) == MISSION_AREA_GUARD)
        return;
    FIELD(unit, T_FOCUS, BYTE *) = cell;
    ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(unit, VT_QUEUEMISSION))(unit, MISSION_AREA_GUARD, 1);
}

/* A unit the ferry has put across: nearer the enemy's base than to ours. */
static int dir_landed(DirState *d, CellXY c)
{
    return d->enemy && dir_dist2(c, FIELD(d->enemy, H_BASE_CENTER, CellXY)) < dir_dist2(c, d->base);
}

static void dir_army(BYTE *house, DirState *d)
{
    dir_flags = director_enabled(house);
    BYTE *pool[MAX_POOL];
    int n = 0, value = 0, sx = 0, sy = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count && n < MAX_POOL; i++) {
        BYTE *o = v->Items[i];
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || !dir_poolable(o, dir_whatami(o)))
            continue;
        if ((d->blocked || d->island) && !dir_crosses(o)) {
            /* on an island map only hover/air units and troops already ferried across attack */
            CellXY c = object_cell(o);
            if (d->threat_value < 1500 && !dir_landed(d, c))
                continue;   /* ...but everyone defends the base */
        }
        pool[n++] = o;
        value += dir_cost(dir_type(o));
        CellXY c = object_cell(o);
        sx += c.X;
        sy += c.Y;
    }
    d->army_value = value;
    d->army_count = n;
    if (n)
        d->centroid = (CellXY){ (short)(sx / n), (short)(sy / n) };
    /* The front: the pooled unit closest to the enemy base. Objectives are picked from there. */
    d->front = d->centroid;
    if (d->enemy) {
        CellXY target = dir_house_center(d->enemy);
        int best = 0x7FFFFFFF;
        for (int i = 0; i < n; i++) {
            CellXY c = object_cell(pool[i]);
            int dd = dir_dist2(c, target);
            if (dd < best) {
                best = dd;
                d->front = c;
            }
        }
    }

    /* The fight at the front: armed enemies (defenses included) against our units there. The
     * centroid would lag behind fresh units waiting at home and hide a losing battle. */
    d->local_enemy = d->local_ours = 0;
    for (int i = 0; i < dir_enemy_count; i++)
        if (dir_enemies[i].armed && dir_dist2(dir_enemies[i].at, d->front) <= 12 * 12)
            d->local_enemy += dir_enemies[i].value;
    for (int i = 0; i < n; i++)
        if (dir_dist2(object_cell(pool[i]), d->front) <= 12 * 12)
            d->local_ours += dir_cost(dir_type(pool[i]));

    int previous = d->state;
    /* Minimum dwell times: flipping between defend and gather sends the army back and forth
     * through chokes and bridges, where it jams. */
    int dwell = CURRENT_FRAME - d->state_frame;
    if (dir_base_threat(d->threat_value, d->army_value, d->state)) {
        if (d->state != DIR_DEFEND && (d->state != DIR_GATHER || dwell >= 300 || d->threat_value > 4000))
            d->state = DIR_DEFEND;
    } else if (d->state == DIR_DEFEND && dwell >= 450)
        d->state = DIR_GATHER;
    if (d->state == DIR_GATHER) {
        if (d->enemy && dir_should_launch(d->army_value, n, d->target_army + (d->enemy_army - d->target_army) / 3, d->enemy_def, CURRENT_FRAME)) {
            d->state = DIR_ATTACK;
            d->launch_value = d->army_value;
            d->launch_frame = d->progress_frame = CURRENT_FRAME;
            d->reached = 0;
            d->best_dist = 0x7FFFFFFF;
            d->objective = NULL;
        }
    } else if (d->state == DIR_ATTACK) {
        /* No progress toward the objective for a long time, with nothing fighting us: the ground
         * route does not exist. Switch production to units that cross water. */
        /* Progress is measured at the front: the unit closest to the objective. Fighting counts. */
        int dd = 0x7FFFFFFF, fighting = 0;
        for (int i = 0; i < n; i++) {
            int u = d->objective ? dir_dist2(object_cell(pool[i]), d->objective_at) : 0x7FFFFFFF;
            dd = u < dd ? u : dd;
            fighting |= FIELD(pool[i], O_TARGET, BYTE *) != NULL;
        }
        if (dd <= 15 * 15)
            d->reached = 1;
        if (fighting || (d->repair_hut && !d->repair_mode) || (long long)dd * 10 < (long long)d->best_dist * 9) {
            d->best_dist = dd < d->best_dist ? dd : d->best_dist;
            d->progress_frame = CURRENT_FRAME;
        } else if (CURRENT_FRAME - d->progress_frame > 1500 && dd > 8 * 8) {
            /* Try other targets first: a building across water may be out of reach while the base
             * itself is not. After several failures the enemy is out of reach by land. */
            if (d->unreachable_count < 8)
                d->unreachable[d->unreachable_count++] = d->objective;
            logmsg("director: house %d frame %d: no progress toward %d,%d (%d unreachable)", FIELD(house, 0x30, int),
                   CURRENT_FRAME, d->objective_at.X, d->objective_at.Y, d->unreachable_count);
            d->objective = NULL;
            d->progress_frame = CURRENT_FRAME;
            d->best_dist = 0x7FFFFFFF;
            if (d->unreachable_count >= 5 && !d->reached && !d->blocked) {
                logmsg("director: house %d: no ground route to the enemy, switching to hover/air",
                       FIELD(house, 0x30, int));
                d->blocked = 1;
                d->blocked_frame = CURRENT_FRAME;
                d->unreachable_count = 0;
                d->state = DIR_RETREAT;
            }
        }
        if (!d->enemy || n == 0 || dir_should_retreat(d->army_value, d->launch_value, d->local_ours, d->local_enemy))
            d->state = DIR_RETREAT;
    } else if (d->state == DIR_RETREAT) {
        if (!n || dir_dist2(d->centroid, d->rally) <= 10 * 10 || dwell > 1200)
            d->state = DIR_GATHER;   /* stragglers must not keep the whole army in retreat */
    }
    if (d->state != previous)
        d->state_frame = CURRENT_FRAME;
    if (d->state != previous || CURRENT_FRAME - d->last_log > 1500) {
        d->last_log = CURRENT_FRAME;
        logmsg("director: house %d frame %d state %d army %d/%d enemy army %d threat %d local %d",
               FIELD(house, 0x30, int), CURRENT_FRAME, d->state, n, d->army_value, d->enemy_army,
               d->threat_value, d->local_enemy);
    }

    if (bench_file && CURRENT_FRAME - d->last_log == 0 && n) {   /* benchmark: sample a few units */
        for (int i = 0; i < n && i < 4; i++) {
            CellXY c = object_cell(pool[i]);
            BYTE *focus = FIELD(pool[i], T_FOCUS, BYTE *);
            CellXY fc = focus && is_cell(focus) ? FIELD(focus, C_MAPCOORDS, CellXY) : (CellXY){ -1, -1 };
            logmsg("director:   unit %.24s at %d,%d mission %d target %p dest %p focus %d,%d",
                   (char *)dir_type(pool[i]) + T_ID, c.X, c.Y, FIELD(pool[i], COMBAT_MISSION, int),
                   FIELD(pool[i], O_TARGET, BYTE *), FIELD(pool[i], COMBAT_DESTINATION, BYTE *), fc.X, fc.Y);
        }
        logmsg("director:   centroid %d,%d rally %d,%d objective %d,%d base %d,%d", d->centroid.X, d->centroid.Y,
               d->rally.X, d->rally.Y, d->objective_at.X, d->objective_at.Y, d->base.X, d->base.Y);
    }
    CellXY goal = d->rally;
    int engage = (director_enabled(house) & DIR_F_FOCUS) != 0;
    if (d->state == DIR_DEFEND)
        goal = d->threat_at;
    else if (d->state == DIR_RETREAT)
        engage = 0;
    else if (d->state == DIR_ATTACK) {
        int alive = 0;
        for (int i = 0; i < dir_enemy_count; i++)
            if (dir_enemies[i].obj == d->objective)
                alive = 1;
        if (!alive || CURRENT_FRAME % 300 < 15) {
            BYTE *previous_objective = d->objective;
            d->objective = dir_pick_objective(d, d->front);
            if (d->objective != previous_objective)
                d->best_dist = 0x7FFFFFFF;
        }
        if (!d->objective && d->unreachable_count && !d->reached && !d->blocked) {
            logmsg("director: house %d: every enemy structure out of reach, switching to hover/air",
                   FIELD(house, 0x30, int));
            d->blocked = 1;
            d->blocked_frame = CURRENT_FRAME;
            d->unreachable_count = 0;
            d->state = DIR_RETREAT;
        } else if (!d->objective) {
            d->enemy = dir_pick_enemy(house, d->base);
            d->unreachable_count = 0;
            d->state = DIR_GATHER;
        } else
            goal = d->objective_at;
    }
    /* Cohesion: the marching group's median distance to the objective. Units well ahead of it
     * hold their ground until the rest catch up, instead of arriving one by one. */
    int hold = -1;
    if (d->state == DIR_ATTACK && d->objective && n >= 4 && (director_enabled(house) & DIR_F_COHESION)) {
        int dist[MAX_POOL], m = 0;
        for (int i = 0; i < n; i++) {
            CellXY c = object_cell(pool[i]);
            int dd = dir_dist2(c, d->objective_at);
            if (dir_dist2(c, d->front) <= 30 * 30)   /* the marching group, not units waiting at home */
                dist[m++] = dd;
        }
        for (int i = 1; i < m; i++)
            for (int j = i; j > 0 && dist[j - 1] > dist[j]; j--) {
                int t = dist[j];
                dist[j] = dist[j - 1];
                dist[j - 1] = t;
            }
        if (m >= 4) {
            int median = 0;
            while (median * median < dist[m / 2])
                median++;
            hold = median > 14 ? (median - 8) * (median - 8) : -1;   /* close in: everyone fights */
        }
    }
    for (int i = 0; i < n; i++) {
        CellXY c = object_cell(pool[i]);
        if (hold > 0 && !FIELD(pool[i], O_TARGET, BYTE *) && dir_dist2(c, d->objective_at) < hold) {
            dir_command(pool[i], c, NULL, engage);   /* area-guard where it stands */
            continue;
        }
        /* In an attack, fresh units far behind wait at the rally for the next wave. */
        if (d->state == DIR_ATTACK && d->launch_frame != CURRENT_FRAME
            && dir_dist2(object_cell(pool[i]), d->centroid) > 30 * 30
            && dir_dist2(object_cell(pool[i]), d->rally) <= 12 * 12) {
            dir_command(pool[i], d->rally, NULL, 1);
            continue;
        }
        dir_command(pool[i], goal, d->state == DIR_ATTACK ? d->objective : NULL, engage);
    }
}

/* ---- economy ---- */
static int dir_idle_harvesters(BYTE *house);
static void dir_economy(BYTE *house, DirState *d)
{
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2 || !(director_enabled(house) & DIR_F_ECONOMY) || !oil_eligible(house))
        return;
    /* The first war factory as soon as a refinery stands: stock base plans (Allied especially) put
     * it behind airfields and walls, leaving the army nothing to come from for minutes. */
    static const char *factories[3] = { "GAWEAP", "NAWEAP", "YAWEAP" };
    if (FIELD(house, OIL_H_PRODUCING, int) == -1 && !combat_building_count(house, factories[side])) {
        BYTE *type = find_type(BUILDINGTYPE_ARRAY, factories[side]);
        if (type && FIELD(house, OIL_H_CASH, int) >= dir_cost(type)
            && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0) {
            FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
            logmsg("director: house %d queued its first war factory at frame %d", FIELD(house, 0x30, int), CURRENT_FRAME);
        }
    }
    if (CURRENT_FRAME >= d->next_economy) {
        d->next_economy = CURRENT_FRAME + 450;
        d->idle_harvesters = dir_idle_harvesters(house);
    }
    /* checked every tick: the build queue is rarely free, and stock picks fill it at once */
    if (FIELD(house, OIL_H_PRODUCING, int) != -1
        || !dir_want_refinery(CURRENT_FRAME, FIELD(house, OIL_H_REFINERIES, int), FIELD(house, H_HARVESTERS, int),
                              d->idle_harvesters, FIELD(house, OIL_H_CASH, int)))
        return;
    static const char *refineries[] = { "GAREFN", "NAREFN", "YAREFN" };
    BYTE *type = find_type(BUILDINGTYPE_ARRAY, refineries[side]);
    if (type && FIELD(house, OIL_H_CASH, int) >= dir_cost(type)
        && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0) {
        FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
        logmsg("director: house %d queued %s (refineries %d, harvesters %d, cash %d)", FIELD(house, 0x30, int),
               refineries[side], FIELD(house, OIL_H_REFINERIES, int), FIELD(house, H_HARVESTERS, int),
               FIELD(house, OIL_H_CASH, int));
    }
}

static int dir_idle_harvesters(BYTE *house)
{
    int idle = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i], *type;
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 1 || !(type = dir_type(o))
            || !in_list("HARV,CMIN,SMIN", (char *)type + T_ID))
            continue;
        int m = FIELD(o, COMBAT_MISSION, int);
        idle += m != 10 && m != 7 && m != 12 && m != 16 && m != 2;   /* harvest, enter, return, unload, move */
    }
    return idle;
}

/* ---- bridges ----
 * A destroyed bridge cuts armies and harvesters off. Stock AI never repairs one; the director sends
 * an engineer into the nearest reachable repair hut whose bridge is down, the check the engineer's
 * own enter cursor uses. */
#define BT_BRIDGE_HUT 0x16B6              /* BuildingTypeClass::BridgeRepairHut */
#define MAP_BRIDGE_DOWN 0x587410          /* MapClass::IsLinkedBridgeDestroyed(CellStruct *hut cell) */
#define MISSION_ENTER 7
#define MISSION_CAPTURE 8

static int dir_bridge_down(BYTE *hut)
{
    CellXY c = object_cell(hut);
    return ((char (GTHISCALL *)(void *, CellXY *))MAP_BRIDGE_DOWN)(MAP_INSTANCE, &c) != 0;
}

static int dir_is_engineer(BYTE *obj)
{
    BYTE *type = dir_type(obj);
    return type && in_list("ENGINEER,SENGINEER,YENGINEER", (char *)type + T_ID);
}

static int dir_object_listed(DynVec *v, BYTE *obj)
{
    for (int i = 0; i < v->Count; i++)
        if (v->Items[i] == obj)
            return 1;
    return 0;
}

/* Engineer jobs, one at a time: repair the nearest broken bridge within reach, otherwise capture an
 * enemy tech building (oil derrick first) near our army or base once no armed enemy guards it. */
static void dir_engineers(BYTE *house, DirState *d)
{
    BYTE *job = d->repair_hut;
    int alive = job && dir_object_listed(OIL_BUILDING_ARRAY, job) && oil_live(job);
    int done = !alive || (d->repair_mode == 0 ? !dir_bridge_down(job) : FIELD(job, O_OWNER, BYTE *) == house
                                                                    || !dir_hostile(house, FIELD(job, O_OWNER, BYTE *)));
    if (job && (done || CURRENT_FRAME - d->repair_frame > 4000)) {
        if (!done) {   /* out of reach (e.g. the hut is across the water): leave it for a while */
            d->failed_job = job;
            d->failed_frame = CURRENT_FRAME;
        }
        if (alive && done)
            logmsg("director: house %d engineer job done (%s)", FIELD(house, 0x30, int),
                   d->repair_mode ? "captured" : "bridge repaired");
        d->repair_hut = d->repair_engineer = NULL;
    }
    d->want_engineer = 0;
    if (d->repair_hut && d->repair_engineer && dir_object_listed(OIL_TECHNO_ARRAY, d->repair_engineer)
        && oil_live(d->repair_engineer))
        return;
    CellXY base = d->base;
    (void)base;
    BYTE *target = NULL;
    int best = 45 * 45, mode = 0;
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *b = v->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
        if (!oil_live(b) || !type || !type[BT_BRIDGE_HUT] || !dir_bridge_down(b))
            continue;
        CellXY c = object_cell(b);
        int dd = dir_dist2(c, d->base), df = dir_dist2(c, d->front);
        dd = df < dd ? df : dd;
        if (dd >= best || (b == d->failed_job && CURRENT_FRAME - d->failed_frame < 9000))
            continue;
        best = dd;
        target = b;
    }
    if (!target) {
        best = 18 * 18;
        for (int i = 0; i < dir_enemy_count; i++) {
            DirEnemy *e = &dir_enemies[i];
            if (!e->capturable)
                continue;
            int dd = dir_dist2(e->at, d->base), df = dir_dist2(e->at, d->front);
            dd = df < dd ? df : dd;
            int oil = !_stricmp((char *)dir_type(e->obj) + T_ID, "CAOILD");
            if (!oil)
                dd += 6 * 6;
            if (dd >= best)
                continue;
            int guarded = 0;
            for (int k = 0; k < dir_enemy_count && !guarded; k++)
                guarded = dir_enemies[k].armed && !dir_enemies[k].capturable
                          && dir_dist2(dir_enemies[k].at, e->at) <= 7 * 7;
            if (guarded || (e->obj == d->failed_job && CURRENT_FRAME - d->failed_frame < 9000))
                continue;
            best = dd;
            target = e->obj;
            mode = 1;
        }
    }
    if (!target)
        return;
    BYTE *engineer = NULL;
    v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count && !engineer; i++) {
        BYTE *o = v->Items[i];
        if (oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_whatami(o) == 15 && dir_is_engineer(o)
            && !FIELD(o, F_TEAM, BYTE *))
            engineer = o;
    }
    if (!engineer) {
        d->want_engineer = 1;
        return;
    }
    d->repair_hut = target;
    d->repair_engineer = engineer;
    d->repair_frame = CURRENT_FRAME;
    d->repair_mode = mode;
    dir_order(engineer, mode ? MISSION_CAPTURE : MISSION_ENTER, target, NULL);
    CellXY c = object_cell(target);
    logmsg("director: house %d sends an engineer to %s %.24s at %d,%d", FIELD(house, 0x30, int),
           mode ? "capture" : "repair the bridge at", (char *)dir_type(target) + T_ID, c.X, c.Y);
}

#define MAP_NEARBY 0x56DC20      /* MapClass::NearByLocation, 15 arguments (ret 0x3C) */
typedef CellXY *(GTHISCALL *nearby_fn)(void *, CellXY *, CellXY *, int, int, int, char, int, int, char, char,
                                        char, char, CellXY *, char, char);

/* Rally a third of the way toward the enemy (at most 14 cells out), on a clear 5x5 patch of land,
 * no bridge, reachable from the base. A straight-line point can land on a bridge or at a choke,
 * and an army gathering there jams it. */
static CellXY dir_pick_rally(DirState *d)
{
    if (!d->enemy)
        return d->base;
    CellXY e = dir_house_center(d->enemy);
    int dx = e.X - d->base.X, dy = e.Y - d->base.Y, len = 1;
    while (len * len < dx * dx + dy * dy)
        len++;
    CellXY base = d->base, fallback = base;
    int zone = ((int (GTHISCALL *)(void *, CellXY *, int, char))MAP_ZONE)(MAP_INSTANCE, &base, 0, 0);
    /* Out of the base: units idling on building sites block construction. */
    for (int step = 12; step <= 24 && step < len; step += 4) {
        CellXY want = { (short)(d->base.X + dx * step / len), (short)(d->base.Y + dy * step / len) }, out = { 0, 0 };
        if (step == 12)
            fallback = want;
        /* SpeedType Track, MovementZone Normal, 5x5 clear, no overlay, no bridge */
        ((nearby_fn)MAP_NEARBY)(MAP_INSTANCE, &out, &want, 1, zone, 0, 0, 5, 5, 1, 0, 0, 0, &want, 0, 0);
        if (out.X <= 0 || out.Y <= 0 || dir_dist2(out, want) > 10 * 10)
            continue;
        CellXY c = { (short)(out.X + 2), (short)(out.Y + 2) };   /* the patch's centre */
        if (dir_dist2(c, d->base) >= 9 * 9 && !dir_near_own_building(NULL, c, 6))
            return c;
    }
    return fallback;
}

/* ---- expansion ----
 * When harvesters stand idle (ore gone or cut off), look for reachable ore away from our refineries
 * and enemy structures, build an MCV and deploy it there. An AI construction yard re-centres the
 * base plan on itself (UnitClass deploy, 0x7398CE-0x739926), so new refineries follow the ore. */
#define CELL_ORE_VALUE 0x485020            /* CellClass::GetContainedTiberiumValue */

static int dir_ore_near(CellXY c, int r)
{
    int sum = 0;
    for (int dy = -r; dy <= r; dy += 2)
        for (int dx = -r; dx <= r; dx += 2) {
            BYTE *cell = dir_cell((CellXY){ (short)(c.X + dx), (short)(c.Y + dy) });
            if (cell)
                sum += ((int (GTHISCALL *)(BYTE *))CELL_ORE_VALUE)(cell);
        }
    return sum;
}

static int dir_find_site(BYTE *house, DirState *d, CellXY *site)
{
    int best = 0, max_ore = 0, with_ore = 0, rejected = 0;
    DynVec *bv = OIL_BUILDING_ARRAY;
    for (int y = 4; y < 400; y += 4)
        for (int x = 4; x < 400; x += 4) {
            CellXY c = { (short)x, (short)y };
            if (!dir_cell(c))
                continue;
            int ore = dir_ore_near(c, 6);
            max_ore = ore > max_ore ? ore : max_ore;
            if (ore < 2500)
                continue;
            with_ore++;
            int dist = dir_dist2(c, d->base), ok = dist >= 16 * 16 && dist <= 45 * 45;
            for (int i = 0; ok && i < bv->Count; i++) {
                BYTE *b = bv->Items[i], *owner = FIELD(b, O_OWNER, BYTE *);
                if (!oil_live(b))
                    continue;
                CellXY bc = object_cell(b);
                if (owner == house && FIELD(FIELD(b, B_TYPE, BYTE *), 0x16BB, char))   /* BuildingTypeClass::Refinery: ours nearby */
                    ok = dir_dist2(bc, c) >= 14 * 14;
                else if (dir_hostile(house, owner))
                    ok = dir_dist2(bc, c) >= 28 * 28;
            }
            for (int i = 0; ok && i < dir_enemy_count; i++)
                ok = !dir_enemies[i].armed || dir_dist2(dir_enemies[i].at, c) >= 14 * 14;
            if (!ok) {
                rejected++;
                continue;
            }
            int dd = 0;
            while (dd * dd < dist)
                dd++;
            int score = ore - dd * 60;   /* rich ore, not too far from home */
            if (score > best) {
                best = score;
                *site = c;
            }
        }
    if (bench_file)
        logmsg("director: house %d site search: max ore %d, %d ore spots, %d rejected, best score %d",
               FIELD(house, 0x30, int), max_ore, with_ore, rejected, best);
    if (!best)
        return 0;
    /* a clear 4x4 spot for the construction yard beside the ore */
    CellXY out = { 0, 0 }, want = *site;
    /* overlay allowed: the ore is overlay */
    ((nearby_fn)MAP_NEARBY)(MAP_INSTANCE, &out, &want, 1, -1, 0, 0, 4, 4, 0, 0, 0, 0, &want, 0, 0);
    if (out.X <= 0 || dir_dist2(out, want) > 10 * 10)
        return 0;
    *site = (CellXY){ (short)(out.X + 1), (short)(out.Y + 1) };
    return 1;
}

static void dir_expansion(BYTE *house, DirState *d)
{
    int yards = combat_building_count(house, "GACNST,NACNST,YACNST");
    BYTE *mcv = NULL;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count && !mcv; i++) {
        BYTE *o = v->Items[i];
        if (oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_whatami(o) == 1
            && in_list(dir_mcvs, (char *)dir_type(o) + T_ID))
            mcv = o;
    }
    /* Drive an MCV we built to the site and deploy. Without a yard the stock AI redeploys it at home. */
    if (mcv && yards && d->site.X > 0) {
        d->want_mcv = 0;
        CellXY at = object_cell(mcv);
        BYTE *cell = dir_cell(d->site);
        if (dir_dist2(at, d->site) <= 2 * 2) {
            if (!dir_recent_order(mcv, (BYTE *)1, 120)) {
                dir_order(mcv, 16, NULL, NULL);   /* Unload = deploy */
                logmsg("director: house %d deploying expansion MCV at %d,%d", FIELD(house, 0x30, int), at.X, at.Y);
            }
        } else if (cell && !dir_recent_order(mcv, cell, 450))
            dir_order(mcv, MISSION_MOVE, NULL, cell);
        return;
    }
    if (CURRENT_FRAME < d->next_site)
        return;
    d->next_site = CURRENT_FRAME + 900;
    d->want_mcv = 0;
    if (!(director_enabled(house) & DIR_F_EXPANSION) || !yards || yards >= 3 || mcv || CURRENT_FRAME < 9000
        || (d->idle_harvesters < 1 && !bench_force_expand) || d->state == DIR_DEFEND)
        return;
    if (!dir_find_site(house, d, &d->site)) {
        if (bench_file)
            logmsg("director: house %d found no expansion site", FIELD(house, 0x30, int));
        return;
    }
    d->want_mcv = 1;
    logmsg("director: house %d wants an expansion at %d,%d (idle harvesters %d)", FIELD(house, 0x30, int),
           d->site.X, d->site.Y, d->idle_harvesters);
    /* the MCV needs a service depot (Yuri: grinder) */
    static const char *depots[3] = { "GADEPT", "NADEPT", "YAGRND" };
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side >= 0 && side <= 2 && !combat_building_count(house, depots[side]) && FIELD(house, OIL_H_PRODUCING, int) == -1) {
        BYTE *type = find_type(BUILDINGTYPE_ARRAY, depots[side]);
        if (type && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0) {
            FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
            logmsg("director: house %d queued %s for the expansion MCV", FIELD(house, 0x30, int), depots[side]);
        }
    }
}

/* ---- ferry ----
 * The enemy base lies in another movement zone: land units cannot walk there. One amphibious
 * transport at a time loads ground units at home, drives to a landing spot in the enemy's zone and
 * unloads; landed units rejoin the army there. */

static int dir_zone(CellXY c)
{
    return ((int (GTHISCALL *)(void *, CellXY *, int, char))MAP_ZONE)(MAP_INSTANCE, &c, 0, 0);
}

static void dir_ferry(BYTE *house, DirState *d)
{
    int side = FIELD(house, OIL_H_SIDE, int);
    if (!(d->island || d->blocked) || side < 0 || side > 2)
        return;
    if (d->ferry && (!dir_object_listed(OIL_TECHNO_ARRAY, d->ferry) || !oil_live(d->ferry)
                     || FIELD(d->ferry, O_OWNER, BYTE *) != house))
        d->ferry = NULL;
    if (!d->ferry) {
        DynVec *v = OIL_TECHNO_ARRAY;
        for (int i = 0; i < v->Count && !d->ferry; i++) {
            BYTE *o = v->Items[i];
            if (oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_whatami(o) == 1
                && !_stricmp((char *)dir_type(o) + T_ID, dir_transports[side]))
                d->ferry = o;
        }
        d->ferry_state = 0;
        d->ferry_frame = CURRENT_FRAME;
        if (!d->ferry)
            return;
        d->dock = d->rally;   /* amphibious: it drives ashore to the rally, where the troops are */
    }
    BYTE *t = d->ferry;
    CellXY at = object_cell(t);
    int passengers = FIELD(t, T_PASSENGERS, int);
    if (bench_file && CURRENT_FRAME % 900 < 15)
        logmsg("director: house %d ferry %.24s state %d aboard %d at %d,%d dock %d,%d mission %d",
               FIELD(house, 0x30, int), (char *)dir_type(t) + T_ID, d->ferry_state, passengers, at.X, at.Y,
               d->dock.X, d->dock.Y, FIELD(t, COMBAT_MISSION, int));
    if (d->ferry_state == 0 && dir_dist2(at, d->dock) > 6 * 6) {   /* come ashore first */
        if (CURRENT_FRAME - d->ferry_frame > 600)
            d->dock = at;   /* the dock is blocked: load where it stands */
        else if (!dir_recent_order(t, dir_cell(d->dock), 450))
            dir_order(t, MISSION_MOVE, NULL, dir_cell(d->dock));
        return;
    }
    if (d->ferry_state == 0) {   /* load: call the nearest idle ground units on this side */
        if (passengers >= 6 || (passengers && CURRENT_FRAME - d->ferry_frame > 1200)) {
            CellXY e = dir_house_center(d->enemy), out = { 0, 0 };
            int zone = dir_zone(e);
            ((nearby_fn)MAP_NEARBY)(MAP_INSTANCE, &out, &e, 1, zone, 0, 0, 2, 2, 1, 0, 0, 0, &d->base, 0, 0);
            if (out.X <= 0)
                out = d->landing;   /* crowded now: the last landing spot */
            if (out.X <= 0)
                return;
            d->landing = out;
            d->ferry_state = 1;
            d->ferry_frame = CURRENT_FRAME;
            logmsg("director: house %d ferry sails with %d aboard to %d,%d", FIELD(house, 0x30, int), passengers,
                   out.X, out.Y);
            dir_order(t, MISSION_MOVE, NULL, dir_cell(out));
            return;
        }
        int called = 0;
        DynVec *v = OIL_TECHNO_ARRAY;
        for (int i = 0; i < v->Count && called < 8 - passengers; i++) {
            BYTE *o = v->Items[i];
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_crosses(o) || !dir_poolable(o, dir_whatami(o)))
                continue;
            CellXY c = object_cell(o);
            if (dir_dist2(c, at) > 40 * 40 || dir_landed(d, c))
                continue;
            called++;
            if (!dir_recent_order(o, t, 300)) {
                /* Entering a transport follows Destination, not Target */
                ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(o, VT_QUEUEMISSION))(o, MISSION_ENTER, 0);
                ((void (GTHISCALL *)(BYTE *, BYTE *, char))VFUNC(o, COMBAT_SET_DESTINATION))(o, t, 1);
            }
        }
    } else if (d->ferry_state == 1) {   /* sail, then unload beside the enemy */
        if (dir_dist2(at, d->landing) <= 4 * 4 || CURRENT_FRAME - d->ferry_frame > 4000) {
            dir_order(t, 16, NULL, NULL);
            d->ferry_state = 2;
            d->ferry_frame = CURRENT_FRAME;
        } else if (!dir_recent_order(t, dir_cell(d->landing), 450))
            dir_order(t, MISSION_MOVE, NULL, dir_cell(d->landing));
    } else if (d->ferry_state == 2) {   /* wait until empty, then go home for the next load */
        if (!passengers || CURRENT_FRAME - d->ferry_frame > 900) {
            d->ferry_state = 3;
            dir_order(t, MISSION_MOVE, NULL, dir_cell(d->dock));
        } else if (!dir_recent_order(t, (BYTE *)2, 150))
            dir_order(t, 16, NULL, NULL);
    } else if (dir_dist2(at, d->dock) <= 5 * 5 || CURRENT_FRAME - d->ferry_frame > 6000) {
        d->ferry_state = 0;
        d->ferry_frame = CURRENT_FRAME;
    }
}

static void dir_update(BYTE *house)
{
    if (!dir_active(house) || !(director_enabled(house) & DIR_F_ARMY))
        return;
    DirState *d = dir_get(house);
    if (!d || CURRENT_FRAME < d->next_think)
        return;
    d->next_think = CURRENT_FRAME + 15;
    d->base = dir_house_center(house);
    if (!d->enemy || !dir_hostile(house, d->enemy) || !dir_house_alive(d->enemy)) {
        d->enemy = dir_pick_enemy(house, d->base);
        d->unreachable_count = 0;
    }
    if (d->enemy != d->rally_enemy || CURRENT_FRAME >= d->rally_frame) {
        d->rally_enemy = d->enemy;
        d->rally_frame = CURRENT_FRAME + 3000;
        d->rally = dir_pick_rally(d);
        d->home_zone = dir_zone(d->rally);   /* the rally is clear land, unlike a built-over base centre */
    }
    /* Cut off by water, from the start or because a bridge fell (the engine re-zones the map then).
     * Re-checked often: a repaired bridge brings the ground war back. */
    if (CURRENT_FRAME >= d->next_island) {
        d->next_island = CURRENT_FRAME + 600;
        /* Engine zone labels proved unreliable as a connectivity test (one landmass reads several
         * labels), so being cut off is decided from evidence: an attack that could reach no objective
         * sets `blocked`. It is retried every 6000 frames, e.g. once engineers have mended a bridge. */
        int island = d->enemy && bench_force_island;
        if (d->blocked && CURRENT_FRAME - d->blocked_frame > 6000) {
            d->blocked = 0;
            logmsg("director: house %d frame %d: retrying the ground route", FIELD(house, 0x30, int), CURRENT_FRAME);
        }
        if (island != d->island)
            logmsg("director: house %d frame %d: enemy base %s", FIELD(house, 0x30, int), CURRENT_FRAME,
                   island ? "cut off by water: ferrying troops, building hover/air units" : "reachable by land again");
        d->island = island;
    }
    dir_scan_enemies(house, d);
    dir_army(house, d);
    if (CURRENT_FRAME % 150 < 15 && (director_enabled(house) & DIR_F_ENGINEERS))
        dir_engineers(house, d);
    dir_economy(house, d);
    dir_expansion(house, d);
    dir_ferry(house, d);
    /* Allies need a Robot Control Center before Robot Tanks can cross water. */
    if ((d->blocked || d->island) && FIELD(house, OIL_H_SIDE, int) == 0 && FIELD(house, OIL_H_PRODUCING, int) == -1
        && !combat_building_count(house, "GAROBO")) {
        BYTE *type = find_type(BUILDINGTYPE_ARRAY, "GAROBO");
        if (type && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0
            && FIELD(house, OIL_H_CASH, int) >= dir_cost(type)) {
            FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
            logmsg("director: house %d queued GAROBO for Robot Tanks", FIELD(house, 0x30, int));
        }
    }
}

static void patch_director(void)
{
    const BYTE unit[] = { 0x81, 0xEC, 0xDC, 0x04, 0x00, 0x00 };
    if (!patch_checked("director unit production", DIR_UNIT_PRODUCTION, unit, sizeof unit)
        || !patch_checked("director infantry production", DIR_INF_PRODUCTION, unit, sizeof unit))
        return;
    BYTE *tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        logmsg("director: could not allocate trampolines, not patched");
        return;
    }
    memcpy(tramp, unit, sizeof unit);
    patch_rel((DWORD)tramp + sizeof unit, 0xE9, DIR_UNIT_PRODUCTION + sizeof unit);
    memcpy(tramp + 16, unit, sizeof unit);
    patch_rel((DWORD)tramp + 16 + sizeof unit, 0xE9, DIR_INF_PRODUCTION + sizeof unit);
    dir_unit_original = (dir_prod_fn)tramp;
    dir_inf_original = (dir_prod_fn)(tramp + 16);
    patch(DIR_UNIT_PRODUCTION + 5, (const BYTE[]){ 0x90 }, 1);
    patch_rel(DIR_UNIT_PRODUCTION, 0xE9, (DWORD)dir_unit_production);
    patch(DIR_INF_PRODUCTION + 5, (const BYTE[]){ 0x90 }, 1);
    patch_rel(DIR_INF_PRODUCTION, 0xE9, (DWORD)dir_inf_production);
    logmsg("director: production patches applied (Brutal skirmish only)");
}

/* Benchmark CSV tail: war factories, current vehicle and infantry orders, director state and army. */
static void bench_row_extra(BYTE *h)
{
    int unit = FIELD(h, H_PRODUCING_UNIT, int), inf = FIELD(h, H_PRODUCING_INF, int), idx = FIELD(h, 0x30, int) & 31;
    DynVec *ut = UNITTYPE_ARRAY, *it = INFANTRYTYPE_ARRAY;
    DirState *d = &dir_state[idx];
    int building = FIELD(h, OIL_H_PRODUCING, int);
    DynVec *bt = BUILDINGTYPE_ARRAY;
    fprintf(bench_file, ",%d,%.24s,%.24s,%.24s,%d,%d\n", combat_building_count(h, "GAWEAP,NAWEAP,YAWEAP"),
            building >= 0 && building < bt->Count ? (char *)bt->Items[building] + T_ID : "-",
            unit >= 0 && unit < ut->Count ? (char *)ut->Items[unit] + T_ID : "-",
            inf >= 0 && inf < it->Count ? (char *)it->Items[inf] + T_ID : "-",
            d->house == h ? d->state : -1, d->house == h ? d->army_value : 0);
}

/* Benchmark camera: centre the view on a house's army front (TacticalClass::SetTacticalPosition). */
static void bench_camera_update(void)
{
    if (bench_camera < 0 || bench_camera >= 32 || !dir_state[bench_camera].house)
        return;
    CellXY c = dir_state[bench_camera].army_count ? dir_state[bench_camera].front : dir_state[bench_camera].base;
    Coord at = { c.X * 256 + 128, c.Y * 256 + 128, 0 };
    at.Z = ((int (GTHISCALL *)(void *, Coord *))MAP_FLOOR_HEIGHT)(MAP_INSTANCE, &at);
    ((void (GTHISCALL *)(void *, Coord *))0x6D6070)(*(void **)0x887324, &at);
}
