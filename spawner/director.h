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
#define MAX_ENEMY 1024

typedef int (GTHISCALL *dir_prod_fn)(BYTE *);
static dir_prod_fn dir_unit_original, dir_inf_original;

typedef struct {
    BYTE *house;
    int state, next_think, launch_value, launch_frame, last_log;
    int army_value, army_count, enemy_army, local_enemy, threat_value;
    int enemy_air, enemy_inf, enemy_armor, enemy_def;
    int unit_request, unit_request_frame;
    int blocked, best_dist, progress_frame;   /* ground army cannot reach the enemy: island map */
    int reached;                              /* this attack got within 15 cells of an objective */
    BYTE *unreachable[8];                     /* objectives the army made no progress toward */
    int unreachable_count;
    CellXY base, rally, threat_at, objective_at, centroid, front;
    BYTE *objective, *enemy;
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
            if (type && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0
                && dir_can_spend(FIELD(house, OIL_H_CASH, int), dir_cost(type), reserve)
                && (_stricmp(id, "MIND") || dir_owned_of(house, type) < 3))
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
        pick[r] = dir_first_buildable(house, UNITTYPE_ARRAY, d->blocked && r == ROLE_MAIN
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
    if (current == -1)
        dir_choose_vehicle(house, d);
    return result;
}

static int GFASTCALL dir_inf_production(BYTE *house, void *unused)
{
    (void)unused;
    int result = dir_inf_original(house);
    if (!dir_active(house) || !(director_enabled(house) & DIR_F_PRODUCTION) || FIELD(house, H_PRODUCING_INF, int) != -1
        || FIELD(house, H_OWNED_INFANTRY, int) >= 30)
        return result;
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2)
        return result;
    BYTE *type = dir_first_buildable(house, INFANTRYTYPE_ARRAY, dir_infantry[side], 4000);
    int index = type ? dir_type_index(INFANTRYTYPE_ARRAY, type) : -1;
    if (index >= 0)
        FIELD(house, H_PRODUCING_INF, int) = index;
    return result;
}

/* ---- army ---- */

typedef struct {
    BYTE *obj;
    CellXY at;
    int value, armed, building, air, infantry, focus;
} DirEnemy;
static DirEnemy dir_enemies[MAX_ENEMY];
static int dir_enemy_count;

static void dir_scan_enemies(BYTE *house, DirState *d)
{
    dir_enemy_count = 0;
    d->enemy_army = d->enemy_air = d->enemy_inf = d->enemy_armor = d->enemy_def = 0;
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
        e->building = what == 6;
        e->infantry = what == 15;
        e->focus = 0;
        Coord c = FIELD(o, O_LOCATION, Coord);
        int floor = ((int (GTHISCALL *)(void *, Coord *))MAP_FLOOR_HEIGHT)(MAP_INSTANCE, &c);
        e->air = what == 2 || c.Z > floor + 128;
        if (!e->armed)
            continue;
        if (e->building) {
            d->enemy_def += e->value;
            continue;
        }
        d->enemy_army += e->value;
        if (e->air)
            d->enemy_air += e->value;
        else if (e->infantry)
            d->enemy_inf += e->value;
        else
            d->enemy_armor += e->value;
        if (!e->air && dir_dist2(e->at, d->base) <= 22 * 22) {
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

/* The nearest hostile player house to our base. */
static BYTE *dir_pick_enemy(BYTE *house, CellXY base)
{
    DynVec *v = HOUSE_ARRAY;
    BYTE *best = NULL;
    int best_d = 0x7FFFFFFF;
    for (int i = 0; i < v->Count; i++) {
        BYTE *h = v->Items[i];
        if (!dir_hostile(house, h) || !FIELD(h, H_OWNED_BUILDINGS, int))
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
    int best_score = 0x7FFFFFFF;
    for (int i = 0; i < dir_enemy_count; i++) {
        DirEnemy *e = &dir_enemies[i];
        if (!e->building || FIELD(e->obj, O_OWNER, BYTE *) != d->enemy)
            continue;
        int skip = 0;
        for (int k = 0; k < d->unreachable_count; k++)
            skip |= d->unreachable[k] == e->obj;
        if (skip)
            continue;
        int score = dir_dist2(from, e->at);
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

static int dir_weapon_cells(BYTE *unit)
{
    BYTE **w = ((BYTE **(GTHISCALL *)(BYTE *, int))VFUNC(unit, VT_GETWEAPON))(unit, 0);
    int range = w && *w ? FIELD(*w, W_RANGE, int) : 0;
    return range < 0 ? 20 : range / 256;
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
            if (dd > reach * reach)
                continue;
            if (e->obj == current)
                keep = e;
            int priority = e->armed ? (e->building ? 70 : 100) : (e->building ? 25 : 45);
            int strength = FIELD(dir_type(e->obj), OT_STRENGTH, int);
            int hurt = strength > 0 ? 40 - 40 * FIELD(e->obj, O_HEALTH, int) / strength : 0;
            int score = priority + hurt + (e->focus < 5 ? e->focus * 8 : -40) - dd / 4;
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

static void dir_army(BYTE *house, DirState *d)
{
    BYTE *pool[MAX_POOL];
    int n = 0, value = 0, sx = 0, sy = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count && n < MAX_POOL; i++) {
        BYTE *o = v->Items[i];
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || !dir_poolable(o, dir_whatami(o)))
            continue;
        if (d->blocked && !dir_crosses(o))
            continue;   /* on an island map only hover/air units attack; the rest guard the base */
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

    /* local fight around the army */
    d->local_enemy = 0;
    for (int i = 0; i < dir_enemy_count; i++)
        if (dir_enemies[i].armed && dir_dist2(dir_enemies[i].at, d->centroid) <= 14 * 14)
            d->local_enemy += dir_enemies[i].value;

    int previous = d->state;
    if (dir_base_threat(d->threat_value, d->army_value, d->state))
        d->state = DIR_DEFEND;
    else if (d->state == DIR_DEFEND)
        d->state = DIR_GATHER;
    if (d->state == DIR_GATHER) {
        if (d->enemy && dir_should_launch(d->army_value, n, d->enemy_army, d->enemy_def, CURRENT_FRAME)) {
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
        if (fighting || (long long)dd * 10 < (long long)d->best_dist * 9) {
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
                d->unreachable_count = 0;
                d->state = DIR_RETREAT;
            }
        }
        if (!d->enemy || n == 0 || dir_should_retreat(d->army_value, d->launch_value, d->local_enemy))
            d->state = DIR_RETREAT;
    } else if (d->state == DIR_RETREAT) {
        if (!n || dir_dist2(d->centroid, d->rally) <= 10 * 10)
            d->state = DIR_GATHER;
    }
    if (d->state != previous || CURRENT_FRAME - d->last_log > 1500) {
        d->last_log = CURRENT_FRAME;
        logmsg("director: house %d frame %d state %d army %d/%d enemy army %d threat %d local %d",
               FIELD(house, 0x30, int), CURRENT_FRAME, d->state, n, d->army_value, d->enemy_army,
               d->threat_value, d->local_enemy);
    }

    if (CURRENT_FRAME - d->last_log == 0 && n) {   /* sample a few units with each state line */
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
            d->unreachable_count = 0;
            d->state = DIR_RETREAT;
        } else if (!d->objective) {
            d->enemy = dir_pick_enemy(house, d->base);
            d->unreachable_count = 0;
            d->state = DIR_GATHER;
        } else
            goal = d->objective_at;
    }
    for (int i = 0; i < n; i++) {
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

static void dir_update(BYTE *house)
{
    if (!dir_active(house) || !(director_enabled(house) & DIR_F_ARMY))
        return;
    DirState *d = dir_get(house);
    if (!d || CURRENT_FRAME < d->next_think)
        return;
    d->next_think = CURRENT_FRAME + 15;
    d->base = dir_house_center(house);
    if (!d->enemy || !dir_hostile(house, d->enemy) || !FIELD(d->enemy, H_OWNED_BUILDINGS, int)) {
        d->enemy = dir_pick_enemy(house, d->base);
        d->unreachable_count = 0;
    }
    if (d->enemy) {
        /* Rally a third of the way toward the enemy, at most 18 cells out. */
        CellXY e = dir_house_center(d->enemy);
        int dx = e.X - d->base.X, dy = e.Y - d->base.Y;
        int len = 1;
        while (len * len < dx * dx + dy * dy)
            len++;
        int step = len / 3 < 18 ? len / 3 : 18;
        d->rally = (CellXY){ (short)(d->base.X + dx * step / len), (short)(d->base.Y + dy * step / len) };
    } else
        d->rally = d->base;
    dir_scan_enemies(house, d);
    dir_army(house, d);
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
