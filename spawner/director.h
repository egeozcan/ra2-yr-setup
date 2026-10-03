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
#define AIRCRAFTTYPE_ARRAY ((DynVec *)0xA8B218)
#define H_PRODUCING_AIR 0x5658
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
#define MAX_POOL 512   /* 160 cut big armies short: the rest went uncounted and idle, and the enemy's
                            * full army looked bigger than ours forever (stalemates to the time limit) */
#define MAP_ZONE 0x56D230          /* MapClass::GetMovementZoneType(cell, MovementZone, bridge) */
#define T_PASSENGERS 0x114         /* TechnoClass::Passengers.NumPassengers */
#define MAX_ENEMY 1024
#define UT_HARVESTER 0xE0E         /* UnitTypeClass::Harvester (read at 0x7476A6) */
#define TT_NAVAL 0xCCE             /* TechnoTypeClass::Naval (after Repairable 0xCCC, Crewed 0xCCD) */
#define VT_SELL 0x1A0              /* ObjectClass::Sell(control): 1 queues the Selling mission */
#define T_CAPTURE_MANAGER 0x2BC    /* TechnoClass::CaptureManager: set on mind-controllers */
#define T_MIND_CONTROLLED_BY 0x2C0 /* TechnoClass::MindControlledBy */
#define T_BUNKER_LINK 0x2E4        /* TechnoClass::BunkerLinkedItem: unit <-> the tank bunker holding it */

#define CELL_ORE_VALUE 0x485020            /* CellClass::GetContainedTiberiumValue */
static int dir_is_engineer(BYTE *obj);
static int dir_wall_cell(BYTE *cell);
static int dir_zone(CellXY c);
static int dir_is_land(CellXY c);
static void dir_announce(BYTE *house, const char *fmt, ...);
static int dir_height(CellXY c);
static int dir_blocks_passage(BYTE *type, CellXY tl);
typedef struct DirState DirState;
static unsigned dir_next_roll(DirState *d);
static int dir_ore_blocked(BYTE *house, CellXY c);
typedef int (GTHISCALL *dir_prod_fn)(BYTE *);
static const char *dir_mcvs = "AMCV,SMCV,PCV";
#define DIR_WALLS "GAWALL,NAWALL,YAWALL,GAFWLL"
static const char *dir_transports[3] = { "LCRF", "SAPC", "YHVR" };   /* amphibious ferries */
static dir_prod_fn dir_unit_original, dir_inf_original;

#define DIR_CONVOY 4
struct DirState {
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
    CellXY repair_eng_at;                     /* where the engineer was last seen alive */
    int repair_frame, repair_mode, want_engineer, failed_frame;
    BYTE *failed_job;   /* mode 0 bridge, 1 capture */
    int idle_harvesters, next_economy;
    int want_mcv, want_mcv_frame, next_site, deploy_tries;                  /* expansion: build an MCV and deploy it by fresh ore */
    CellXY site;
    int island, next_island, home_zone;   /* enemy only reachable by water: ferry troops */
    /* the convoy: transports that load together, sail together and land on the same beach */
    BYTE *ferry[DIR_CONVOY];
    int ferry_state[DIR_CONVOY], ferry_frame[DIR_CONVOY], ferry_aboard[DIR_CONVOY], ferry_aboard_frame[DIR_CONVOY];
    int ferry_docked[DIR_CONVOY], ferry_want, convoy_since, home_ground;
    CellXY ferry_seen[DIR_CONVOY];      /* where each transport last made headway, and when */
    CellXY ferry_dock_at[DIR_CONVOY];   /* where each docked transport loads */
    int ferry_seen_frame[DIR_CONVOY], dock_frame;
    CellXY landing, dock, dock_failed;   /* dock_failed: a beach the transports couldn't get to */
    int rally_frame;
    int third_party, last_attack_end, next_repick;          /* other enemies' units near the target's base */
    int naval_threat, want_navy;                            /* enemy ships near our buildings */
    int air_near, air_seen, own_aa, want_aa, next_aa_defense;   /* enemy aircraft near home or the army */
    int navy_state, navy_launch, navy_best, navy_progress, navy_bad_count, last_navy_log;   /* fleet attacks */
    BYTE *navy_target, *navy_bad[8];
    CellXY navy_target_at;
    int next_unstick, last_regroup_log;                                    /* units walled in by our own buildings */
    int refinery_queued, refinery_count, refinery_fails, refinery_backoff;
    BYTE *bunker_unit[8], *bunker_site[8];                  /* tanks sent into tank bunkers */
    int bunker_frame[8], next_bunker;
    BYTE *bunker_failed[8];                                 /* tanks that could not get in: skip a while */
    int bunker_failed_frame[8], bunker_failed_next, last_veto_log, last_pick_log, next_garrison;
    BYTE *garrison_unit[24], *garrison_site[24];                               /* infantry on their way into civilian buildings */
    int garrison_frame[24], garrison_next, garrison_held, want_occupier;
    CellXY stranded_at;                                     /* units cut off by a fallen bridge */
    BYTE *outpost_anchor, *outpost_type, *outpost_built;    /* ore outpost by a captured building */
    BYTE *post_unit[6];                                     /* Allied infantry dug in at the base edge */
    BYTE *col_target, *col_ferry, *col_eng, *col_failed[8];  /* colonising islands by transport */
    CellXY col_landing;
    int col_state, col_frame, next_col, want_col_ferry, col_failed_count;
    BYTE *col_return;                   /* a colonising transport sent home after giving up, to unload there */
    int col_tries;                      /* landing cells tried for this target */
    int col_return_frame;
    CellXY post_cell[6], post_rally;
    int post_toggled[6], next_posts;
    CellXY outpost_ore;
    int outpost_step, outpost_frame, next_outpost;
    int stranded_frame;
    /* strategy: the plan chosen at the start, and the posture that follows the game */
    int plan, plan_chosen, land_steps, posture, posture_frame, next_posture, next_hold_defense, opening;
    int plan_frame, next_replan, plan_roll;   /* when the plan was adopted; the next re-plan; the random stream */
    int plan_dropped;                     /* a rush called off before it struck (dir_rush_failing) */
    int threat_near, target_home;   /* armed enemy units within 35 cells of home; the target's near its base */
    int naval_pick, naval_pick_frame;   /* the shipyard's own order: UnitType index + 1 (0: none) */
    BYTE *breach_unit[2];               /* units shooting a gap in a fence for an engineer */
    CellXY miner_from, miner_site;      /* a Slave Miner packed up to move to richer ore */
    int inf_defense, raid_inf, raid_armor;  /* this raid is met with a barracks flood; what the raiders are */
    int miner_frame, next_miner;
    int breach_frame;
    int fleet_value, sea;               /* armed ships' value; our sea reaches enemy buildings */
    int enemy_subs;                     /* enemy submarines seen (Typhoons, Boomers) */
    int fleet_air;                      /* enemy aircraft over our ships */
    CellXY fleet_at;                    /* the fleet's centre */
    int escape_frame, next_escape;      /* a construction yard packed up to flee */
    BYTE *cover_unit[3];                /* Floating Discs flying cover over Yuri's fleet */
    int cover_frame;
};
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

/* A plan with a phase has run out: its time is up, a rush's first attack is over, or a rush fell
 * too far behind to strike at all (and stays called off: the house plans again). */
static int dir_plan_over(DirState *d)
{
    const DirPlanLevers *p = &dir_plan_levers[d->plan];
    if (d->plan == PLAN_RUSH && !d->plan_dropped && d->state != DIR_ATTACK
        && dir_rush_failing(CURRENT_FRAME - d->plan_frame, d->army_value, d->target_army)) {
        d->plan_dropped = 1;
        logmsg("director: house %d frame %d calls the rush off (army %d, the target's %d)", FIELD(d->house, 0x30, int),
               CURRENT_FRAME, d->army_value, d->target_army);
    }
    return p->phase && (CURRENT_FRAME - d->plan_frame >= p->phase || d->plan_dropped
                        || (d->plan == PLAN_RUSH && d->last_attack_end > d->plan_frame));
}

/* The levers in force: the plan's, or balanced once a phased plan has run out (until the house
 * plans again). Without the strategy feature every house plays balanced: the director as it was. */
static const DirPlanLevers *dir_levers(DirState *d)
{
    if (!d || !(director_enabled(d->house) & DIR_F_STRATEGY) || !d->plan_chosen || dir_plan_over(d))
        return &dir_plan_levers[PLAN_BALANCED];
    return &dir_plan_levers[d->plan];
}

/* For combat-ai.h, which adds war factories: the plan's scale on their cash thresholds. */
static int dir_factory_cash_pct(BYTE *house)
{
    int idx = FIELD(house, 0x30, int);
    return idx >= 0 && idx < 32 && dir_state[idx].house == house ? dir_levers(&dir_state[idx])->factory_cash_pct : 100;
}

/* Sell (or, for a building with UndeploysInto, pack up). BuildingClass::Sell (0x447110) does nothing
 * unless +0x6E9 is set, which building setup (0x442CCF) sets only when the type's build-up art is
 * there (type vtable +0xC0, after 0x465AF0 loads it). Buildings the starting base puts down when the
 * scenario loads miss it, so they could never be sold or packed up: load it and set it the same way. */
static void dir_sell(BYTE *b)
{
    BYTE *type = FIELD(b, B_TYPE, BYTE *);
    if (!b[0x6E9] && type) {
        ((void (GTHISCALL *)(BYTE *))0x465AF0)(type);
        if (((int (GTHISCALL *)(BYTE *))VFUNC(type, 0xC0))(type))
            b[0x6E9] = 1;
    }
    int before = FIELD(b, COMBAT_MISSION, int);
    ((void (GTHISCALL *)(BYTE *, int))VFUNC(b, VT_SELL))(b, 1);
    if (bench_file)
        logmsg("director: sell %.24s: flag %d buildup %d mission %d -> %d", type ? (char *)type + T_ID : "?", b[0x6E9],
               type ? ((int (GTHISCALL *)(BYTE *))VFUNC(type, 0xC0))(type) : -1, before, FIELD(b, COMBAT_MISSION, int));
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

#define HT_MULTIPLAY_PASSIVE 0x1A6   /* HouseTypeClass::MultiplayPassive: Neutral, Special (civilians) */

/* A civilian house: never an enemy to fight, though its tech buildings can be captured. */
static int dir_passive(BYTE *owner)
{
    BYTE *type = owner ? FIELD(owner, 0x34, BYTE *) : NULL;
    return type && type[HT_MULTIPLAY_PASSIVE];
}

static int dir_hostile(BYTE *house, BYTE *owner)
{
    if (!owner || owner == house || owner[OIL_H_DEFEATED] || dir_passive(owner))
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

#define T_RECRUITABLE 0x421           /* TechnoClass::RecruitableA: FootClass::CanBeRecruited returns it */

static int dir_team_takeable(BYTE *obj)
{
    BYTE *team = FIELD(obj, F_TEAM, BYTE *);
    if (!team)
        return 1;
    if (!(director_enabled(FIELD(obj, O_OWNER, BYTE *)) & DIR_F_TAKEOVER))
        return 0;
    BYTE *type = FIELD(team, TEAM_TYPE, BYTE *), *script = FIELD(team, TEAM_SCRIPT, BYTE *);
    if (!type || !script || type[TT_BASE_DEFENSE])
        return 0;
    BYTE *script_type = FIELD(type, TT_SCRIPT, BYTE *);
    const char *id = script_type ? (char *)script_type + T_ID : "";
    return _strnicmp(id, "0F1BC", 5) && _stricmp(id, "0F1BF007-G");
}

static int dir_take_from_team(BYTE *obj)
{
    BYTE *team = FIELD(obj, F_TEAM, BYTE *);
    if (!team)
        return 1;
    if (!(director_enabled(FIELD(obj, O_OWNER, BYTE *)) & DIR_F_TAKEOVER))
        return 0;
    /* Attack teams still recruiting (script line -1) are taken too: one waiting for a unit type it
     * never gets holds a dozen units idle in the base, where they jam. A unit taken over is marked
     * unrecruitable, so no team pulls it back and the two never trade orders. */
    BYTE *type = FIELD(team, TEAM_TYPE, BYTE *), *script = FIELD(team, TEAM_SCRIPT, BYTE *);
    if (!type || !script || type[TT_BASE_DEFENSE])
        return 0;
    BYTE *script_type = FIELD(type, TT_SCRIPT, BYTE *);
    const char *id = script_type ? (char *)script_type + T_ID : "";
    if (!_strnicmp(id, "0F1BC", 5) || !_stricmp(id, "0F1BF007-G"))
        return 0;
    ((char (GTHISCALL *)(BYTE *, BYTE *, int, char))TEAM_LIBERATE)(team, obj, -1, 0);
    if (FIELD(obj, F_TEAM, BYTE *))
        return 0;
    obj[T_RECRUITABLE] = 0;
    return 1;
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
    if (!type || in_list("HARV,CMIN,SMIN,SLAV,AMCV,SMCV,PCV,SBDOZR,ENGINEER,SENGINEER,YENGINEER,SPY,"
                         "IVAN,CIVAN,TERROR,SHAD,JUMPJET,CCOMAND,TANY,BORIS,GHOST,YURIPR,"
                         "CARRIER,DEST,SUB,AEGIS,LCRF,DRED,SQD,DLPH,HYD,BSUB,SAPC,YHVR,VIRUS,DTRUCK",
                         (char *)type + T_ID))
        return 0;
    if (what == 1 && type[UT_HARVESTER])
        return 0;   /* any other harvester the list doesn't name */
    if (FIELD(obj, T_BUNKER_LINK, BYTE *))
        return 0;   /* garrisoning a tank bunker */
    int mission = FIELD(obj, COMBAT_MISSION, int);
    /* 7 Enter, 8 Capture, 16 Unload, 10 Harvest: busy with something the AI chose deliberately */
    return mission != 7 && mission != 8 && mission != 10 && mission != 16 && dir_take_from_team(obj);
}

/* ---- production ---- */

static const char *dir_vehicle_roles[3][ROLE_COUNT] = {
    /* Mirage first: the Liberator, at 375 HP and half its fire rate, lost more than it killed (0.89) */
    { "MGTK,TNKD,MTNK,ATTNK", "FV", "SREF", "MGTK,FV" },
    { "APOC,TTNK,HTNK", "HTK", "V3", "HTK" },
    { "MIND,LTNK", "YTNK", "TELE", "YTNK" },
};
/* Enemy out of reach by land: Robot Tanks hover over water, Kirovs, Siege Choppers and Discs fly. */
static const char *dir_blocked_main[3] = { "ROBO", "SCHP,ZEP", "DISK" };   /* Siege Choppers before Kirovs */
static const char *dir_infantry[3] = { "GGI,E1", "SHK,E2", "BRUTE,INIT" };
static const char *dir_warships[3] = { "CARRIER,DEST", "SUB,HYD", "BSUB" };   /* navy answers enemy ships */
/* Anti-air answers to Kirovs, Discs, Harriers and Rocketeers, the same way for every side. */
static const char *dir_aa_vehicles[3] = { "FV", "HTK", "YTNK" };
static const char *dir_aa_infantry[3] = { "GGI", "FLAKT", "" };
static const char *dir_aa_defenses[3] = { "NASAM", "NAFLAK", "YAGGUN" };   /* the Allied Patriot is NASAM */

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

static int dir_naval(BYTE *obj);

static int dir_armed_vehicles(BYTE *house)
{
    int n = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i];
        n += oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_whatami(o) == 1 && dir_armed(o) && !dir_naval(o);
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
                && (_stricmp(id, "ATTNK") || dir_owned_of(house, type) < 2)    /* slow and costly: a pair at most */
                && (_stricmp(id, "ZEP") || dir_owned_of(house, type) < 4)      /* Kirovs: slow and costly */
                /* Tank Destroyers only hurt vehicles: a third of the tanks at most, never the army */
                && (_stricmp(id, "TNKD") || dir_owned_of(house, type) * 3 < dir_armed_vehicles(house) + 3))
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

/* Cut off by water, ground vehicles only fight what reaches our shore or what the ferry carries
 * across a few at a time: past a home garrison, more of them are money with no plan. */
#define DIR_HOME_VEHICLES 16
static int dir_landed(DirState *d, CellXY c);

static int dir_ground_full(BYTE *house, DirState *d)
{
    if (!(d->blocked || d->island) || d->threat_value >= 1500)
        return 0;
    int n = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i];
        n += oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_whatami(o) == 1 && dir_armed(o)
            && !dir_naval(o) && !dir_crosses(o) && !dir_landed(d, object_cell(o));
    }
    return n >= DIR_HOME_VEHICLES;
}

/* One refinery after the opening and too poor for a second: units wait until it is paid for. */
static int dir_saving_for_refinery(BYTE *house)
{
    return CURRENT_FRAME > 4500 && FIELD(house, OIL_H_REFINERIES, int) < 2 && FIELD(house, H_HARVESTERS, int) >= 1
        && FIELD(house, OIL_H_CASH, int) < 2500;
}

static void dir_choose_vehicle(BYTE *house, DirState *d)
{
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2 || dir_saving_for_refinery(house))
        return;
    int shares[ROLE_COUNT], have[ROLE_COUNT] = { 0 }, available[ROLE_COUNT];
    BYTE *pick[ROLE_COUNT];
    dir_role_shares(d->enemy_air, d->enemy_inf, d->enemy_armor, d->enemy_def, shares);
    dir_plan_shares(dir_levers(d), shares);
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i];
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 1)
            continue;
        BYTE *type = dir_type(o);
        if (type && dir_armed(o))
            have[dir_role_of(side, (char *)type + T_ID)] += dir_cost(type);
    }
    int ground_full = dir_ground_full(house, d);
    for (int r = 0; r < ROLE_COUNT; r++) {
        /* cut off by water: hover/air units carry the attack (and the siege), unless the base is
         * being hit, when ground units defend it first; the other roles only up to a home garrison */
        int cross = (d->blocked || d->island) && (r == ROLE_MAIN || r == ROLE_SIEGE) && d->threat_value < 1500;
        pick[r] = cross || !ground_full
            ? dir_first_buildable(house, UNITTYPE_ARRAY, cross ? dir_blocked_main[side] : dir_vehicle_roles[side][r], 2500)
            : NULL;
        available[r] = pick[r] != NULL;
    }
    int role = dir_pick_role(shares, have, available);
    if (bench_file && CURRENT_FRAME - d->last_pick_log > 1500) {
        d->last_pick_log = CURRENT_FRAME;
        logmsg("director: house %d frame %d pick: cash %d shares %d/%d/%d/%d have %d/%d/%d/%d picks %s %s %s %s -> %d",
               FIELD(house, 0x30, int), CURRENT_FRAME, FIELD(house, OIL_H_CASH, int), shares[0], shares[1], shares[2],
               shares[3], have[0], have[1], have[2], have[3],
               pick[0] ? (char *)pick[0] + T_ID : "-", pick[1] ? (char *)pick[1] + T_ID : "-",
               pick[2] ? (char *)pick[2] + T_ID : "-", pick[3] ? (char *)pick[3] + T_ID : "-", role);
    }
    if (role < 0)
        return;
    int index = dir_type_index(UNITTYPE_ARRAY, pick[role]);
    if (index < 0)
        return;
    FIELD(house, H_PRODUCING_UNIT, int) = index;
    d->unit_request = index;
    d->unit_request_frame = CURRENT_FRAME;
}

static void dir_veto_production(BYTE *house, DirState *d);

static int dir_ferry_count(DirState *d)
{
    int n = 0;
    for (int k = 0; k < DIR_CONVOY; k++)
        n += d->ferry[k] != NULL;
    return n;
}

static int dir_is_ferry(DirState *d, BYTE *o)
{
    for (int k = 0; k < DIR_CONVOY; k++)
        if (d->ferry[k] == o)
            return 1;
    return 0;
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
    dir_veto_production(house, d);   /* right after the stock pick */
    int current = FIELD(house, H_PRODUCING_UNIT, int);
    /* A request the factory never picked up would block the queue; drop it after a while. */
    if (current != -1 && current == d->unit_request && CURRENT_FRAME - d->unit_request_frame > 3000) {
        FIELD(house, H_PRODUCING_UNIT, int) = -1;
        current = -1;
    }
    /* One-off orders outrank the stock team picks, which otherwise never leave the queue free. */
    int side0 = FIELD(house, OIL_H_SIDE, int);
    BYTE *warship = d->want_navy && side0 >= 0 && side0 <= 2
        ? dir_first_buildable(house, UNITTYPE_ARRAY, dir_warships[side0], 1000) : NULL;
    if (!warship && d->want_aa && side0 >= 0 && side0 <= 2)   /* the air raid outranks stock picks too */
        warship = dir_first_buildable(house, UNITTYPE_ARRAY, dir_aa_vehicles[side0], 0);
    if (!warship && side0 == 2 && d->fleet_air >= 1500 && !dir_owned_of(house, find_type(UNITTYPE_ARRAY, "DISK")))
        warship = dir_first_buildable(house, UNITTYPE_ARRAY, "DISK", 0);   /* air cover for the fleet */
    if (!warship && d->want_col_ferry && side0 >= 0 && side0 <= 2)   /* a transport to colonise an island */
        warship = dir_first_buildable(house, UNITTYPE_ARRAY, dir_transports[side0], 1000);
    BYTE *urgent = d->want_mcv ? dir_first_buildable(house, UNITTYPE_ARRAY, dir_mcvs, 0)
        : (d->island || d->blocked) && dir_ferry_count(d) < d->ferry_want && side0 >= 0 && side0 <= 2
        ? dir_first_buildable(house, UNITTYPE_ARRAY, dir_transports[side0], 1000) : warship;
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
    if (current == -1 && (d->island || d->blocked) && dir_ferry_count(d) < d->ferry_want && side >= 0 && side <= 2) {
        BYTE *type = dir_first_buildable(house, UNITTYPE_ARRAY, dir_transports[side], 1000);
        int index = type ? dir_type_index(UNITTYPE_ARRAY, type) : -1;
        if (index >= 0) {
            FIELD(house, H_PRODUCING_UNIT, int) = current = d->unit_request = index;
            d->unit_request_frame = CURRENT_FRAME;
        }
    }
    if (current == -1 && warship) {
        int index = dir_type_index(UNITTYPE_ARRAY, warship);
        if (index >= 0) {
            FIELD(house, H_PRODUCING_UNIT, int) = current = d->unit_request = index;
            d->unit_request_frame = CURRENT_FRAME;
        }
    }
    /* The MCV is bought as an urgent order when cash allows; the army is never paused for it. */
    if (current == -1)
        dir_choose_vehicle(house, d);
    /* Ships come from the naval yard, but stock production keeps one vehicle order for war
     * factories and yards alike: a ship in it idled every war factory until the ship was out
     * (France on Isolation sat on 40k for 20000 frames behind Carriers). The ship moves to the
     * yard's own order (dir_factory_pick), and the war factories get a ground unit at once. */
    DynVec *ut = UNITTYPE_ARRAY;
    if (d->naval_pick && CURRENT_FRAME - d->naval_pick_frame > 3000)
        d->naval_pick = 0;   /* no yard took it: no yard, or it could not be built */
    /* A naval plan keeps a war fleet beside the army, in the yard's own order. Capital ships alone
     * sat helpless under aircraft and submarines, so escorts fill their share first: anti-air
     * (Aegis, Sea Scorpion; Yuri has none) and anti-submarine (Destroyer, Typhoon, Boomer), each 20%
     * of the fleet, 35% once enemy aircraft or submarines are about. Capital ships: Carriers (their
     * Hornets traded 3.8), Dreadnoughts (missiles 1.6), Boomers. */
    static const char *fleet_main[3] = { "CARRIER", "DRED", "BSUB" }, *fleet_aa[3] = { "AEGIS", "HYD", "" },
                      *fleet_asw[3] = { "DEST", "SUB", "BSUB" };
    int share = dir_levers(d)->navy_share;
    if (share && !d->naval_pick && side >= 0 && side <= 2
        && (long long)d->fleet_value * 100 < (long long)(d->army_value + d->fleet_value) * share) {
        int main_v = 0, aa_v = 0, asw_v = 0, subs = 0;
        DynVec *tv = OIL_TECHNO_ARRAY;
        for (int i = 0; i < tv->Count; i++) {
            BYTE *o = tv->Items[i], *ot;
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || !dir_naval(o) || !(ot = dir_type(o)))
                continue;
            const char *sid = (char *)ot + T_ID;
            if (*fleet_aa[side] && in_list(fleet_aa[side], sid))
                aa_v += dir_cost(ot);
            else if (in_list(fleet_asw[side], sid) && !in_list(fleet_main[side], sid))
                asw_v += dir_cost(ot);
            else if (in_list(fleet_main[side], sid))
                main_v += dir_cost(ot);
        }
        subs = d->enemy_subs;
        int total = main_v + aa_v + asw_v + 1, aa_share = d->enemy_air > 2000 ? 35 : 20, asw_share = subs ? 35 : 20;
        const char *pick = fleet_main[side];
        if (*fleet_aa[side] && aa_v * 100 < total * aa_share)
            pick = fleet_aa[side];
        else if (strcmp(fleet_asw[side], fleet_main[side]) && asw_v * 100 < total * asw_share)
            pick = fleet_asw[side];
        BYTE *ship = dir_first_buildable(house, UNITTYPE_ARRAY, pick, 2500);
        if (!ship)
            ship = dir_first_buildable(house, UNITTYPE_ARRAY, fleet_main[side], 2500);
        if (ship) {
            d->naval_pick = dir_type_index(UNITTYPE_ARRAY, ship) + 1;
            d->naval_pick_frame = CURRENT_FRAME;
        }
    }
    current = FIELD(house, H_PRODUCING_UNIT, int);
    if (current >= 0 && current < ut->Count && ((BYTE *)ut->Items[current])[TT_NAVAL]) {
        if (!d->naval_pick) {
            d->naval_pick = current + 1;
            d->naval_pick_frame = CURRENT_FRAME;
        }
        FIELD(house, H_PRODUCING_UNIT, int) = -1;
        if (d->unit_request == current)
            d->unit_request = -1;
        dir_choose_vehicle(house, d);
    }
    return result;
}

/* BuildingClass's factory AI asks the house what to build (HouseClass 0x4FBD80, called at
 * 0x45032D with the factory's Factory= RTTI; the second argument is always 0) and only then checks that the
 * unit's Naval matches the factory's. A naval yard of a director house is handed the yard's own
 * order first. The building's type is still in EDX at the call, hence fastcall. */
#define DIR_FACTORY_PICK_CALL 0x45032D
#define DIR_PRIMARY_IN_PRODUCTION 0x4FBD80
static BYTE *GFASTCALL dir_factory_pick(BYTE *house, BYTE *btype, int rtti, int zero)
{
    BYTE *type = ((BYTE *(GTHISCALL *)(BYTE *, int, int))DIR_PRIMARY_IN_PRODUCTION)(house, rtti, zero);
    /* a factory's Factory= is a type RTTI: UnitType is 0x28 (0x4FBD80 maps it and Unit, 1, alike) */
    if ((rtti != 1 && rtti != 0x28) || !btype || !btype[TT_NAVAL] || !dir_active(house)
        || !(director_enabled(house) & DIR_F_PRODUCTION))
        return type;
    DirState *d = dir_get(house);
    DynVec *ut = UNITTYPE_ARRAY;
    if (!d || d->naval_pick <= 0 || d->naval_pick > ut->Count)
        return type;
    BYTE *ship = ut->Items[d->naval_pick - 1];
    d->naval_pick = 0;   /* one ship per order: this yard starts it now */
    return ship;
}

static void dir_veto_production(BYTE *house, DirState *d);

static int GFASTCALL dir_inf_production(BYTE *house, void *unused)
{
    (void)unused;
    int result = dir_inf_original(house);
    DirState *dd = dir_get(house);
    if (dd && dir_active(house) && (director_enabled(house) & DIR_F_PRODUCTION)) {
        dir_veto_production(house, dd);   /* right after the stock pick, before a factory takes it */
        int s0 = FIELD(house, OIL_H_SIDE, int);
        BYTE *aa = dd->want_aa && s0 >= 0 && s0 <= 2 && *dir_aa_infantry[s0]
            ? dir_first_buildable(house, INFANTRYTYPE_ARRAY, dir_aa_infantry[s0], 0) : NULL;
        int index = aa ? dir_type_index(INFANTRYTYPE_ARRAY, aa) : -1;
        if (index >= 0) {   /* an air raid: anti-air infantry instead of the stock pick */
            FIELD(house, H_PRODUCING_INF, int) = index;
            return result;
        }
        /* a barracks flood against a raid: anti-tank infantry against vehicles, cheap infantry
         * against infantry; past the usual cap, while the raid lasts */
        static const char *anti_armor[3] = { "GGI,E1", "SHK,E2", "BRUTE,INIT" }, *anti_inf[3] = { "E1", "E2", "INIT" };
        if (dd->inf_defense && s0 >= 0 && s0 <= 2 && FIELD(house, H_OWNED_INFANTRY, int) < 60) {
            BYTE *t = dir_first_buildable(house, INFANTRYTYPE_ARRAY,
                                          dd->raid_armor >= dd->raid_inf ? anti_armor[s0] : anti_inf[s0], 1500);
            index = t ? dir_type_index(INFANTRYTYPE_ARRAY, t) : -1;
            if (index >= 0) {
                FIELD(house, H_PRODUCING_INF, int) = index;
                return result;
            }
        }
    }
    /* cut off by water, infantry can't reach the enemy beyond a ferry load or two: keep a garrison */
    int cap = dd && (dd->blocked || dd->island) ? 16 : 30;
    if (!dir_active(house) || !(director_enabled(house) & DIR_F_PRODUCTION) || FIELD(house, H_PRODUCING_INF, int) != -1
        || (FIELD(house, H_OWNED_INFANTRY, int) >= cap && !(dd && dd->want_engineer)))
        return result;
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2)
        return result;
    DirState *d = dir_get(house);
    static const char *engineers[3] = { "ENGINEER", "SENGINEER", "YENGINEER" };
    static const char *occupiers[3] = { "E1", "E2", "INIT" };
    BYTE *type = d && d->want_engineer ? dir_first_buildable(house, INFANTRYTYPE_ARRAY, engineers[side], 0) : NULL;
    if (!type && d && d->want_occupier)
        type = dir_first_buildable(house, INFANTRYTYPE_ARRAY, occupiers[side], 500);
    if (!type && !dir_saving_for_refinery(house))
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
    int value, armed, building, air, infantry, focus, capturable, range, naval, threat;
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

static CellXY dir_house_center(BYTE *house);

/* Per-house army and defense totals, for choosing whom to fight (dir_house_strengths). */
static int dir_house_army[32], dir_house_def[32];

static int dir_naval(BYTE *obj)
{
    BYTE *type = dir_type(obj);
    return type && type[TT_NAVAL];
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
    d->threat_value = d->naval_threat = d->third_party = d->air_near = 0;
    d->threat_near = d->target_home = d->raid_inf = d->raid_armor = d->enemy_subs = 0;
    CellXY target_base = d->enemy ? dir_house_center(d->enemy) : (CellXY){ 0, 0 };
    int threat_best = 0x7FFFFFFF;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count && dir_enemy_count < MAX_ENEMY; i++) {
        BYTE *o = v->Items[i];
        BYTE *owner0 = oil_live(o) ? FIELD(o, O_OWNER, BYTE *) : NULL;
        int neutral = dir_passive(owner0);
        if (!owner0 || (!neutral && !dir_hostile(house, owner0)))
            continue;
        int what = dir_whatami(o);
        BYTE *type = dir_type(o);
        if (!type)
            continue;
        /* civilians: only their tech buildings matter (engineers capture them); the army leaves
         * their houses alone, so infantry can garrison them instead */
        if (neutral && (what != 6 || !in_list("CAOILD,CAAIRP,CATHOSP,CAOUTP,CAMACH,CAPOWR", (char *)type + T_ID)))
            continue;
        DirEnemy *e = &dir_enemies[dir_enemy_count++];
        e->obj = o;
        e->at = object_cell(o);
        e->value = dir_cost(type);
        e->armed = !neutral && dir_armed(o);
        e->range = e->armed ? dir_weapon_cells(o) : 0;
        e->building = what == 6;
        /* tech buildings are worth more captured: derricks pay forever, the others give units/repair */
        e->capturable = e->building && in_list("CAOILD,CAAIRP,CATHOSP,CAOUTP,CAMACH,CAPOWR", (char *)type + T_ID);
        e->infantry = what == 15;
        e->focus = e->threat = 0;
        e->naval = !e->building && type[TT_NAVAL];
        d->enemy_subs += e->naval && in_list("SUB,BSUB", (char *)type + T_ID);
        Coord c = FIELD(o, O_LOCATION, Coord);
        int floor = ((int (GTHISCALL *)(void *, Coord *))MAP_FLOOR_HEIGHT)(MAP_INSTANCE, &c);
        e->air = what == 2 || c.Z > floor + 128;
        if (!e->armed)
            continue;
        BYTE *owner = FIELD(o, O_OWNER, BYTE *);
        if (e->building) {
            if (owner == d->enemy)   /* the target's defenses, for launch and siege share */
                d->enemy_def += e->value;
            continue;
        }
        d->enemy_army += e->value;
        /* across water only what can reach us counts: hover/air units, or units already ashore */
        if (!e->air && !e->naval && dir_dist2(e->at, d->base) <= 35 * 35
            && (!(d->island || d->blocked) || dir_crosses(o) || dir_near_own_building(house, e->at, 15)))
            d->threat_near += e->value;
        if (owner == d->enemy && dir_dist2(e->at, target_base) <= 30 * 30)
            d->target_home += e->value;
        if (owner == d->enemy)
            d->target_army += e->value;
        else if (d->enemy && dir_dist2(e->at, target_base) <= 25 * 25)
            d->third_party += e->value;   /* a third player fighting there too: in a free-for-all the
                                           * rest of the map's armies are not this attack's business */
        if (e->air)
            d->enemy_air += e->value;
        if (e->air && (dir_near_own_building(house, e->at, 25) || dir_dist2(e->at, d->front) <= 25 * 25))
            d->air_near += e->value;
        else if (e->infantry)
            d->enemy_inf += e->value;
        else
            d->enemy_armor += e->value;
        if (e->naval) {
            /* ships shell the base from the water, often from beyond 10 cells: the navy answers,
             * not the ground army, which would only crowd the shore */
            int r = e->range + 2 > 10 ? e->range + 2 : 10;
            if (dir_near_own_building(house, e->at, r)) {
                d->naval_threat += e->value;
                e->threat = 1;
            }
        } else if (!e->air && dir_near_own_building(house, e->at, 10)) {
            d->threat_value += e->value;
            e->threat = 1;
            if (e->infantry)
                d->raid_inf += e->value;
            else
                d->raid_armor += e->value;
            /* the raid closest to home: the mean of raids on two sides lands inside the base */
            int dd = dir_dist2(e->at, d->base);
            if (dd < threat_best) {
                threat_best = dd;
                d->threat_at = e->at;
            }
        }
    }
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

static int dir_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

/* How attractive a hostile house is as the next target (lower is better): near and weak. */
static int dir_enemy_rating(BYTE *h, CellXY base)
{
    int idx = FIELD(h, 0x30, int) & 31;
    return dir_enemy_score(dir_isqrt(dir_dist2(base, dir_house_center(h))), dir_house_army[idx] + dir_house_def[idx] / 2);
}

static void dir_house_strengths(void)
{
    memset(dir_house_army, 0, sizeof dir_house_army);
    memset(dir_house_def, 0, sizeof dir_house_def);
    DynVec *t = OIL_TECHNO_ARRAY;
    for (int i = 0; i < t->Count; i++) {
        BYTE *o = t->Items[i], *owner, *type;
        if (!oil_live(o) || !(owner = FIELD(o, O_OWNER, BYTE *)) || !dir_armed(o) || !(type = dir_type(o)))
            continue;
        int idx = FIELD(owner, 0x30, int) & 31;
        if (dir_whatami(o) == 6)
            dir_house_def[idx] += dir_cost(type);
        else
            dir_house_army[idx] += dir_cost(type);
    }
}

static BYTE *dir_pick_enemy(BYTE *house, CellXY base)
{
    dir_house_strengths();
    DynVec *v = HOUSE_ARRAY;
    BYTE *best = NULL;
    int best_score = 0x7FFFFFFF;
    for (int i = 0; i < v->Count; i++) {
        BYTE *h = v->Items[i];
        if (!dir_hostile(house, h) || !dir_house_alive(h))
            continue;
        int score = dir_enemy_rating(h, base);
        if (score < best_score) {
            best_score = score;
            best = h;
        }
    }
    return best;
}

/* The enemy structure closest to the army: attack the edge of the base first. */
static BYTE *dir_pick_objective(DirState *d, CellXY from)
{
    BYTE *best = NULL;
    int best_score = 0x7FFFFFFF, buildings = 0, eco = dir_levers(d)->eco_targets;
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
        /* production and income first: a refinery, factory or yard up to ~1.4x as far beats the
         * nearest wall or power plant */
        if (eco && e->building && in_list("GAREFN,NAREFN,YAREFN,GAWEAP,NAWEAP,YAWEAP,GACNST,NACNST,YACNST",
                                          (char *)dir_type(e->obj) + T_ID))
            score /= 2;
        if (score < best_score) {
            best_score = score;
            best = e->obj;
            d->objective_at = e->at;
        }
    }
    return best;
}

/* The target enemy's nearest structure to c (capturable tech buildings aside), or NULL. */
static BYTE *dir_nearest_structure(DirState *d, CellXY c, CellXY *at)
{
    BYTE *best = NULL;
    int best_d = 0x7FFFFFFF;
    for (int i = 0; i < dir_enemy_count; i++) {
        DirEnemy *e = &dir_enemies[i];
        int dd;
        if (e->building && !e->capturable && FIELD(e->obj, O_OWNER, BYTE *) == d->enemy
            && (dd = dir_dist2(c, e->at)) < best_d) {
            best_d = dd;
            best = e->obj;
            *at = e->at;
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

/* Benchmark diagnostics: units given many orders while barely moving (wiggling in place). */
static const char *dir_why = "other";
static struct { BYTE *unit; CellXY at; int frame, count; const char *last; int logged; } dir_churn[1024];

static void dir_note_order(BYTE *unit)
{
    if (!bench_file)
        return;
    unsigned k = ((DWORD)unit >> 3) % 1024;
    CellXY at = object_cell(unit);
    if (dir_churn[k].unit != unit || CURRENT_FRAME - dir_churn[k].frame > 600 || dir_dist2(dir_churn[k].at, at) > 3 * 3) {
        dir_churn[k].unit = unit;
        dir_churn[k].at = at;
        dir_churn[k].frame = CURRENT_FRAME;
        dir_churn[k].count = 0;
        dir_churn[k].logged = 0;
    }
    dir_churn[k].count++;
    if (dir_churn[k].count >= 12 && !dir_churn[k].logged) {
        dir_churn[k].logged = 1;
        logmsg("director: frame %d: %.24s at %d,%d got %d orders in %d frames without moving (last: %s, mission %d)",
               CURRENT_FRAME, (char *)dir_type(unit) + T_ID, at.X, at.Y, dir_churn[k].count,
               CURRENT_FRAME - dir_churn[k].frame, dir_why, FIELD(unit, COMBAT_MISSION, int));
    }
}

static void dir_order(BYTE *unit, int mission, BYTE *target, BYTE *cell)
{
    dir_note_order(unit);
    ((char (GTHISCALL *)(BYTE *, int, BYTE *, BYTE *, BYTE *))VFUNC(unit, VT_CLICKEDMISSION))(
        unit, mission, target, cell, NULL);
}

/* The last cell each unit was sent to guard: a goal that drifts (a moving raider, the marching body)
 * is the same order while it stays within 6 cells, so the unit is not restarted every tick. */
static struct { BYTE *unit; CellXY goal; int frame; } dir_goals[4096];
static struct { BYTE *unit; int frame; } dir_engaged[4096];   /* last engagement order */

static int dir_same_goal(BYTE *unit, CellXY goal)
{
    unsigned k = ((DWORD)unit >> 3) % 4096;
    if (dir_goals[k].unit == unit && CURRENT_FRAME - dir_goals[k].frame < 450 && dir_dist2(dir_goals[k].goal, goal) <= 6 * 6)
        return 1;
    dir_goals[k].unit = unit;
    dir_goals[k].goal = goal;
    dir_goals[k].frame = CURRENT_FRAME;
    return 0;
}

/* Attack-move: fight whatever is worth fighting near the unit, else keep heading for the goal. */
static int dir_flags;   /* the commanding house's DirectorFlags, set by dir_army */

static void dir_command(BYTE *unit, CellXY goal, BYTE *goal_obj, int engage)
{
    CellXY at = object_cell(unit);
    BYTE *current = FIELD(unit, O_TARGET, BYTE *);
    int reach = dir_weapon_cells(unit) + 3, best_score = -1000000, keep_score = -1000000;
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
            /* mind control: kill the controller (Yuri, Yuri Prime, Mastermind, Psychic Tower) and its
             * captives switch back; while it is close by, its captives are the wrong target */
            if (FIELD(e->obj, T_CAPTURE_MANAGER, BYTE *))
                priority += 80;
            BYTE *controller = FIELD(e->obj, T_MIND_CONTROLLED_BY, BYTE *);
            if (controller && oil_live(controller)
                && dir_dist2(object_cell(controller), at) <= (reach + 6) * (reach + 6))
                priority -= 60;
            int strength = FIELD(dir_type(e->obj), OT_STRENGTH, int);
            int hurt = strength > 0 ? 40 - 40 * FIELD(e->obj, O_HEALTH, int) / strength : 0;
            int score = priority + hurt + (e->focus < 5 ? e->focus * 8 : -40) - dd / 4;
            if (e->capturable)
                continue;   /* an engineer will take it; don't shoot what we want to own */
            if (e == keep)
                keep_score = score;
            if (score > best_score && dir_can_fire_at(unit, e->obj)) {
                best_score = score;
                best = e;
            }
        }
    }
    /* Stay on a live armed target; switch from buildings or harmless targets to a real threat. */
    /* ...and a target is only swapped for a clearly better one: near-equal scores shift every tick
     * with focus counts, and each swap was a new order */
    if (keep && ((keep->armed && !keep->building) || !best || !best->armed || best->building
                 || best_score < keep_score + 30)) {
        keep->focus++;
        return;
    }
    if (best) {
        best->focus++;
        /* a new attack order needs the unit's target to settle first: before it does, the choice
         * can flip between two enemies every tick, and each flip restarted the unit */
        unsigned k = ((DWORD)unit >> 3) % 4096;
        if (dir_engaged[k].unit == unit && CURRENT_FRAME - dir_engaged[k].frame < 60)
            return;
        if (!dir_recent_order(unit, best->obj, 90)) {
            dir_order(unit, MISSION_ATTACK, best->obj, NULL);
            dir_engaged[k].unit = unit;
            dir_engaged[k].frame = CURRENT_FRAME;
        }
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
    int guarding = FIELD(unit, COMBAT_MISSION, int) == MISSION_AREA_GUARD;
    BYTE *focus = FIELD(unit, T_FOCUS, BYTE *);
    if (guarding && (focus == cell || dir_dist2(at, goal) <= 4 * 4
                     || (focus && is_cell(focus) && dir_dist2(FIELD(focus, C_MAPCOORDS, CellXY), goal) <= 4 * 4)))
        return;   /* a goal that drifts a few cells (a defence point on a moving raider) is the same order */
    /* The engine moves Focus while it guards, so Focus alone can't tell an order already given:
     * re-queueing every tick restarted the unit, which wiggled in place. Our own order memory
     * leaves it to work for a while (it may be fighting on the way). */
    if (dir_same_goal(unit, goal))
        return;
    FIELD(unit, T_FOCUS, BYTE *) = cell;
    dir_note_order(unit);
    ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(unit, VT_QUEUEMISSION))(unit, MISSION_AREA_GUARD, 1);
}

/* A unit the ferry has put across: nearer the enemy's base than to ours. */
static int dir_landed(DirState *d, CellXY c)
{
    return d->enemy && dir_dist2(c, FIELD(d->enemy, H_BASE_CENTER, CellXY)) < dir_dist2(c, d->base);
}

/* ---- units walled in ----
 * The base planner packs buildings shoulder to shoulder, and a unit that rolls out into a pocket
 * between them and the shore can never leave. A unit that has not moved for a long time while its
 * goal is far away and nothing is in its sights is stuck; selling a cheap building next to it (a
 * wall, or a power plant while others remain) opens the way, as a player would. */
#define DIR_STUCK 2048
static int dir_bridge_down_near(CellXY at, int r);
static struct { BYTE *unit; CellXY at; int frame, stranded; } dir_moves[DIR_STUCK];

/* Stranded: stuck on a section cut off by a fallen bridge. It guards where it stands instead of
 * pathing at an impossible goal, and engineers go for that bridge first. Re-checked every 600
 * frames, so a repaired bridge brings it back into the army. */
static int dir_stranded(BYTE *unit)
{
    unsigned h = ((DWORD)unit >> 3) % DIR_STUCK;
    for (int i = 0; i < 8; i++) {
        unsigned j = (h + i) % DIR_STUCK;
        if (dir_moves[j].unit == unit)
            return dir_moves[j].stranded && CURRENT_FRAME - dir_moves[j].stranded < 600;
    }
    return 0;
}

#define C_LANDTYPE 0xEC                /* CellClass::LandType: 2 water, 3 rock, 4 wall */
#define C_FLAGS 0x140                  /* CellClass::Flags: 0x100 a bridge deck (ContainsBridge) */
#define C_OCCUPATION 0x124             /* CellClass::OccupationFlags: 0x80 a building */

static int dir_on_bridge(CellXY c)
{
    BYTE *cell = dir_cell(c);
    return cell && (FIELD(cell, C_FLAGS, DWORD) & 0x100);
}

/* The nearest land off the bridge deck a unit stands on, following its own (perhaps broken) span. */
static int dir_off_bridge(CellXY at, CellXY *out);

/* ---- deployable infantry ----
 * GIs and Guardian GIs fight far better behind sandbags; the AI never deploys them. In a fight
 * (an armed ground enemy within weapon range + 2) they deploy, as a player's deploy click does
 * (FootClass::ClickedAction Self_Deploy, 0x4D75E1: ClickedMission(Unload)), and stay out of the
 * army's orders while deployed. With nothing armed within range + 5 for 150 frames they pack up
 * the same way and rejoin. Deploy and undeploy are at least 300 frames apart. */
#define I_SEQUENCE 0x6C4                /* InfantryClass::SequenceAnim: 27..30 deploying/deployed */
#define MISSION_UNLOAD 16
static const char *dir_deployers = "E1,GGI";
static struct { BYTE *unit; int toggled, last_enemy; } dir_deploy[1024];

static int dir_deployed(BYTE *unit)
{
    int seq = FIELD(unit, I_SEQUENCE, int);
    return seq >= 27 && seq <= 30;
}

static int dir_infantry_deploy(BYTE *unit, CellXY at, int may_deploy)
{
    BYTE *type = dir_type(unit);
    if (dir_whatami(unit) != 15 || !type || !in_list(dir_deployers, (char *)type + T_ID))
        return 0;
    unsigned k = ((DWORD)unit >> 3) % 1024;
    if (dir_deploy[k].unit != unit) {
        dir_deploy[k].unit = unit;
        dir_deploy[k].toggled = dir_deploy[k].last_enemy = -100000;
    }
    int reach = dir_weapon_cells(unit), enemy = 0;
    for (int i = 0; i < dir_enemy_count && !enemy; i++) {
        DirEnemy *e = &dir_enemies[i];
        int r = dir_deployed(unit) ? reach + 5 : reach + 2;
        enemy = e->armed && !e->air && dir_dist2(e->at, at) <= r * r;
    }
    if (enemy)
        dir_deploy[k].last_enemy = CURRENT_FRAME;
    int settled = CURRENT_FRAME - dir_deploy[k].toggled >= 300;
    if (dir_deployed(unit)) {
        /* pack up when the fight is over, or at once when the army retreats */
        if (settled && (!may_deploy || (!enemy && CURRENT_FRAME - dir_deploy[k].last_enemy >= 150))) {
            dir_why = "undeploy";
            static int last_undeploy_log;
            if (CURRENT_FRAME - last_undeploy_log > 600) {
                last_undeploy_log = CURRENT_FRAME;
                logmsg("director: frame %d: %.24s at %d,%d packs up", CURRENT_FRAME, (char *)type + T_ID, at.X, at.Y);
            }
            dir_order(unit, MISSION_UNLOAD, NULL, NULL);
            dir_deploy[k].toggled = CURRENT_FRAME;
        }
        return 1;   /* deployed (or packing up): no army orders meanwhile */
    }
    if (enemy && may_deploy && settled) {
        dir_why = "deploy";
        static int last_log;
        if (CURRENT_FRAME - last_log > 600) {
            last_log = CURRENT_FRAME;
            logmsg("director: frame %d: %.24s at %d,%d deploys for a fight", CURRENT_FRAME, (char *)type + T_ID, at.X, at.Y);
        }
        dir_order(unit, MISSION_UNLOAD, NULL, NULL);
        dir_deploy[k].toggled = CURRENT_FRAME;
        return 1;
    }
    return CURRENT_FRAME - dir_deploy[k].toggled < 60;   /* let the deploy start */
}

/* Kirovs are slow: a unit that can't shoot back gets out from under one instead of sitting there,
 * toward our nearest anti-air if any is close, else straight away from it. Returns 1 while dodging. */
static struct { BYTE *unit; int until; } dir_dodge[512];
static const char *dir_aa_vehicles[3], *dir_aa_infantry[3];

static int dir_dodge_air(BYTE *house, BYTE *unit, CellXY at)
{
    unsigned k = ((DWORD)unit >> 3) % 512;
    if (dir_dodge[k].unit == unit && CURRENT_FRAME < dir_dodge[k].until)
        return 1;
    DirEnemy *air = NULL;
    int best = 7 * 7 + 1;
    for (int i = 0; i < dir_enemy_count; i++) {
        DirEnemy *e = &dir_enemies[i];
        int dd = dir_dist2(e->at, at);
        if (e->air && e->armed && e->value >= 1500 && dd < best) {   /* slow heavies: Kirovs, Discs */
            best = dd;
            air = e;
        }
    }
    if (!air)
        return 0;
    /* units that can shoot it kite: fire while it is in range, step out from under it when it
     * closes in (it bombs straight down and is slower than they are), then fire again */
    if (dir_can_fire_at(unit, air->obj)) {
        if (best > 3 * 3)
            return 0;
        int step = dir_weapon_cells(unit) - 1;
        step = step < 5 ? 5 : step;
        int dx = at.X - air->at.X, dy = at.Y - air->at.Y, len = dir_isqrt(dx * dx + dy * dy);
        if (!len) {
            dx = 1;
            len = 1;
        }
        BYTE *cell = dir_cell((CellXY){ (short)(at.X + dx * step / len), (short)(at.Y + dy * step / len) });
        if (!cell || FIELD(cell, C_LANDTYPE, int) == 2)
            return 0;
        dir_why = "kite air";
        static int last_kite_log;
        if (CURRENT_FRAME - last_kite_log > 600) {
            last_kite_log = CURRENT_FRAME;
            logmsg("director: frame %d: %.24s at %d,%d steps out from under %.24s", CURRENT_FRAME,
                   (char *)dir_type(unit) + T_ID, at.X, at.Y, (char *)dir_type(air->obj) + T_ID);
        }
        dir_order(unit, MISSION_MOVE, NULL, cell);
        dir_dodge[k].unit = unit;
        dir_dodge[k].until = CURRENT_FRAME + 60;
        return 1;
    }
    CellXY to = { 0, 0 };
    int side = FIELD(house, OIL_H_SIDE, int), cover = 20 * 20 + 1;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; side >= 0 && side <= 2 && i < v->Count; i++) {
        BYTE *o = v->Items[i], *type;
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || !(type = dir_type(o))
            || !(in_list(dir_aa_vehicles[side], (char *)type + T_ID) || in_list("NASAM,NAFLAK,YAGGUN", (char *)type + T_ID)))
            continue;
        CellXY c = object_cell(o);
        int dd = dir_dist2(c, at);
        if (dd < cover && dir_dist2(c, air->at) > dd) {   /* cover that is not under the Kirov too */
            cover = dd;
            to = c;
        }
    }
    if (!to.X) {
        int dx = at.X - air->at.X, dy = at.Y - air->at.Y, len = dir_isqrt(dx * dx + dy * dy);
        if (!len) {
            dx = 1;
            len = 1;
        }
        to = (CellXY){ (short)(at.X + dx * 8 / len), (short)(at.Y + dy * 8 / len) };
    }
    BYTE *cell = dir_cell(to);
    if (!cell || FIELD(cell, C_LANDTYPE, int) == 2)
        return 0;
    dir_why = "dodge air";
    dir_order(unit, MISSION_MOVE, NULL, cell);
    dir_dodge[k].unit = unit;
    dir_dodge[k].until = CURRENT_FRAME + 150;
    return 1;
}

/* A unit standing still on a bridge deck (often a broken one, the far end cut off) is walked to the
 * nearest land along its own span, and left alone meanwhile. Returns 1 while it is being moved. */
static struct { BYTE *unit; CellXY at; int since, until; } dir_evac[512];

static int dir_bridge_evac(BYTE *unit, CellXY at)
{
    unsigned k = ((DWORD)unit >> 3) % 512;
    if (dir_evac[k].unit == unit && CURRENT_FRAME < dir_evac[k].until)
        return 1;
    if (!dir_on_bridge(at)) {
        if (dir_evac[k].unit == unit)
            dir_evac[k].unit = NULL;
        return 0;
    }
    if (dir_evac[k].unit != unit || dir_dist2(dir_evac[k].at, at) > 1) {
        dir_evac[k].unit = unit;
        dir_evac[k].at = at;
        dir_evac[k].since = CURRENT_FRAME;
        dir_evac[k].until = 0;
        return 0;
    }
    CellXY off;
    if (CURRENT_FRAME - dir_evac[k].since < 600 || !dir_off_bridge(at, &off))
        return 0;
    dir_why = "off the bridge";
    dir_order(unit, MISSION_MOVE, NULL, dir_cell(off));
    dir_evac[k].since = CURRENT_FRAME;
    dir_evac[k].until = CURRENT_FRAME + 450;
    return 1;
}

static int dir_off_bridge(CellXY at, CellXY *out)
{
    enum { R = 16, W = 2 * R + 1 };
    static unsigned char seen[W * W];
    static CellXY queue[W * W];
    memset(seen, 0, sizeof seen);
    int head = 0, tail = 0;
    queue[tail++] = at;
    seen[R * W + R] = 1;
    while (head < tail) {
        CellXY c = queue[head++];
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                CellXY n = { (short)(c.X + dx), (short)(c.Y + dy) };
                int bx = n.X - at.X + R, by = n.Y - at.Y + R;
                if ((!dx && !dy) || bx < 0 || by < 0 || bx >= W || by >= W || seen[by * W + bx])
                    continue;
                seen[by * W + bx] = 1;
                BYTE *cell = dir_cell(n);
                if (!cell)
                    continue;
                int land = FIELD(cell, C_LANDTYPE, int), deck = (FIELD(cell, C_FLAGS, DWORD) & 0x100) != 0;
                if (deck) {
                    queue[tail++] = n;
                    continue;
                }
                if (land != 2 && land != 3 && land != 4 && !(FIELD(cell, C_OCCUPATION, DWORD) & 0x80)) {
                    *out = n;
                    return 1;
                }
            }
    }
    return 0;
}
#define CELL_GET_BUILDING 0x47C520     /* CellClass::GetBuilding */

/* The region the unit can walk to, within 14 cells: enclosed when the fill never reaches the edge
 * of that box. Buildings on the region's rim are collected as candidates to open it up. */
static int dir_enclosed(CellXY at, BYTE **rim, int *rim_count)
{
    enum { R = 14, W = 2 * R + 1 };
    static unsigned char seen[W * W];
    static CellXY queue[W * W];
    memset(seen, 0, sizeof seen);
    int head = 0, tail = 0;
    *rim_count = 0;
    queue[tail++] = at;
    seen[R * W + R] = 1;
    while (head < tail) {
        CellXY c = queue[head++];
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                CellXY n = { (short)(c.X + dx), (short)(c.Y + dy) };
                int bx = n.X - at.X + R, by = n.Y - at.Y + R;
                if ((!dx && !dy) || bx < 0 || by < 0 || bx >= W || by >= W || seen[by * W + bx])
                    continue;
                seen[by * W + bx] = 1;
                BYTE *cell = dir_cell(n);
                if (!cell)
                    continue;
                int land = FIELD(cell, C_LANDTYPE, int);
                if (FIELD(cell, C_OCCUPATION, DWORD) & 0x80) {
                    BYTE *b = ((BYTE *(GTHISCALL *)(BYTE *))CELL_GET_BUILDING)(cell);
                    int known = 0;
                    for (int k = 0; k < *rim_count; k++)
                        known |= rim[k] == b;
                    if (b && !known && *rim_count < 16)
                        rim[(*rim_count)++] = b;
                    continue;
                }
                if (land == 2 || land == 3 || dir_wall_cell(cell))   /* walls and fences: overlays with Wall=yes */
                    continue;
                if (bx == 0 || by == 0 || bx == W - 1 || by == W - 1)
                    return 0;   /* open ground leads out of the box */
                queue[tail++] = n;
            }
    }
    return 1;
}

static void dir_unstick(BYTE *house, DirState *d, BYTE *unit, CellXY at)
{
    BYTE *rim[16];
    int rim_count;
    d->next_unstick = CURRENT_FRAME + 600;
    if (!dir_enclosed(at, rim, &rim_count))
        return;   /* a jam or a long way round, not a pocket */
    static const char *powers = "GAPOWR,NAPOWR,YAPOWR";
    int plants = combat_building_count(house, powers);
    BYTE *best = NULL;
    int best_cost = 0x7FFFFFFF;
    for (int i = 0; i < rim_count; i++) {
        BYTE *b = rim[i], *type;
        if (!oil_live(b) || FIELD(b, O_OWNER, BYTE *) != house || !(type = dir_type(b)))
            continue;
        const char *id = (char *)type + T_ID;
        int wall = in_list("GAWALL,NAWALL,GAFWLL", id);
        if (!wall && !(in_list(powers, id) && plants >= 3))
            continue;
        int cost = wall ? 0 : dir_cost(type);
        if (cost < best_cost) {
            best_cost = cost;
            best = b;
        }
    }
    logmsg("director: house %d frame %d: %.24s at %d,%d is walled in (%d buildings around), %s%.24s",
           FIELD(house, 0x30, int), CURRENT_FRAME, (char *)dir_type(unit) + T_ID, at.X, at.Y, rim_count,
           best ? "selling " : "nothing to sell", best ? (char *)dir_type(best) + T_ID : "");
    if (!best)
        return;
    d->next_unstick = CURRENT_FRAME + 2400;
    dir_sell(best);
}

static void dir_track_stuck(BYTE *house, DirState *d, BYTE *unit, CellXY at, CellXY goal)
{
    unsigned h = ((DWORD)unit >> 3) % DIR_STUCK, k = h;
    for (int i = 0; i < 8; i++) {
        unsigned j = (h + i) % DIR_STUCK;
        if (dir_moves[j].unit == unit || !dir_moves[j].unit || CURRENT_FRAME - dir_moves[j].frame > 6000) {
            k = j;
            break;
        }
    }
    if (dir_moves[k].unit != unit || dir_dist2(dir_moves[k].at, at) > 2 * 2 || FIELD(unit, O_TARGET, BYTE *)
        || dir_dist2(at, goal) <= 15 * 15) {
        dir_moves[k].unit = unit;   /* moving, fighting or where it should be */
        dir_moves[k].at = at;
        dir_moves[k].frame = CURRENT_FRAME;
        return;
    }
    /* far from home and the rally too: a unit by our own bridgehead is blocked, not stranded, and
     * holding it there would block the crossing */
    if (CURRENT_FRAME - dir_moves[k].frame > 1500 && !dir_crosses(unit) && dir_bridge_down_near(at, 20)
        && dir_dist2(at, d->base) > 25 * 25 && dir_dist2(at, d->rally) > 25 * 25) {
        if (!dir_moves[k].stranded || CURRENT_FRAME - dir_moves[k].stranded >= 600) {
            if (!dir_moves[k].stranded)
                logmsg("director: house %d frame %d: %.24s at %d,%d is cut off by a fallen bridge", FIELD(house, 0x30, int),
                       CURRENT_FRAME, (char *)dir_type(unit) + T_ID, at.X, at.Y);
            dir_moves[k].stranded = CURRENT_FRAME;
        }
        d->stranded_at = at;
        d->stranded_frame = CURRENT_FRAME;
        return;
    }
    dir_moves[k].stranded = 0;
    if (CURRENT_FRAME - dir_moves[k].frame > 3000 && CURRENT_FRAME >= d->next_unstick && !dir_crosses(unit))
        dir_unstick(house, d, unit, at);
}

/* Hostile players still in the game: a duel never stalls the way a free-for-all does. */
static int dir_hostile_houses(BYTE *house)
{
    int count = 0;
    DynVec *v = HOUSE_ARRAY;
    for (int i = 0; i < v->Count; i++)
        count += dir_hostile(house, v->Items[i]) && dir_house_alive(v->Items[i]);
    return count;
}

static int dir_bridge_down_near(CellXY at, int r);

/* The base is falling: a raid at home outvalues the army there. Units out guarding lesser places
 * (oil derricks, stock guard and base-defence teams) more than 15 cells from home leave their teams
 * and come back to fight; afterwards they are team-less and join the army. */
static void dir_recall_guards(BYTE *house, DirState *d)
{
    if (d->state != DIR_DEFEND || d->threat_value < 3000 || d->threat_value * 10 < d->army_value * 12
        || CURRENT_FRAME % 150 >= 15)
        return;
    int n = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i], *team;
        int what;
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || !(team = FIELD(o, F_TEAM, BYTE *))
            || ((what = dir_whatami(o)) != 1 && what != 15) || !dir_armed(o) || dir_naval(o) || dir_is_engineer(o)
            || dir_dist2(object_cell(o), d->base) <= 15 * 15)
            continue;
        ((char (GTHISCALL *)(BYTE *, BYTE *, int, char))TEAM_LIBERATE)(team, o, -1, 0);
        if (FIELD(o, F_TEAM, BYTE *))
            continue;
        o[T_RECRUITABLE] = 0;
        dir_why = "recalled";
        dir_command(o, d->threat_at, NULL, 1);
        n++;
    }
    if (n)
        logmsg("director: house %d frame %d: base falling (raid %d, army %d): %d guards recalled", FIELD(house, 0x30, int),
               CURRENT_FRAME, d->threat_value, d->army_value, n);
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
        int bound = 0;   /* on its way into a tank bunker or a civilian building */
        for (int k = 0; k < 8; k++)
            bound |= d->bunker_unit[k] == o;
        for (int k = 0; k < 24; k++)
            bound |= d->garrison_unit[k] == o && CURRENT_FRAME - d->garrison_frame[k] < 900;
        for (int k = 0; k < 6; k++)
            bound |= d->post_unit[k] == o;
        for (int k = 0; k < 2; k++)
            bound |= d->breach_unit[k] == o && CURRENT_FRAME - d->breach_frame < 600;
        for (int k = 0; k < 3; k++)
            bound |= d->cover_unit[k] == o && CURRENT_FRAME - d->cover_frame < 600;
        if (bound)
            continue;
        if ((d->blocked || d->island) && !dir_crosses(o)) {
            /* on an island map only hover/air units and troops already ferried across attack */
            CellXY c = object_cell(o);
            if (d->threat_value < 1500 && !dir_landed(d, c)) {
                /* ...but everyone defends the base. Those left by the water when the route fell
                 * come back to the rally, where the ferry loads, instead of crowding the shore. */
                if (!dir_bridge_evac(o, c) && dir_dist2(c, d->rally) > 12 * 12) {
                    dir_why = "cut off";
                    dir_command(o, d->rally, NULL, 1);
                }
                continue;
            }
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
        /* the target's army plus third players' units around its base; patience counts from the
         * last attack (or from frame 15000: the opening is no stalemate) */
        int since = d->last_attack_end > 15000 ? d->last_attack_end : 15000;
        int waited = CURRENT_FRAME > since ? CURRENT_FRAME - since : 0;
        /* a duel's first attack waits 9000 frames longer: an army that stays home there is ready for
         * the other's attack, and patience pays. It still ends: two capped armies sat to the time
         * limit. Not again after an attack: both houses reached the lower edge together at frame
         * 36000, traded armies and waited 21000 frames more (9 of 12 Rockets duels timed out). */
        if (dir_hostile_houses(house) < 2 && d->last_attack_end <= 15000)
            waited = waited > 9000 ? waited - 9000 : 0;
        const DirPlanLevers *lv = dir_levers(d);
        int edge = lv->edge - (d->posture == POSTURE_PRESS ? 2 : 0);
        /* the target's army is away from home: what the strike meets is what stayed behind */
        int opposed = d->posture == POSTURE_OPPORTUNITY ? d->target_home : d->target_army;
        int held_home = d->posture == POSTURE_HOLD && d->threat_near * 10 > d->army_value * 7;
        if (d->enemy && !held_home
            && dir_should_launch_plan(d->army_value, n, opposed + d->third_party, d->enemy_def, CURRENT_FRAME, waited,
                                      lv->floor_pct, edge < 8 ? 8 : edge, lv->min_units)) {
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
            /* fighting is progress at the objective or at a front out in the field; skirmishes at
             * home while the attack order silently fails (no land route) are not */
            CellXY c = object_cell(pool[i]);
            fighting |= FIELD(pool[i], O_TARGET, BYTE *) != NULL
                && (u <= 25 * 25 || (dir_dist2(c, d->front) <= 15 * 15 && dir_dist2(d->front, d->base) > 20 * 20));
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
            /* a fallen bridge by the front: the ground route is gone now, not after five stalls */
            if (!d->blocked && dir_bridge_down_near(d->front, 25)) {
                logmsg("director: house %d frame %d: a bridge on the route is down, switching to hover/air and ferries",
                       FIELD(house, 0x30, int), CURRENT_FRAME);
                d->blocked = 1;
                d->blocked_frame = CURRENT_FRAME;
                d->unreachable_count = 0;
                d->state = DIR_RETREAT;
            } else if (d->unreachable_count >= 5 && !d->reached && !d->blocked) {
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
    /* Some raids are met with a barracks flood: infantry trains fast and close to the fight. A
     * draw per raid (one in four), so it isn't the same every time. Its own stream, apart from the
     * strategy layer's, so every director house does it. */
    if (d->state == DIR_DEFEND && previous != DIR_DEFEND) {
        static unsigned inf_roll[32];
        unsigned *x = &inf_roll[FIELD(house, 0x30, int) & 31];
        if (!*x)
            *x = ((unsigned)*GAME_SEED ^ (0x85EBCA6Bu * (unsigned)(FIELD(house, 0x30, int) + 7))) | 1;
        *x ^= *x << 13;
        *x ^= *x >> 17;
        *x ^= *x << 5;
        d->inf_defense = *x % 4 == 0;
        if (d->inf_defense)
            logmsg("director: house %d frame %d: meeting the raid (%d infantry, %d vehicles) with infantry",
                   FIELD(house, 0x30, int), CURRENT_FRAME, d->raid_inf, d->raid_armor);
    } else if (d->state != DIR_DEFEND)
        d->inf_defense = 0;
    dir_recall_guards(house, d);
    if (previous == DIR_ATTACK && d->state != DIR_ATTACK)
        d->last_attack_end = CURRENT_FRAME;
    if (d->state != previous || CURRENT_FRAME - d->last_log > 1500) {
        d->last_log = CURRENT_FRAME;
        logmsg("director: house %d frame %d state %d army %d/%d enemy army %d target %d threat %d at %d,%d local %d",
               FIELD(house, 0x30, int), CURRENT_FRAME, d->state, n, d->army_value, d->enemy_army, d->target_army,
               d->threat_value, d->threat_at.X, d->threat_at.Y, d->local_enemy);
    }

    if (bench_file && CURRENT_FRAME - d->last_log == 0) {   /* benchmark: units crowding the base */
        int crowd = 0, pooled = 0, teamed = 0, missions[32] = { 0 };
        BYTE *crowd_teams[8];
        int crowd_lines[8], crowd_sizes[8], crowd_team_count = 0;
        DynVec *tv = OIL_TECHNO_ARRAY;
        for (int i = 0; i < tv->Count; i++) {
            BYTE *o = tv->Items[i];
            int what;
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || ((what = dir_whatami(o)) != 1 && what != 15)
                || dir_naval(o) || !dir_near_own_building(house, object_cell(o), 3))
                continue;
            crowd++;
            int in_pool = 0;
            for (int k = 0; k < n; k++)
                in_pool |= pool[k] == o;
            pooled += in_pool;
            BYTE *team = FIELD(o, F_TEAM, BYTE *);
            teamed += team != NULL;
            if (team && crowd_team_count < 8) {
                BYTE *tt = FIELD(team, TEAM_TYPE, BYTE *), *script = FIELD(team, TEAM_SCRIPT, BYTE *);
                int k = 0;
                while (k < crowd_team_count && crowd_teams[k] != tt)
                    k++;
                if (k == crowd_team_count) {
                    crowd_teams[crowd_team_count] = tt;
                    crowd_lines[crowd_team_count] = script ? FIELD(script, SCRIPT_LINE, int) : -9;
                    crowd_sizes[crowd_team_count++] = 0;
                }
                crowd_sizes[k]++;
            }
            missions[FIELD(o, COMBAT_MISSION, int) & 31]++;
        }
        logmsg("director:   base crowd %d (pooled %d, in teams %d) guard %d area %d move %d sleep %d harvest %d",
               crowd, pooled, teamed, missions[5], missions[11], missions[2], missions[0], missions[10]);
        for (int k = 0; k < crowd_team_count; k++)
            if (crowd_sizes[k] >= 3)
                logmsg("director:     team %.24s (%s) line %d: %d units", (char *)crowd_teams[k] + T_ID,
                       crowd_teams[k][TT_BASE_DEFENSE] ? "base defense" : "attack", crowd_lines[k], crowd_sizes[k]);
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
    /* The marching body: the group's median unit. A forward section that runs into far more than
     * it can handle falls back to it instead of dying piecemeal. */
    CellXY body = d->front;
    int body_cells = -1, group_value = 0;
    if (d->state == DIR_ATTACK && d->objective && n >= 4 && (dir_flags & DIR_F_REGROUP)) {
        int dist[MAX_POOL], m = 0;
        for (int i = 0; i < n; i++) {
            CellXY c = object_cell(pool[i]);
            if (dir_dist2(c, d->front) <= 30 * 30) {
                dist[m++] = dir_dist2(c, d->objective_at);
                group_value += dir_cost(dir_type(pool[i]));
            }
        }
        if (m >= 4) {
            for (int i = 1; i < m; i++)
                for (int j = i; j > 0 && dist[j - 1] > dist[j]; j--) {
                    int t = dist[j];
                    dist[j] = dist[j - 1];
                    dist[j - 1] = t;
                }
            for (int i = 0; i < n; i++)
                if (dir_dist2(object_cell(pool[i]), d->objective_at) == dist[m / 2]) {
                    body = object_cell(pool[i]);
                    break;
                }
            body_cells = dir_isqrt(dist[m / 2]);
        }
    }
    int regrouping = 0, held = 0, waiting = 0;
    for (int i = 0; i < n; i++) {
        CellXY c = object_cell(pool[i]);
        if (dir_bridge_evac(pool[i], c) || dir_dodge_air(house, pool[i], c)
            || dir_infantry_deploy(pool[i], c, d->state != DIR_RETREAT && !dir_on_bridge(c)))
            continue;
        if (dir_stranded(pool[i])) {
            dir_why = "stranded";
            dir_track_stuck(house, d, pool[i], c, d->state == DIR_ATTACK ? d->objective_at : d->rally);
            dir_command(pool[i], c, NULL, 1);   /* guard the section until the bridge is mended */
            continue;
        }
        /* nobody stops on a bridge deck: holding, regrouping or waiting there blocks the crossing */
        int deck = dir_on_bridge(c);
        if (body_cells >= 0 && !deck) {
            int ahead = body_cells - dir_isqrt(dir_dist2(c, d->objective_at));
            if (ahead >= 8) {
                int ours = 0, theirs = 0;
                for (int k = 0; k < n; k++)
                    if (dir_dist2(object_cell(pool[k]), c) <= 8 * 8)
                        ours += dir_cost(dir_type(pool[k]));
                for (int k = 0; k < dir_enemy_count; k++)
                    if (dir_enemies[k].armed && !dir_enemies[k].air && dir_dist2(dir_enemies[k].at, c) <= 9 * 9)
                        theirs += dir_enemies[k].value;
                if (dir_should_regroup(ours, theirs, ahead, group_value)) {
                    regrouping++;
                    dir_why = "regroup";
                    dir_command(pool[i], body, NULL, 0);
                    continue;
                }
            }
        }
        if (hold > 0 && !deck && !FIELD(pool[i], O_TARGET, BYTE *) && dir_dist2(c, d->objective_at) < hold) {
            held++;
            dir_why = "cohesion hold";
            dir_command(pool[i], c, NULL, engage);   /* area-guard where it stands */
            continue;
        }
        /* In an attack, fresh units far behind wait at the rally for the next wave. Pressing a
         * clear advantage, they reinforce the front instead. */
        if (d->state == DIR_ATTACK && d->launch_frame != CURRENT_FRAME && !deck && d->posture != POSTURE_PRESS
            && dir_dist2(object_cell(pool[i]), d->centroid) > 30 * 30
            && dir_dist2(object_cell(pool[i]), d->rally) <= 12 * 12) {
            waiting++;
            if (dir_flags & DIR_F_UNSTICK)   /* waiting is no proof of being free: test for a pocket */
                dir_track_stuck(house, d, pool[i], c, d->objective_at);
            dir_why = "wait at rally";
            dir_command(pool[i], d->rally, NULL, 1);
            continue;
        }
        /* across the water by ferry: the home army's rally can't be walked to from there, so a
         * landing party that isn't part of an attack fights its way into the nearest structure */
        if ((d->blocked || d->island) && d->state != DIR_ATTACK && !dir_crosses(pool[i]) && dir_landed(d, c)) {
            CellXY at;
            BYTE *hit = dir_nearest_structure(d, c, &at);
            if (hit) {
                dir_why = "landed";
                dir_command(pool[i], at, hit, engage);
                continue;
            }
        }
        if (d->state != DIR_DEFEND && (dir_flags & DIR_F_UNSTICK))
            dir_track_stuck(house, d, pool[i], c, goal);
        dir_why = d->state == DIR_ATTACK ? "attack" : d->state == DIR_DEFEND ? "defend" : d->state == DIR_RETREAT ? "retreat" : "gather";
        dir_command(pool[i], goal, d->state == DIR_ATTACK ? d->objective : NULL, engage);
    }
    if (bench_file && CURRENT_FRAME == d->last_log && d->state == DIR_ATTACK)
        logmsg("director:   attack: %d held (radius %d), %d waiting at the rally, %d regrouping, front %d,%d",
               held, hold > 0 ? dir_isqrt(hold) : -1, waiting, regrouping, d->front.X, d->front.Y);
    if (regrouping && CURRENT_FRAME - d->last_regroup_log > 600) {
        d->last_regroup_log = CURRENT_FRAME;
        logmsg("director: house %d frame %d: %d units fall back to the main body at %d,%d", FIELD(house, 0x30, int),
               CURRENT_FRAME, regrouping, body.X, body.Y);
    }
}

/* ---- economy ---- */
static int dir_idle_harvesters(BYTE *house);
static void dir_outpost(BYTE *house, DirState *d);
static void dir_fill_land(CellXY from);
static void dir_fill_walk(CellXY from);
static int dir_land_reachable(CellXY c);
static int dir_watch_harvester(BYTE *house, BYTE *o);
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
    /* a stock pick the house can't build (a construction yard, say: those come from MCVs) held the
     * building queue for good; free it for the next pick */
    int pick = FIELD(house, OIL_H_PRODUCING, int);
    DynVec *bts = BUILDINGTYPE_ARRAY;
    if (pick >= 0 && pick < bts->Count
        && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, bts->Items[pick], 0, 1) <= 0) {
        FIELD(house, OIL_H_PRODUCING, int) = -1;
        static int last_log[32];
        int idx = FIELD(house, 0x30, int) & 31;
        if (CURRENT_FRAME - last_log[idx] > 3000) {
            last_log[idx] = CURRENT_FRAME;
            logmsg("director: house %d frame %d: dropped %.24s from the build queue (can't be built)", idx, CURRENT_FRAME,
                   (char *)bts->Items[pick] + T_ID);
        }
    }
    /* the expansion MCV needs a service depot (Yuri: grinder); tried every tick like the refinery */
    static const char *depots[3] = { "GADEPT", "NADEPT", "YAGRND" };
    if (d->want_mcv && FIELD(house, OIL_H_PRODUCING, int) == -1 && !combat_building_count(house, depots[side])) {
        BYTE *type = find_type(BUILDINGTYPE_ARRAY, depots[side]);
        if (type && FIELD(house, OIL_H_CASH, int) >= dir_cost(type)
            && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0) {
            FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
            logmsg("director: house %d queued %s for the expansion MCV", FIELD(house, 0x30, int), depots[side]);
        }
    }
    /* Power ahead of need: the combat AI adds war factories only with 50 to spare, and stock
     * plans build power late. Russia on MCV starts sat at one factory with 25-30k unspent, its
     * one reactor at 150 against a drain of 135. */
    static const char *power_plants[3] = { "GAPOWR", "NANRCT,NAPOWR", "YAPOWR" };
    if (FIELD(house, OIL_H_PRODUCING, int) == -1 && CURRENT_FRAME > 1800
        && FIELD(house, OIL_H_POWER, int) - FIELD(house, OIL_H_DRAIN, int) < 100) {
        BYTE *type = dir_first_buildable(house, BUILDINGTYPE_ARRAY, power_plants[side], 1000);
        if (type) {
            FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
            logmsg("director: house %d frame %d: queued %.24s (power %d, drain %d)", FIELD(house, 0x30, int), CURRENT_FRAME,
                   (char *)type + T_ID, FIELD(house, OIL_H_POWER, int), FIELD(house, OIL_H_DRAIN, int));
        }
    }
    if (CURRENT_FRAME >= d->next_economy) {
        d->next_economy = CURRENT_FRAME + 450;
        d->idle_harvesters = dir_idle_harvesters(house);
        dir_outpost(house, d);
    }
    /* checked every tick: the build queue is rarely free, and stock picks fill it at once */
    if (FIELD(house, OIL_H_PRODUCING, int) != -1 || CURRENT_FRAME < d->refinery_backoff
        || !dir_want_refinery_plan(CURRENT_FRAME, FIELD(house, OIL_H_REFINERIES, int), FIELD(house, H_HARVESTERS, int),
                                   d->idle_harvesters, FIELD(house, OIL_H_CASH, int), dir_levers(d)->refinery_bonus,
                                   dir_levers(d)->refinery_early))
        return;
    static const char *refineries[] = { "GAREFN", "NAREFN", "YAREFN" };
    BYTE *type = find_type(BUILDINGTYPE_ARRAY, refineries[side]);
    if (type && FIELD(house, OIL_H_CASH, int) >= dir_cost(type)
        && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0) {
        /* a refinery that never appears could not be placed (cramped or cut-off ground): after
         * three in a row, stop trying for a while instead of tying up the build queue */
        int count = FIELD(house, OIL_H_REFINERIES, int);
        d->refinery_fails = d->refinery_queued && count <= d->refinery_count ? d->refinery_fails + 1 : 0;
        d->refinery_queued = 1;
        d->refinery_count = count;
        if (d->refinery_fails >= 3) {
            d->refinery_fails = d->refinery_queued = 0;
            d->refinery_backoff = CURRENT_FRAME + 6000;
            logmsg("director: house %d: refineries are not being placed, pausing them", FIELD(house, 0x30, int));
            return;
        }
        FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
        logmsg("director: house %d queued %s (refineries %d, harvesters %d, cash %d)", FIELD(house, 0x30, int),
               refineries[side], FIELD(house, OIL_H_REFINERIES, int), FIELD(house, H_HARVESTERS, int),
               FIELD(house, OIL_H_CASH, int));
    }
}

/* ---- ore outposts ----
 * A captured tech building (an oil derrick, say) is our own building, so we may build next to it.
 * Where rich ore lies near one, far from our refineries and clear of enemies, the director builds
 * a refinery beside it on the ore side, then a ground defence and an anti-air defence by the
 * refinery. One outpost at a time; a step that hasn't appeared after 3000 frames ends it. */
static const char *dir_refineries[3] = { "GAREFN", "NAREFN", "YAREFN" };
static int dir_object_listed(DynVec *v, BYTE *obj);
static int dir_ore_near(CellXY c, int r);
static const char *dir_light_defenses[3] = { "GAPILL", "NALASR", "YAGGUN" };
static const char *dir_outpost_defenses[3] = { "NASAM", "NAFLAK", "YAGGUN" };

static BYTE *dir_built_near(BYTE *house, BYTE *type, CellXY at, int r)
{
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *b = v->Items[i];
        if (oil_live(b) && FIELD(b, O_OWNER, BYTE *) == house && FIELD(b, B_TYPE, BYTE *) == type
            && dir_dist2(object_cell(b), at) <= r * r)
            return b;
    }
    return NULL;
}

/* A refinery goes where the ore is: the placeable spot nearest the best field within 30 cells of
 * home (ore value minus 30 per cell of distance; fields a refinery of ours already serves count a
 * third), not wherever the stock base plan had a node. Fields by enemy buildings are skipped. */
static int dir_refinery_place(BYTE *house, DirState *d, BYTE *type, CellXY *out)
{
    if (!in_list("GAREFN,NAREFN,YAREFN", (char *)type + T_ID) || !(director_enabled(house) & DIR_F_ECONOMY))
        return 0;
    CellXY base = d->base, ore = { 0, 0 };
    int best = 0;
    DynVec *bv = OIL_BUILDING_ARRAY;
    for (int dy = -30; dy <= 30; dy += 3)
        for (int dx = -30; dx <= 30; dx += 3) {
            CellXY c = { (short)(base.X + dx), (short)(base.Y + dy) };
            BYTE *cell = dir_cell(c);
            if (!cell || dx * dx + dy * dy > 30 * 30 || ((int (GTHISCALL *)(BYTE *))CELL_ORE_VALUE)(cell) <= 0)
                continue;
            int value = dir_ore_near(c, 4), served = 0;
            for (int i = 0; i < bv->Count && !served; i++) {
                BYTE *b = bv->Items[i];
                served = oil_live(b) && FIELD(b, O_OWNER, BYTE *) == house
                    && in_list("GAREFN,NAREFN,YAREFN", (char *)dir_type(b) + T_ID) && dir_dist2(object_cell(b), c) <= 8 * 8;
            }
            int score = (served ? value / 3 : value) - 30 * dir_isqrt(dx * dx + dy * dy);
            if (score > best && !dir_ore_blocked(house, c)) {
                best = score;
                ore = c;
            }
        }
    if (!best)
        return 0;
    /* on the ore's own level: the nearest spot as the crow flies was at times down a cliff, which
     * harvesters reach only the long way round (and refineries on the beach blocked its ramp) */
    int level = dir_height(ore), closest = 0x7FFFFFFF;
    for (int r = 1; r <= 12; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                CellXY c = { (short)(ore.X + dx), (short)(ore.Y + dy) };
                int dd = dx * dx + dy * dy;
                if ((abs(dx) == r || abs(dy) == r) && dd < closest && dir_cell(c) && abs(dir_height(c) - level) <= 104
                    && ((char (GTHISCALL *)(BYTE *, CellXY *, BYTE *))BTYPE_CAN_PLACE)(type, &c, house)
                    && !dir_blocks_passage(type, c)) {
                    closest = dd;
                    *out = c;
                }
            }
    if (closest == 0x7FFFFFFF)
        return 0;
    logmsg("director: house %d frame %d: %.24s placed at %d,%d by the ore at %d,%d (value %d)", FIELD(house, 0x30, int),
           CURRENT_FRAME, (char *)type + T_ID, out->X, out->Y, ore.X, ore.Y, best);
    return 1;
}

static int dir_outpost_place(BYTE *house, BYTE *type, CellXY *out)
{
    if (!house || !dir_active(house))
        return 0;
    DirState *d = dir_get(house);
    if (d && (!d->outpost_step || type != d->outpost_type))
        return dir_refinery_place(house, d, type, out);
    if (!d)
        return 0;
    BYTE *anchor = d->outpost_step == 1 ? d->outpost_anchor : d->outpost_built;
    if (!anchor || !dir_object_listed(OIL_BUILDING_ARRAY, anchor) || !oil_live(anchor)
        || FIELD(anchor, O_OWNER, BYTE *) != house)
        return 0;
    /* the placeable spot beside the anchor closest to the ore */
    CellXY at = object_cell(anchor);
    int best = 0x7FFFFFFF;
    for (int r = 2; r <= 6; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;
                CellXY c = { (short)(at.X + dx), (short)(at.Y + dy) };
                int dd = dir_dist2(c, d->outpost_ore);
                if (dd < best && ((char (GTHISCALL *)(BYTE *, CellXY *, BYTE *))BTYPE_CAN_PLACE)(type, &c, house)) {
                    best = dd;
                    *out = c;
                }
            }
    if (best == 0x7FFFFFFF)
        return 0;
    logmsg("director: house %d frame %d: outpost %.24s placed at %d,%d", FIELD(house, 0x30, int), CURRENT_FRAME,
           (char *)type + T_ID, out->X, out->Y);
    return 1;
}

static int dir_queue_building(BYTE *house, const char *id, BYTE **queued)
{
    BYTE *type = find_type(BUILDINGTYPE_ARRAY, id);
    if (!type || FIELD(house, OIL_H_PRODUCING, int) != -1 || FIELD(house, OIL_H_CASH, int) < dir_cost(type)
        || ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) <= 0)
        return 0;
    FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
    *queued = type;
    return 1;
}

static void dir_outpost(BYTE *house, DirState *d)
{
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2)
        return;
    if (d->outpost_step) {
        /* step 1: the refinery beside the captured building; 2 and 3: a ground and an anti-air
         * defence beside the new refinery */
        BYTE *anchor = d->outpost_step == 1 ? d->outpost_anchor : d->outpost_built;
        if (!anchor || !dir_object_listed(OIL_BUILDING_ARRAY, anchor) || !oil_live(anchor)
            || FIELD(anchor, O_OWNER, BYTE *) != house || CURRENT_FRAME - d->outpost_frame > 3000) {
            d->outpost_step = 0;   /* lost the anchor, or the step never appeared */
            return;
        }
        BYTE *done = dir_built_near(house, d->outpost_type, object_cell(anchor), 8);
        if (!done) {   /* keep the order in the queue: stock picks may have taken the slot */
            if (FIELD(house, OIL_H_PRODUCING, int) == -1)
                dir_queue_building(house, (char *)d->outpost_type + T_ID, &d->outpost_type);
            return;
        }
        if (d->outpost_step == 1)
            d->outpost_built = done;
        if (d->outpost_step == 3) {
            logmsg("director: house %d frame %d: ore outpost done", FIELD(house, 0x30, int), CURRENT_FRAME);
            d->outpost_step = 0;
            return;
        }
        BYTE *next = find_type(BUILDINGTYPE_ARRAY, d->outpost_step == 1 ? dir_light_defenses[side] : dir_outpost_defenses[side]);
        if (!next) {
            d->outpost_step = 0;
            return;
        }
        d->outpost_type = next;   /* queued from the next tick on, as above */
        d->outpost_step++;
        d->outpost_frame = CURRENT_FRAME;
        return;
    }
    if (CURRENT_FRAME < d->next_outpost || FIELD(house, OIL_H_PRODUCING, int) != -1)
        return;
    d->next_outpost = CURRENT_FRAME + 3000;
    BYTE *refinery = find_type(BUILDINGTYPE_ARRAY, dir_refineries[side]);
    if (!refinery)
        return;
    DynVec *v = OIL_BUILDING_ARRAY;
    BYTE *best = NULL, *island = NULL;
    CellXY best_ore = { 0, 0 };
    int best_score = 2000;
    dir_fill_land(d->rally);   /* holdings across the water get defences even without ore */
    for (int i = 0; i < v->Count; i++) {
        BYTE *b = v->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
        if (!oil_live(b) || FIELD(b, O_OWNER, BYTE *) != house || !type
            || !in_list("CAOILD,CAAIRP,CATHOSP,CAOUTP,CAMACH,CAPOWR", (char *)type + T_ID))
            continue;
        CellXY at = object_cell(b);
        int distant = 1;
        for (int k = 0; k < v->Count && distant; k++) {
            BYTE *r = v->Items[k];
            distant = !(oil_live(r) && FIELD(r, O_OWNER, BYTE *) == house && FIELD(r, B_TYPE, BYTE *)
                    && FIELD(r, B_TYPE, BYTE *)[0x16BB] && dir_dist2(object_cell(r), at) < 15 * 15);   /* Refinery */
        }
        int armed = 0;
        for (int k = 0; k < dir_enemy_count && !armed; k++)
            armed = dir_enemies[k].armed && dir_dist2(dir_enemies[k].at, at) <= 12 * 12;
        if (!distant || armed)
            continue;
        if (!island && !dir_land_reachable(at) && !dir_built_near(house, find_type(BUILDINGTYPE_ARRAY,
                                                                                   dir_light_defenses[side]), at, 8))
            island = b;
        for (int dy = -10; dy <= 10; dy += 2)
            for (int dx = -10; dx <= 10; dx += 2) {
                CellXY c = { (short)(at.X + dx), (short)(at.Y + dy) };
                int score = dir_ore_near(c, 3);
                if (score > best_score) {
                    best_score = score;
                    best = b;
                    best_ore = c;
                }
            }
    }
    if (!best && island) {   /* no ore: straight to the defences */
        BYTE *def = find_type(BUILDINGTYPE_ARRAY, dir_light_defenses[side]);
        if (!def)
            return;
        d->outpost_anchor = d->outpost_built = island;
        d->outpost_type = def;
        d->outpost_ore = object_cell(island);
        d->outpost_step = 2;
        d->outpost_frame = CURRENT_FRAME;
        logmsg("director: house %d frame %d: fortifying the island %.24s", FIELD(house, 0x30, int), CURRENT_FRAME,
               (char *)FIELD(island, B_TYPE, BYTE *) + T_ID);
        return;
    }
    if (!best || !dir_queue_building(house, dir_refineries[side], &d->outpost_type))
        return;
    d->outpost_anchor = best;
    d->outpost_built = NULL;
    d->outpost_ore = best_ore;
    d->outpost_step = 1;
    d->outpost_frame = CURRENT_FRAME;
    CellXY at = object_cell(best);
    logmsg("director: house %d frame %d: ore outpost by the %.24s at %d,%d (ore %d at %d,%d)", FIELD(house, 0x30, int),
           CURRENT_FRAME, (char *)FIELD(best, B_TYPE, BYTE *) + T_ID, at.X, at.Y, best_score, best_ore.X, best_ore.Y);
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
        int stranded = dir_watch_harvester(house, o);
        idle += stranded || (m != 10 && m != 7 && m != 12 && m != 16 && m != 2);   /* harvest, enter, return, unload, move */
    }
    return idle;
}

/* ---- stranded harvesters ----
 * Stock harvesters only look for ore near where they are. Once that is mined out they sit at the
 * refinery on guard, or stay in Harvest with nowhere to go, for the rest of the match. A harvester
 * that has not moved for 900 frames, is not docked and has no destination is sent to the richest ore
 * within 60 cells (by value, minus distance, away from enemy buildings), as a player's click on ore
 * would (FootClass::ClickedAction on a cell, 0x4D7E8E: ClickedMission(Harvest, NULL, cell)). A field
 * it still could not reach is skipped for 9000 frames. */
#define RADIO_CONTACT 0x65AD30            /* RadioClass::GetRadioContact(index) */
#define MISSION_HARVEST 10
static struct { BYTE *unit; CellXY at, sent; int frame, sent_frame; } dir_harv[256];
static struct { BYTE *house; CellXY at; int frame; } dir_bad_ore[32];
static int dir_bad_ore_next;
static int dir_ore_near(CellXY c, int r);

static int dir_ore_blocked(BYTE *house, CellXY c)
{
    for (int i = 0; i < 32; i++)
        if (dir_bad_ore[i].house == house && CURRENT_FRAME - dir_bad_ore[i].frame < 9000
            && dir_dist2(dir_bad_ore[i].at, c) <= 8 * 8)
            return 1;
    DynVec *bv = OIL_BUILDING_ARRAY;
    for (int i = 0; i < bv->Count; i++) {
        BYTE *b = bv->Items[i];
        if (oil_live(b) && dir_hostile(house, FIELD(b, O_OWNER, BYTE *)) && dir_dist2(object_cell(b), c) <= 12 * 12)
            return 1;
    }
    return 0;
}

static int dir_find_ore(BYTE *house, CellXY from, CellXY *out)
{
    int best = 0;
    for (int dy = -60; dy <= 60; dy += 3)
        for (int dx = -60; dx <= 60; dx += 3) {
            CellXY c = { (short)(from.X + dx), (short)(from.Y + dy) };
            BYTE *cell = dir_cell(c);
            if (!cell || dx * dx + dy * dy > 60 * 60 || ((int (GTHISCALL *)(BYTE *))CELL_ORE_VALUE)(cell) <= 0)
                continue;
            int score = dir_ore_near(c, 2) - 40 * dir_isqrt(dx * dx + dy * dy);
            if (score > best && !dir_ore_blocked(house, c)) {
                best = score;
                *out = c;
            }
        }
    return best > 0;
}

/* Returns 1 when the harvester is stranded (counts as idle for refinery and expansion decisions). */
static int dir_watch_harvester(BYTE *house, BYTE *o)
{
    unsigned h = ((DWORD)o >> 3) % 256, k = h;
    for (int i = 0; i < 8; i++) {
        unsigned j = (h + i) % 256;
        if (dir_harv[j].unit == o || !dir_harv[j].unit || CURRENT_FRAME - dir_harv[j].frame > 20000) {
            k = j;
            break;
        }
    }
    CellXY at = object_cell(o);
    int m = FIELD(o, COMBAT_MISSION, int);
    if (dir_harv[k].unit != o || dir_dist2(dir_harv[k].at, at) > 1 || (m != 5 && m != 10 && m != 11 && m != 0)
        || FIELD(o, COMBAT_DESTINATION, BYTE *) || ((BYTE *(GTHISCALL *)(BYTE *, int))RADIO_CONTACT)(o, 0)
        || (dir_cell(at) && ((int (GTHISCALL *)(BYTE *))CELL_ORE_VALUE)(dir_cell(at)) > 0)) {   /* mining */
        /* moving, docked, or busy with a real job: not stranded */
        if (dir_harv[k].unit != o)
            dir_harv[k].sent_frame = 0;
        dir_harv[k].unit = o;
        dir_harv[k].at = at;
        dir_harv[k].frame = CURRENT_FRAME;
        return 0;
    }
    if (CURRENT_FRAME - dir_harv[k].frame < 900)
        return 0;
    if (dir_harv[k].sent_frame && CURRENT_FRAME - dir_harv[k].sent_frame < 1500)
        return 1;   /* give the last order time */
    if (dir_harv[k].sent_frame) {   /* sent there and never left: that field is out of reach... */
        int f = dir_bad_ore_next++ % 32;
        dir_bad_ore[f].house = house;
        dir_bad_ore[f].at = dir_harv[k].sent;
        dir_bad_ore[f].frame = CURRENT_FRAME;
        /* ...or the harvester is walled in by our own buildings */
        DirState *d = dir_get(house);
        if (d && CURRENT_FRAME >= d->next_unstick && (director_enabled(house) & DIR_F_UNSTICK))
            dir_unstick(house, d, o, at);
    }
    CellXY ore;
    if (!(director_enabled(house) & DIR_F_ECONOMY) || !dir_find_ore(house, at, &ore)) {
        dir_harv[k].sent_frame = CURRENT_FRAME;   /* nothing to send it to: ask again later */
        dir_harv[k].sent = (CellXY){ -100, -100 };
        return 1;
    }
    dir_harv[k].sent = ore;
    dir_harv[k].sent_frame = CURRENT_FRAME;
    logmsg("director: house %d frame %d: %.24s at %d,%d stranded (mission %d) for %d frames, sent to ore at %d,%d",
           FIELD(house, 0x30, int), CURRENT_FRAME, (char *)dir_type(o) + T_ID, at.X, at.Y, m,
           CURRENT_FRAME - dir_harv[k].frame, ore.X, ore.Y);
    dir_order(o, MISSION_HARVEST, NULL, dir_cell(ore));
    return 1;
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

static int dir_hut_hopeless(BYTE *hut);

static int dir_bridge_down_near(CellXY at, int r)
{
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *b = v->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
        if (oil_live(b) && type && type[BT_BRIDGE_HUT] && dir_dist2(object_cell(b), at) <= r * r && dir_bridge_down(b)
            && !dir_hut_hopeless(b))
            return 1;
    }
    return 0;
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
struct dir_dead_hut { BYTE *hut; int tries; };
static struct dir_dead_hut dir_dead_huts[32];
static int dir_dead_hut_count;

static int dir_hut_hopeless(BYTE *hut)
{
    for (int k = 0; k < dir_dead_hut_count; k++)
        if (dir_dead_huts[k].hut == hut)
            return dir_dead_huts[k].tries >= 2;
    return 0;
}

/* Bridges that already read down when the match starts were never crossable: map-closed ones,
 * barrier-gated, that no engineer can open. They are no repair job and no sign of a cut route. */
static void dir_note_closed_bridges(void)
{
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count && dir_dead_hut_count < 32; i++) {
        BYTE *b = v->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
        if (oil_live(b) && type && type[BT_BRIDGE_HUT] && dir_bridge_down(b)) {
            dir_dead_huts[dir_dead_hut_count++] = (struct dir_dead_hut){ b, 2 };
            CellXY c = object_cell(b);
            logmsg("director: the bridge at the hut at %d,%d is closed from the start", c.X, c.Y);
        }
    }
}

/* A wall in the cell: walls and fences are overlays whose type has Wall=yes (OverlayTypeClass
 * +0x2A8, as its LoadFromINI stores it; the types are the DynVec at 0xA83D80; the cell's overlay index
 * is at +0x44, as CellClass::GetContainedTiberiumValue reads it). They block movement by that flag,
 * not by the land type, which stays clear. */
#define OVERLAYTYPE_ARRAY ((DynVec *)0xA83D80)
#define C_OVERLAY 0x44
#define OT_WALL 0x2A8
static int dir_wall_cell(BYTE *cell)
{
    int ov = FIELD(cell, C_OVERLAY, int);
    DynVec *v = OVERLAYTYPE_ARRAY;
    return FIELD(cell, C_LANDTYPE, int) == 4 || (ov >= 0 && ov < v->Count && ((BYTE *)v->Items[ov])[OT_WALL]);
}

/* A building fenced in, as map derricks often are: a flood fill from it over open ground (its own
 * cells, no walls, water, rock or other buildings) that can't get 7 cells away. Returns 0 when the
 * building can be walked to; 1 when a wall (Wall=yes overlay: CAFNCB, CAFNCW, CAFNCP) encloses it,
 * with the wall cell nearest `from` in *gap; 2 when only rock, water or buildings do (the FENCE01-22
 * overlays are Land=Rock and can't be destroyed). */
static int dir_fenced(BYTE *b, CellXY from, CellXY *gap)
{
    enum { R = 9, W = 2 * R + 1 };
    static unsigned char seen[W * W];
    static CellXY queue[W * W];
    memset(seen, 0, sizeof seen);
    CellXY at = object_cell(b);
    int head = 0, tail = 0, best = 0x7FFFFFFF;
    queue[tail++] = at;
    seen[R * W + R] = 1;
    while (head < tail) {
        CellXY c = queue[head++];
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                CellXY n = { (short)(c.X + dx), (short)(c.Y + dy) };
                int bx = n.X - at.X + R, by = n.Y - at.Y + R;
                if ((!dx && !dy) || bx < 0 || by < 0 || bx >= W || by >= W || seen[by * W + bx])
                    continue;
                seen[by * W + bx] = 1;
                BYTE *cell = dir_cell(n);
                if (!cell)
                    continue;
                int land = FIELD(cell, C_LANDTYPE, int);
                if (dir_wall_cell(cell)) {   /* the fence: the panel nearest our engineer is the one to open */
                    int dd = dir_dist2(n, from);
                    if (dd < best) {
                        best = dd;
                        *gap = n;
                    }
                    continue;
                }
                if (land == 2 || land == 3)
                    continue;
                if ((FIELD(cell, C_OCCUPATION, DWORD) & 0x80) && ((BYTE *(GTHISCALL *)(BYTE *))CELL_GET_BUILDING)(cell) != b)
                    continue;
                if (abs(n.X - at.X) > 7 || abs(n.Y - at.Y) > 7)
                    return 0;   /* open ground leads away from it */
                queue[tail++] = n;
            }
    }
    return best != 0x7FFFFFFF ? 1 : 2;
}

/* The capture target is fenced in: up to two armed ground units of ours nearest the fence (within
 * 30 cells) shoot the panel nearest the engineer, as a player's force-fire on a wall cell does. */
/* Only a warhead with Wall=yes (WarheadTypeClass +0x144, read at 0x75D4FB) hurts walls and fences;
 * a weapon's warhead is WeaponTypeClass +0xAC (0x772992). Flak tracks and the like sent to open a
 * fence fired at it for good. */
#define WT_WARHEAD 0xAC
#define WH_WALL 0x144
static int dir_hurts_walls(BYTE *obj)
{
    for (int k = 0; k < 2; k++) {
        BYTE **w = ((BYTE **(GTHISCALL *)(BYTE *, int))VFUNC(obj, VT_GETWEAPON))(obj, k);
        BYTE *wh = w && *w ? FIELD(*w, WT_WARHEAD, BYTE *) : NULL;
        if (k && dir_whatami(obj) == 15)
            break;   /* a soldier's second weapon is for deployed use (a Guardian GI's missiles): it shot at fences with its rifle */
        if (wh && wh[WH_WALL])
            return 1;
    }
    return 0;
}

/* Units whose MovementZone (TechnoTypeClass +0x5B4, read at 0x716065) paths through a fence: the
 * Crusher zones over a Crushable one (ObjectTypeClass +0x22D, 0x5F940A: the CAFNC* fences and
 * sandbags), the Destroyer zones through any they can shoot. Sent past it, they open it on the way.
 * A Normal-zone tank (Grizzly) is refused a move into a fenced box, Crusher=yes or not. */
#define TT_MOVEMENT_ZONE 0x5B4
#define OT_CRUSHABLE 0x22D
static int dir_crushable_wall(BYTE *cell)
{
    int ov = cell ? FIELD(cell, C_OVERLAY, int) : -1;
    DynVec *v = OVERLAYTYPE_ARRAY;
    return ov >= 0 && ov < v->Count && ((BYTE *)v->Items[ov])[OT_WALL] && ((BYTE *)v->Items[ov])[OT_CRUSHABLE];
}

static int dir_paths_through(BYTE *obj, int crushable)
{
    int zone = FIELD(dir_type(obj), TT_MOVEMENT_ZONE, int);
    if (zone == 1 || zone == 4 || zone == 12)   /* Crusher, AmphibiousCrusher, CrusherAll */
        return crushable;
    return (zone == 2 || zone == 3 || zone == 8) && dir_hurts_walls(obj);   /* the Destroyer zones */
}

static void dir_breach_fence(BYTE *house, DirState *d)
{
    BYTE *eng = d->repair_engineer, *b = d->repair_hut;
    CellXY gap;
    if (!d->repair_mode || !eng || !b || !dir_object_listed(OIL_TECHNO_ARRAY, eng) || !oil_live(eng))
        return;
    int fenced = dir_fenced(b, object_cell(eng), &gap);
    if (fenced == 2) {   /* the box is closed by cliffs: a ramp further out, or no way at all */
        dir_fill_land(object_cell(eng));   /* over land, bridge decks and walls */
        if (dir_land_reachable(object_cell(b)))
            return;
    }
    if (fenced == 2) {   /* sealed for good: give it up now rather than after the job's 4000 frames */
        logmsg("director: house %d frame %d: %.24s is sealed in by rock or water, leaving it", FIELD(house, 0x30, int),
               CURRENT_FRAME, (char *)dir_type(b) + T_ID);
        d->failed_job = b;
        d->failed_frame = CURRENT_FRAME;
        dir_why = "engineer home";
        dir_command(eng, d->base, NULL, 0);
        d->repair_hut = d->repair_engineer = NULL;
        return;
    }
    if (!fenced) {
        if (d->breach_unit[0] && CURRENT_FRAME - d->breach_frame < 600) {
            logmsg("director: house %d frame %d: the fence round %.24s is open", FIELD(house, 0x30, int), CURRENT_FRAME,
                   (char *)dir_type(b) + T_ID);
            d->breach_unit[0] = d->breach_unit[1] = NULL;
        }
        return;
    }
    BYTE *cell = dir_cell(gap);
    int crush = dir_crushable_wall(cell);
    BYTE *pick[2] = { NULL, NULL };
    int dist[2] = { 30 * 30, 30 * 30 };
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i];
        int what, dd;
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || ((what = dir_whatami(o)) != 1 && what != 15)
            || !dir_armed(o) || dir_naval(o) || dir_is_engineer(o) || (what == 1 && dir_type(o)[UT_HARVESTER])
            || (dd = dir_dist2(object_cell(o), gap)) >= dist[1]
            || !(dir_hurts_walls(o) || dir_paths_through(o, crush)) || !dir_team_takeable(o))
            continue;
        if (dd < dist[0]) {
            pick[1] = pick[0], dist[1] = dist[0];
            pick[0] = o, dist[0] = dd;
        } else
            pick[1] = o, dist[1] = dd;
    }
    d->breach_unit[0] = pick[0];
    d->breach_unit[1] = pick[1];
    d->breach_frame = CURRENT_FRAME;
    for (int k = 0; k < 2; k++)
        if (pick[k] && !dir_recent_order(pick[k], cell, 300) && dir_take_from_team(pick[k])) {   /* or its team orders it back */
            dir_why = "breach fence";
            if (dir_paths_through(pick[k], crush)) {   /* past it, toward the building */
                CellXY at = object_cell(b), in = gap;
                in.X += at.X > gap.X ? 1 : at.X < gap.X ? -1 : 0;
                in.Y += at.Y > gap.Y ? 1 : at.Y < gap.Y ? -1 : 0;
                dir_order(pick[k], MISSION_MOVE, NULL, dir_cell(in));
            }
            else
                dir_order(pick[k], MISSION_ATTACK, cell, NULL);
        }
    static int last_log[32];
    int idx = FIELD(house, 0x30, int) & 31;
    if (pick[0] && CURRENT_FRAME - last_log[idx] > 1500) {
        last_log[idx] = CURRENT_FRAME;
        int run_over = dir_paths_through(pick[0], crush);
        logmsg("director: house %d frame %d: %.24s is fenced in, %.12s %s the fence at %d,%d", idx, CURRENT_FRAME,
               (char *)dir_type(b) + T_ID, (char *)dir_type(pick[0]) + T_ID, run_over ? "drives through" : "shoots", gap.X, gap.Y);
    }
}

static void dir_engineers(BYTE *house, DirState *d)
{
    dir_why = "engineer";
    BYTE *job = d->repair_hut;
    int alive = job && dir_object_listed(OIL_BUILDING_ARRAY, job) && oil_live(job);
    BYTE *job_owner = alive ? FIELD(job, O_OWNER, BYTE *) : NULL;
    int done = !alive || (d->repair_mode == 0 ? !dir_bridge_down(job) : job_owner == house
                          || (!dir_hostile(house, job_owner) && !dir_passive(job_owner)));
    /* An engineer gone (inside the hut, or dead) while its bridge still reads down: some map bridges
     * (the barrier-gated ones) read "destroyed" for good and can't be mended. Two such tries and the
     * hut is left alone for the rest of the match. */
    if (d->repair_engineer && dir_object_listed(OIL_TECHNO_ARRAY, d->repair_engineer) && oil_live(d->repair_engineer))
        d->repair_eng_at = object_cell(d->repair_engineer);
    /* gone on the way (killed): no evidence against the bridge, just send another */
    if (job && alive && !done && d->repair_mode == 0 && d->repair_engineer
        && !(dir_object_listed(OIL_TECHNO_ARRAY, d->repair_engineer) && oil_live(d->repair_engineer))
        && dir_dist2(d->repair_eng_at, object_cell(job)) > 3 * 3) {
        logmsg("director: house %d frame %d: the engineer for the hut at %d,%d was lost on the way at %d,%d",
               FIELD(house, 0x30, int), CURRENT_FRAME, object_cell(job).X, object_cell(job).Y, d->repair_eng_at.X,
               d->repair_eng_at.Y);
        d->repair_engineer = NULL;
        d->repair_hut = NULL;
        job = NULL;
    }
    if (job && alive && !done && d->repair_mode == 0 && d->repair_engineer
        && !(dir_object_listed(OIL_TECHNO_ARRAY, d->repair_engineer) && oil_live(d->repair_engineer))) {
        int k = 0;
        while (k < dir_dead_hut_count && dir_dead_huts[k].hut != job)
            k++;
        if (k == dir_dead_hut_count && k < 32)
            dir_dead_huts[dir_dead_hut_count++] = (struct dir_dead_hut){ job, 0 };
        if (k < 32 && ++dir_dead_huts[k].tries == 2) {
            CellXY c = object_cell(job);
            logmsg("director: house %d: the bridge at the hut at %d,%d can't be mended, leaving it", FIELD(house, 0x30, int),
                   c.X, c.Y);
        }
        d->repair_hut = d->repair_engineer = NULL;
        job = NULL;
    }
    if (job && (done || CURRENT_FRAME - d->repair_frame > 4000)) {
        if (!done) {   /* out of reach (e.g. the hut is across the water): leave it for a while */
            d->failed_job = job;
            d->failed_frame = CURRENT_FRAME;
        }
        if (alive && done)
            logmsg("director: house %d frame %d: engineer job done (%s)", FIELD(house, 0x30, int), CURRENT_FRAME,
                   d->repair_mode ? "captured" : "bridge repaired");
        /* an engineer still walking to a hut whose bridge is already whole (mended by someone else,
         * or the job timed out) is called home instead of entering it for nothing */
        BYTE *eng = d->repair_engineer;
        if (eng && dir_object_listed(OIL_TECHNO_ARRAY, eng) && oil_live(eng)
            && FIELD(eng, COMBAT_MISSION, int) != MISSION_CAPTURE) {
            dir_why = "engineer home";
            dir_command(eng, d->base, NULL, 0);
        }
        d->repair_hut = d->repair_engineer = NULL;
    }
    d->want_engineer = 0;
    if (d->repair_hut && d->repair_engineer && dir_object_listed(OIL_TECHNO_ARRAY, d->repair_engineer)
        && oil_live(d->repair_engineer)) {
        dir_breach_fence(house, d);
        return;
    }
    CellXY base = d->base;
    (void)base;
    BYTE *target = NULL;
    int best = 45 * 45, mode = 0;
    DynVec *v = OIL_BUILDING_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *b = v->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
        if (!oil_live(b) || !type || !type[BT_BRIDGE_HUT] || !dir_bridge_down(b) || dir_hut_hopeless(b))
            continue;
        CellXY c = object_cell(b);
        int dd = dir_dist2(c, d->base), df = dir_dist2(c, d->front);
        dd = df < dd ? df : dd;
        if (d->stranded_frame && CURRENT_FRAME - d->stranded_frame < 1500 && dir_dist2(c, d->stranded_at) <= 25 * 25)
            dd = 0;   /* our units are cut off behind this one: mend it first, wherever it is */
        if (dd >= best || (b == d->failed_job && CURRENT_FRAME - d->failed_frame < 9000))
            continue;
        int guarded = 0;   /* an engineer walking into enemy guns is only lost: the army clears it first */
        for (int k = 0; k < dir_enemy_count && !guarded; k++)
            guarded = dir_enemies[k].armed && !dir_enemies[k].building && dir_dist2(dir_enemies[k].at, c) <= 7 * 7;
        if (guarded)
            continue;
        best = dd;
        target = b;
    }
    if (!target) {
        /* derricks within 40 cells of the base, the rally (where the army waits) or the front;
         * other tech buildings closer in. At 18 cells a house whose derricks lay further out never
         * sent an engineer all game, sitting out a stalemate with its ore gone. */
        best = 40 * 40;
        for (int i = 0; i < dir_enemy_count; i++) {
            DirEnemy *e = &dir_enemies[i];
            if (!e->capturable)
                continue;
            int dd = dir_dist2(e->at, d->base), df = dir_dist2(e->at, d->front), dr = dir_dist2(e->at, d->rally);
            dd = df < dd ? df : dd;
            dd = dr < dd ? dr : dd;
            int oil = !_stricmp((char *)dir_type(e->obj) + T_ID, "CAOILD");
            if (!oil)
                dd += 22 * 22;
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
    d->repair_eng_at = object_cell(engineer);
    logmsg("director: house %d frame %d: sends an engineer to %s %.24s at %d,%d", FIELD(house, 0x30, int), CURRENT_FRAME,
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
    int level = dir_height(base);
    /* Out of the base: units idling on building sites block construction. On the base's own level:
     * a rally on the beach below an island base put the whole waiting army where the transports
     * dock, and in the one ramp down to it. */
    /* toward the enemy first, then turned 45 and 90 degrees either way (scaled back to length) */
    static const int turn[5][4] = { {1,0,0,1}, {1,-1,1,1}, {1,1,-1,1}, {0,-1,1,0}, {0,1,-1,0} };
    for (int t = 0; t < 5; t++)
    for (int step = 12; step <= 24 && step < len; step += 4) {
        int vx = turn[t][0] * dx + turn[t][1] * dy, vy = turn[t][2] * dx + turn[t][3] * dy;
        int vlen = (t == 1 || t == 2) ? len * 1414 / 1000 : len;
        CellXY want = { (short)(d->base.X + vx * step / vlen), (short)(d->base.Y + vy * step / vlen) }, out = { 0, 0 };
        if (fallback.X == base.X && fallback.Y == base.Y && abs(dir_height(want) - level) <= 104)
            fallback = want;
        /* SpeedType Track, MovementZone Normal, 5x5 clear, no overlay, no bridge */
        ((nearby_fn)MAP_NEARBY)(MAP_INSTANCE, &out, &want, 1, zone, 0, 0, 5, 5, 1, 0, 0, 0, &want, 0, 0);
        if (out.X <= 0 || out.Y <= 0 || dir_dist2(out, want) > 10 * 10)
            continue;
        CellXY c = { (short)(out.X + 2), (short)(out.Y + 2) };   /* the patch's centre */
        if (dir_dist2(c, d->base) >= 10 * 10 && !dir_near_own_building(NULL, c, 8) && abs(dir_height(c) - level) <= 104)
            return c;
    }
    return fallback;
}

/* ---- expansion ----
 * When harvesters stand idle (ore gone or cut off), look for reachable ore away from our refineries
 * and enemy structures, build an MCV and deploy it there. An AI construction yard re-centres the
 * base plan on itself (UnitClass deploy, 0x7398CE-0x739926), so new refineries follow the ore. */

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
    /* within 45 cells of home; if no field there will do, out to 70 (a long standoff mines out
     * the near fields, and all the rest lay beyond 45) */
    for (int reach = 45; reach <= 70 && !best; reach += 25) {
    max_ore = with_ore = rejected = 0;
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
            int dist = dir_dist2(c, d->base), ok = dist >= 16 * 16 && dist <= reach * reach;
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
        logmsg("director: house %d site search (%d cells): max ore %d, %d ore spots, %d rejected, best score %d",
               FIELD(house, 0x30, int), reach, max_ore, with_ore, rejected, best);
    }
    if (!best)
        return 0;
    /* a clear 4x4 spot for the construction yard beside the ore */
    CellXY out = { 0, 0 }, want = *site;
    /* beside the field: buildings cannot stand on ore, so no overlay under the yard */
    ((nearby_fn)MAP_NEARBY)(MAP_INSTANCE, &out, &want, 1, -1, 0, 0, 4, 4, 1, 0, 0, 0, &want, 0, 0);
    if (out.X <= 0 || dir_dist2(out, want) > 12 * 12)
        return 0;
    *site = (CellXY){ (short)(out.X + 1), (short)(out.Y + 1) };
    return 1;
}

/* ---- construction yards flee ----
 * With MCV repacks on, a construction yard about to fall packs up and its MCV drives off to set up
 * again elsewhere. Packing up is the selling mission (BuildingClass::Mission_Selling undeploys a
 * yard with UndeploysInto when MCVRedeploy is on, its Focus is set and 0x50B730 says the owner may);
 * 0x50B730 answers "human" in a multiplayer session, so a computer player's yard would be sold
 * instead. dir_may_undeploy, patched in at that call (0x449D29), also says yes for a director house
 * whose yard was just told to flee. Mission_Selling asks at five places (patched at each). */
#define SESSION_MCV_REDEPLOY (*(BYTE *)0xA8B320)
static int dir_escape_house[32];   /* frame a yard of this house was told to flee */

static char GFASTCALL dir_may_undeploy(BYTE *house, void *unused)
{
    (void)unused;
    if (((char (GTHISCALL *)(BYTE *))0x50B730)(house))
        return 1;
    int idx = FIELD(house, 0x30, int);
    return idx >= 0 && idx < 32 && dir_escape_house[idx] && CURRENT_FRAME - dir_escape_house[idx] < 900;
}

/* The stock AI deploys a yardless MCV where it stands, at once: the fled yard was back on its old
 * spot within 80 frames. UnitClass::TryToDeploy (0x7393C0) is hooked at its entry (its first five bytes,
 * sub esp,18h / push ebx / push ebp, run in a trampoline): a fleeing house's MCV deploys only at its site. */
#define UNIT_TRY_DEPLOY 0x7393C0
static char (GTHISCALL *dir_try_deploy_original)(BYTE *);
static char GFASTCALL dir_try_deploy(BYTE *unit, void *unused)
{
    (void)unused;
    BYTE *house = FIELD(unit, O_OWNER, BYTE *);
    int idx = house ? FIELD(house, 0x30, int) : -1;
    if (idx >= 0 && idx < 32 && dir_state[idx].house == house && dir_state[idx].escape_frame
        && CURRENT_FRAME - dir_state[idx].escape_frame < 6000 && in_list(dir_mcvs, (char *)dir_type(unit) + T_ID)
        && dir_dist2(object_cell(unit), dir_state[idx].site) > 3 * 3) {
        if (bench_file && CURRENT_FRAME % 60 < 15)
            logmsg("director: house %d frame %d: fleeing MCV kept from deploying at %d,%d", idx, CURRENT_FRAME,
                   object_cell(unit).X, object_cell(unit).Y);
        return 0;
    }
    return dir_try_deploy_original(unit);
}

/* UnitClass::AI puts a computer player's MCV on Hunt when its house has no construction yard (0x73645B),
 * and Hunt deploys an MCV where it stands: the fled yard was back on its spot 16 frames later. The rule
 * skips human players through 0x50B730 (call at 0x736424); while a house's MCV flees, that answers yes
 * too, until it is set up again at its site. */
static char GFASTCALL dir_mcv_left_alone(BYTE *house, void *unused)
{
    (void)unused;
    if (((char (GTHISCALL *)(BYTE *))0x50B730)(house))
        return 1;
    int idx = FIELD(house, 0x30, int);
    return idx >= 0 && idx < 32 && dir_state[idx].house == house && dir_state[idx].escape_frame
        && CURRENT_FRAME - dir_state[idx].escape_frame < 6000;
}

/* Ground units can get from beside the building to 12+ cells away, around buildings: a yard packed
 * in among its own base turns into an MCV that can't get out. */
static int dir_way_out(BYTE *b, int r)
{
    enum { R = 20, W = 2 * R + 1 };
    static unsigned char seen[W * W];
    static CellXY queue[W * W];
    memset(seen, 0, sizeof seen);
    CellXY at = object_cell(b);
    int head = 0, tail = 0;
    queue[tail++] = at;
    seen[R * W + R] = 1;
    while (head < tail) {
        CellXY c = queue[head++];
        if (abs(c.X - at.X) >= r || abs(c.Y - at.Y) >= r)
            return 1;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                CellXY n = { (short)(c.X + dx), (short)(c.Y + dy) };
                int bx = n.X - at.X + R, by = n.Y - at.Y + R;
                if ((!dx && !dy) || bx < 0 || by < 0 || bx >= W || by >= W || seen[by * W + bx])
                    continue;
                seen[by * W + bx] = 1;
                BYTE *cell = dir_cell(n);
                if (!cell || FIELD(cell, C_LANDTYPE, int) == 2 || FIELD(cell, C_LANDTYPE, int) == 3 || dir_wall_cell(cell))
                    continue;
                if ((FIELD(cell, C_OCCUPATION, DWORD) & 0x80) && ((BYTE *(GTHISCALL *)(BYTE *))CELL_GET_BUILDING)(cell) != b)
                    continue;
                queue[tail++] = n;
            }
    }
    return 0;
}

static void dir_yard_escape(BYTE *house, DirState *d)
{

    if (!SESSION_MCV_REDEPLOY || CURRENT_FRAME < d->next_escape || (d->escape_frame && CURRENT_FRAME - d->escape_frame < 3000))
        return;
    d->next_escape = CURRENT_FRAME + 60;
    DynVec *bv = OIL_BUILDING_ARRAY, *tv = OIL_TECHNO_ARRAY;
    for (int i = 0; i < bv->Count; i++) {
        BYTE *b = bv->Items[i], *type;
        if (!oil_live(b) || FIELD(b, O_OWNER, BYTE *) != house || !(type = dir_type(b))
            || !in_list("GACNST,NACNST,YACNST", (char *)type + T_ID)
            || (FIELD(b, O_HEALTH, int) * 10 >= FIELD(type, OT_STRENGTH, int) * 7 && !bench_test_escape))
            continue;
        CellXY at = object_cell(b);
        int theirs = 0, ours = 0;
        for (int k = 0; k < dir_enemy_count; k++)
            if (dir_enemies[k].armed && !dir_enemies[k].building && dir_dist2(dir_enemies[k].at, at) <= 10 * 10)
                theirs += dir_enemies[k].value;
        for (int k = 0; k < tv->Count; k++) {
            BYTE *o = tv->Items[k];
            if (oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_whatami(o) != 6 && dir_armed(o)
                && dir_dist2(object_cell(o), at) <= 10 * 10)
                ours += dir_cost(dir_type(o));
        }
        /* under 40%, outgunned; under 70%, hopelessly (packing up takes a while, and a yard at 40% under
         * a big attack died in the middle of it) */
        int low = FIELD(b, O_HEALTH, int) * 10 < FIELD(type, OT_STRENGTH, int) * 4;
        int test = bench_test_escape && CURRENT_FRAME >= 600 && FIELD(house, 0x30, int) == 1;   /* bench test */
        if (!test && (theirs < 2000 || theirs <= ours || (!low && theirs < ours * 3 + 3000)))
            continue;
        bench_test_escape = 0;
        /* where to: 20-35 cells off, on land reachable from here (and in the engine's movement zone of
         * the ground beside the yard), as far from armed enemies as can be */
        dir_fill_land(at);
        int zone = -1;
        for (int k = 0; k < 16 && zone < 0; k++) {
            CellXY c = { (short)(at.X - 2 + k % 6), (short)(at.Y + 3) };
            BYTE *cell = dir_cell(c);
            if (cell && !(FIELD(cell, C_OCCUPATION, DWORD) & 0x80))
                zone = dir_zone(c);
        }
        CellXY best = { 0, 0 };
        int best_d = -1;
        for (int a = 0; a < 16; a++)
            for (int r = 20; r <= 35; r += 5) {
                static const signed char dx16[16] = { 10, 9, 7, 4, 0, -4, -7, -9, -10, -9, -7, -4, 0, 4, 7, 9 };
                static const signed char dy16[16] = { 0, 4, 7, 9, 10, 9, 7, 4, 0, -4, -7, -9, -10, -9, -7, -4 };
                CellXY c = { (short)(at.X + dx16[a] * r / 10), (short)(at.Y + dy16[a] * r / 10) };
                BYTE *cell = dir_cell(c);
                if (!cell || !dir_is_land(c) || (FIELD(cell, C_OCCUPATION, DWORD) & 0x80) || dir_zone(c) != zone)
                    continue;
                int nearest = 0x7FFFFFFF;
                for (int k = 0; k < dir_enemy_count; k++)
                    if (dir_enemies[k].armed) {
                        int dd = dir_dist2(dir_enemies[k].at, c);
                        nearest = dd < nearest ? dd : nearest;
                    }
                if (nearest > best_d) {
                    best_d = nearest;
                    best = c;
                }
            }
        if (!best.X || best_d < 15 * 15 || !dir_way_out(b, 12))
            continue;   /* nowhere safe to go, or boxed in by its own base: it stays and fights */
        logmsg("director: house %d frame %d: %.24s at %d,%d is falling (%d vs %d), packing up for %d,%d",
               FIELD(house, 0x30, int), CURRENT_FRAME, (char *)type + T_ID, at.X, at.Y, theirs, ours, best.X, best.Y);
        dir_announce(house, "construction yard packing up to escape");
        FIELD(b, T_FOCUS, BYTE *) = dir_cell(best);
        dir_escape_house[FIELD(house, 0x30, int) & 31] = CURRENT_FRAME;
        dir_sell(b);
        d->escape_frame = CURRENT_FRAME;
        d->site = best;
        d->deploy_tries = 0;
        return;
    }
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
    int escaping = d->escape_frame && CURRENT_FRAME - d->escape_frame < 6000;
    if (escaping && yards && !mcv && CURRENT_FRAME - d->escape_frame > 300) {
        logmsg("director: house %d frame %d: construction yard set up again near %d,%d", FIELD(house, 0x30, int),
               CURRENT_FRAME, d->site.X, d->site.Y);
        d->escape_frame = 0;
        d->site = (CellXY){ 0, 0 };
        escaping = 0;
    }
    if (mcv && (yards || escaping) && d->site.X > 0) {
        d->want_mcv = 0;
        CellXY at = object_cell(mcv);
        BYTE *cell = dir_cell(d->site);
        if (dir_dist2(at, d->site) <= 2 * 2) {
            /* TryToDeploy refuses while the unit has a destination or is still rolling */
            if (FIELD(mcv, COMBAT_DESTINATION, BYTE *)) {
                ((void (GTHISCALL *)(BYTE *, BYTE *, char))VFUNC(mcv, COMBAT_SET_DESTINATION))(mcv, NULL, 1);
                ((char (GTHISCALL *)(BYTE *))VFUNC(mcv, COMBAT_STOP_MOVING))(mcv);
                return;
            }
            if (!dir_recent_order(mcv, (BYTE *)1, 120)) {
                if (++d->deploy_tries > 2) {
                    /* the spot is taken (units, a new building): the nearest clear 4x4 around it */
                    CellXY out = { 0, 0 };
                    ((nearby_fn)MAP_NEARBY)(MAP_INSTANCE, &out, &at, 1, -1, 0, 0, 4, 4, 1, 0, 0, 0, &at, 0, 0);
                    if (out.X > 0 && dir_dist2(out, at) <= 12 * 12)
                        d->site = (CellXY){ (short)(out.X + 1), (short)(out.Y + 1) };
                    d->deploy_tries = 0;
                }
                /* the player's deploy order: the Unload mission retries UnitClass::TryToDeploy each frame */
                ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(mcv, VT_QUEUEMISSION))(mcv, 16, 1);
                logmsg("director: house %d deploying expansion MCV at %d,%d (mission %d)", FIELD(house, 0x30, int),
                       at.X, at.Y, FIELD(mcv, COMBAT_MISSION, int));
            }
        } else if (cell && (!dir_recent_order(mcv, cell, 450) || (escaping && FIELD(mcv, COMBAT_MISSION, int) != MISSION_MOVE))) {
            /* fleeing, the stock AI tells a yardless MCV to deploy where it stands: on the move again */
            dir_order(mcv, MISSION_MOVE, NULL, cell);
            if (escaping && bench_file && CURRENT_FRAME % 300 < 15)
                logmsg("director: house %d frame %d: MCV at %d,%d heading for %d,%d", FIELD(house, 0x30, int), CURRENT_FRAME,
                       at.X, at.Y, d->site.X, d->site.Y);
        }
        return;
    }
    if (CURRENT_FRAME < d->next_site)
        return;
    d->next_site = CURRENT_FRAME + 900;
    int wanted = d->want_mcv;
    d->want_mcv = 0;
    const DirPlanLevers *lv = dir_levers(d);
    if (!(director_enabled(house) & DIR_F_EXPANSION) || !yards || yards >= 2 || mcv || CURRENT_FRAME < lv->expand_frame
        || (d->idle_harvesters < 1 && lv->expand_idle && !bench_force_expand) || d->state == DIR_DEFEND
        || d->posture == POSTURE_HOLD)
        return;
    if (!dir_find_site(house, d, &d->site)) {
        if (bench_file)
            logmsg("director: house %d found no expansion site", FIELD(house, 0x30, int));
        return;
    }
    d->want_mcv = 1;
    if (!wanted) {
        d->want_mcv_frame = CURRENT_FRAME;
        logmsg("director: house %d wants an expansion at %d,%d (idle harvesters %d)", FIELD(house, 0x30, int),
               d->site.X, d->site.Y, d->idle_harvesters);
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

static int dir_near_water(CellXY c, int r);
static int dir_is_land(CellXY c);

/* Where the ferry puts troops ashore: the zone lookup's cell beside the enemy base, unless that
 * is on our own land (zone labels mislead); else the shore cell off our land nearest to it. */
static CellXY dir_landing_spot(DirState *d, CellXY e)
{
    CellXY out = { 0, 0 };
    ((nearby_fn)MAP_NEARBY)(MAP_INSTANCE, &out, &e, 1, dir_zone(e), 0, 0, 2, 2, 1, 0, 0, 0, &d->base, 0, 0);
    dir_fill_land(d->base);
    if (out.X > 0 && !dir_land_reachable(out))
        return out;
    for (int r = 0; r < 40; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;
                CellXY c = { (short)(e.X + dx), (short)(e.Y + dy) };
                BYTE *cell = dir_cell(c);
                if (!cell || dir_is_land(c))
                    continue;
                int land = FIELD(cell, C_LANDTYPE, int);
                if (land != 2 && land != 3 && land != 4 && !(FIELD(cell, C_OCCUPATION, DWORD) & 0x80)
                    && dir_near_water(c, 2))
                    return c;
            }
    return (CellXY){ 0, 0 };
}

/* Ground units of ours that could board: armed, not hover/air, not across already, on this side. */
/* Transports fill by size: Passengers= is the room (TechnoTypeClass +0x5E0, read at 0x714B3C), Size=
 * what each passenger takes (+0x380, a double, read at 0x71251F); PassengerClass::GetTotalSize
 * (0x473460, on TechnoClass +0x114) sums those aboard. Four tanks fill a hover transport. */
#define TT_PASSENGER_ROOM 0x5E0
#define TT_SIZE 0x380
static int dir_size(BYTE *o)
{
    double size = FIELD(dir_type(o), TT_SIZE, double);
    return size < 1 ? 1 : (int)(size + 0.999);
}

static int dir_free_room(BYTE *t)
{
    return FIELD(dir_type(t), TT_PASSENGER_ROOM, int) - ((int (GTHISCALL *)(BYTE *))0x473460)(t + T_PASSENGERS);
}

static int dir_boardable(BYTE *house, DirState *d, BYTE *o, int *what)
{
    /* not harvesters, nor the slaves of a Slave Miner (dir_poolable skips them; called to board, they never came) */
    return oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && ((*what = dir_whatami(o)) == 1 || *what == 15)
        && !(*what == 1 && dir_type(o)[UT_HARVESTER])
        && !dir_is_ferry(d, o) && !dir_naval(o) && !dir_crosses(o) && dir_armed(o) && !dir_landed(d, object_cell(o));
}

/* The beach cell nearest `at` (within 30 cells) on land our units can walk to from `from`, clear
 * of buildings, with water within 2 cells and away from `avoid` (a beach transports couldn't get
 * to; 0,0: none); 0,0 if there is none. */
static CellXY dir_beach_near(CellXY at, CellXY from, CellXY avoid)
{
    dir_fill_walk(from);
    for (int r = 0; r <= 30; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;
                CellXY c = { (short)(at.X + dx), (short)(at.Y + dy) };
                BYTE *cell = dir_cell(c);
                if (cell && FIELD(cell, C_LANDTYPE, int) == 6 && !(FIELD(cell, C_OCCUPATION, DWORD) & 0x80)
                    && dir_is_land(c) && dir_near_water(c, 2) && (!avoid.X || dir_dist2(c, avoid) > 6 * 6))
                    return c;
            }
    return (CellXY){ 0, 0 };
}

/* A transport's passengers: a count (T_PASSENGERS) and a chain through ObjectClass::NextObject
 * (0x30) from the first (T_PASSENGERS + 4). Twice on Isolation the game crashed walking a chain
 * with a dead object in it (0x473491, in PassengerClass::GetTotalSize, from a unit asking to
 * board; and earlier at 0x6F3731, perhaps the same). A chain that runs into anything not in the
 * techno list is cut there, before the engine walks it, and logged. */
#define O_NEXT_OBJECT 0x30
static void dir_check_passengers(BYTE *house, BYTE *t)
{
    int count = FIELD(t, T_PASSENGERS, int);
    BYTE *prev = NULL, *p = FIELD(t, T_PASSENGERS + 4, BYTE *);
    int i = 0;
    for (; i < count && p && dir_object_listed(OIL_TECHNO_ARRAY, p) && FIELD(p, O_OWNER, BYTE *); i++) {
        prev = p;
        p = FIELD(p, O_NEXT_OBJECT, BYTE *);
    }
    if (i == count)
        return;
    logmsg("director: house %d frame %d: %.24s passenger list broken at %d of %d (%p, vtable %#x, health %d); cut "
           "there", FIELD(house, 0x30, int), CURRENT_FRAME, (char *)dir_type(t) + T_ID, i, count, (void *)p,
           p ? FIELD(p, 0, DWORD) : 0, p ? FIELD(p, 0x6C, int) : 0);
    if (prev)
        FIELD(prev, O_NEXT_OBJECT, BYTE *) = NULL;
    else
        FIELD(t, T_PASSENGERS + 4, BYTE *) = NULL;
    FIELD(t, T_PASSENGERS, int) = i;
}

static void dir_ferry(BYTE *house, DirState *d)
{
    dir_why = "ferry";
    int side = FIELD(house, OIL_H_SIDE, int);
    if (!(d->island || d->blocked) || side < 0 || side > 2)
        return;
    DynVec *v = OIL_TECHNO_ARRAY;
    /* transports wanted: one per six ground units waiting on this side, two to four */
    if (CURRENT_FRAME % 300 < 15) {
        int n = 0, what;
        for (int i = 0; i < v->Count; i++)
            n += dir_boardable(house, d, v->Items[i], &what) && dir_poolable(v->Items[i], what);
        d->home_ground = n;
        d->ferry_want = n / 6 < 1 ? 1 : n / 6 > DIR_CONVOY ? DIR_CONVOY : n / 6;
    }
    for (int k = 0; k < DIR_CONVOY; k++) {
        BYTE *t = d->ferry[k];
        if (t && (!dir_object_listed(OIL_TECHNO_ARRAY, t) || !oil_live(t) || FIELD(t, O_OWNER, BYTE *) != house
                  || t == d->col_ferry))
            d->ferry[k] = NULL;
        if (d->ferry[k])
            continue;
        for (int i = 0; i < v->Count && !d->ferry[k]; i++) {
            BYTE *o = v->Items[i];
            if (oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_whatami(o) == 1 && o != d->col_ferry
                && !dir_is_ferry(d, o) && !FIELD(o, T_PASSENGERS, int)
                && !_stricmp((char *)dir_type(o) + T_ID, dir_transports[side]))
                d->ferry[k] = o;   /* not the colonising transport (both ordered it about, and it went nowhere),
                                    * nor one an abandoned colonisation left an engineer in */
        }
        d->ferry_state[k] = 0;
        d->ferry_frame[k] = CURRENT_FRAME;
        d->ferry_docked[k] = 0;
    }
    if (!dir_ferry_count(d))
        return;
    for (int k = 0; k < DIR_CONVOY; k++)
            if (d->ferry[k])
                dir_check_passengers(house, d->ferry[k]);
    /* The dock: a beach (land type 6, water beside it) near the rally that our ground units can walk
     * to. Docking at the rally itself wedged transports in among the waiting army on cliff-top
     * bases, where they never got out to the water again. Re-picked every 3000 frames. */
    if (!d->dock.X || CURRENT_FRAME >= d->dock_frame) {
        d->dock_frame = CURRENT_FRAME + 3000;
        d->dock = dir_beach_near(d->rally, d->rally, d->dock_failed);
        if (!d->dock.X)
            d->dock = d->rally;
    }
    /* a stock team that recruited a transport orders it about too (area guard), and it ignored the
     * sailing order with a full load aboard */
    int docked = 0, ready = 0, coming = 0;
    for (int k = 0; k < DIR_CONVOY; k++) {
        BYTE *t = d->ferry[k];
        if (!t)
            continue;
        if (FIELD(t, F_TEAM, BYTE *))
            dir_take_from_team(t);
        CellXY at = object_cell(t);
        int passengers = FIELD(t, T_PASSENGERS, int);
        /* no headway for 900 frames while it should be moving (to the dock, across, home): it is
         * wedged in. Docking, it loads where it stands; out at sea or homeward, the order is given
         * again, and after a second stall it unloads (sailing) or counts as home. */
        int moving = (d->ferry_state[k] == 0 && !d->ferry_docked[k]) || d->ferry_state[k] == 1 || d->ferry_state[k] == 3;
        if (!moving || dir_dist2(at, d->ferry_seen[k]) > 2 * 2) {
            d->ferry_seen[k] = at;
            d->ferry_seen_frame[k] = CURRENT_FRAME;
        } else if (CURRENT_FRAME - d->ferry_seen_frame[k] > 900) {
            logmsg("director: house %d frame %d: ferry %d stuck at %d,%d (state %d), %s", FIELD(house, 0x30, int),
                   CURRENT_FRAME, k, at.X, at.Y, d->ferry_state[k],
                   d->ferry_state[k] == 0 ? "loading here" : "ordering it again");
            d->ferry_seen_frame[k] = CURRENT_FRAME;
            if (d->ferry_state[k] == 0)
                d->ferry_frame[k] = CURRENT_FRAME - 601;   /* the dock is blocked: load where it stands */
            else if (d->ferry_state[k] == 1) {
                if (CURRENT_FRAME - d->ferry_frame[k] > 1800)
                    d->ferry_frame[k] = CURRENT_FRAME - 4001;   /* unload where it is */
                else
                    dir_order(t, MISSION_MOVE, NULL, dir_cell(d->landing));
            } else
                d->ferry_frame[k] = CURRENT_FRAME - 6001;   /* call it home */
        }
        if (bench_file && CURRENT_FRAME % 900 < 15)
            logmsg("director: house %d ferry %d %.24s state %d aboard %d at %d,%d dock %d,%d mission %d",
                   FIELD(house, 0x30, int), k, (char *)dir_type(t) + T_ID, d->ferry_state[k], passengers, at.X, at.Y,
                   d->dock.X, d->dock.Y, FIELD(t, COMBAT_MISSION, int));
        if (d->ferry_state[k] == 0 && !d->ferry_docked[k]) {   /* come ashore first */
            if (dir_dist2(at, d->dock) <= 6 * 6 || CURRENT_FRAME - d->ferry_frame[k] > 600) {
                /* Kept from the dock, it loads where it stands, but only on land our units can walk
                 * to: transports waiting in the water under a cliff-top base had the army called to
                 * the cliff edge, where it crowded for good and no convoy sailed again. Off that
                 * land, the dock moves to the beach on it nearest the transport. */
                dir_fill_walk(d->rally);
                if (dir_dist2(at, d->dock) > 6 * 6 && !dir_is_land(at)) {
                    CellXY to = dir_beach_near(at, d->rally, d->dock);
                    if (to.X) {
                        logmsg("director: house %d frame %d: ferry %d can't get from %d,%d to the dock at %d,%d; "
                               "dock moved to %d,%d", FIELD(house, 0x30, int), CURRENT_FRAME, k, at.X, at.Y,
                               d->dock.X, d->dock.Y, to.X, to.Y);
                        d->dock_failed = d->dock;
                        d->dock = to;
                        d->dock_frame = CURRENT_FRAME + 3000;
                        d->ferry_frame[k] = CURRENT_FRAME;
                        coming++;
                        dir_order(t, MISSION_MOVE, NULL, dir_cell(d->dock));
                        continue;
                    }
                }
                d->ferry_docked[k] = d->ferry_aboard_frame[k] = CURRENT_FRAME;   /* blocked: load where it stands */
                d->ferry_aboard[k] = passengers;
                /* at the dock it may still roll on to the dock cell itself: that is where it loads */
                d->ferry_dock_at[k] = dir_dist2(at, d->dock) <= 6 * 6 ? d->dock : at;
            } else {
                coming++;
                if (!dir_recent_order(t, dir_cell(d->dock), 450))
                    dir_order(t, MISSION_MOVE, NULL, dir_cell(d->dock));
                continue;
            }
        }
        /* Docked, it may still be moved off (something orders transports about every few hundred
         * frames on Isolation; sent back each time, one went back and forth 590 times a game). Off
         * by more than 8 cells it loads where it now stands if our units can walk there, and goes
         * back to the dock only from water or land they can't reach. */
        if (d->ferry_state[k] == 0 && dir_dist2(at, d->ferry_dock_at[k]) > 8 * 8) {
            dir_fill_walk(d->rally);
            int walkable = dir_is_land(at);
            static int last_log[32];
            int idx = FIELD(house, 0x30, int) & 31;
            if (CURRENT_FRAME - last_log[idx] > 1500) {
                last_log[idx] = CURRENT_FRAME;
                logmsg("director: house %d frame %d: ferry %d left its dock at %d,%d for %d,%d; %s", idx,
                       CURRENT_FRAME, k, d->ferry_dock_at[k].X, d->ferry_dock_at[k].Y, at.X, at.Y,
                       walkable ? "loads there" : "back to the dock");
            }
            if (walkable) {
                d->ferry_dock_at[k] = at;
            } else {
                d->ferry_docked[k] = 0;
                d->ferry_frame[k] = CURRENT_FRAME;
                dir_order(t, MISSION_MOVE, NULL, dir_cell(d->dock));
                coming++;
                continue;
            }
        }
        if (d->ferry_state[k] == 0) {
            docked++;
            if (passengers != d->ferry_aboard[k]) {
                d->ferry_aboard[k] = passengers;
                d->ferry_aboard_frame[k] = CURRENT_FRAME;
            }
            /* full; nobody boarded for a while (sooner with a few aboard: a load of tanks fills it by
             * size, four); soldiers walk over in a straggling line and board one by one */
            int still = CURRENT_FRAME - d->ferry_aboard_frame[k];
            ready += dir_free_room(t) < 3 || (passengers && ((passengers >= 3 && still > 450) || still > 900));
        } else if (d->ferry_state[k] == 1) {   /* sail, then unload beside the enemy */
            if (dir_dist2(at, d->landing) <= 4 * 4 || CURRENT_FRAME - d->ferry_frame[k] > 4000) {
                dir_order(t, 16, NULL, NULL);
                d->ferry_state[k] = 2;
                d->ferry_frame[k] = CURRENT_FRAME;
            } else if (!dir_recent_order(t, dir_cell(d->landing), 450))
                dir_order(t, MISSION_MOVE, NULL, dir_cell(d->landing));
        } else if (d->ferry_state[k] == 2) {   /* wait until empty, then go home for the next load */
            if (!passengers || CURRENT_FRAME - d->ferry_frame[k] > 900) {
                d->ferry_state[k] = 3;
                d->ferry_frame[k] = CURRENT_FRAME;
                dir_order(t, MISSION_MOVE, NULL, dir_cell(d->dock));
            } else if (!dir_recent_order(t, (BYTE *)2, 150))
                dir_order(t, 16, NULL, NULL);
        } else if (dir_dist2(at, d->dock) <= 5 * 5 || CURRENT_FRAME - d->ferry_frame[k] > 6000) {
            d->ferry_state[k] = 0;
            d->ferry_frame[k] = CURRENT_FRAME;
            d->ferry_docked[k] = 0;
        }
    }
    if (!docked) {
        d->convoy_since = 0;
        return;
    }
    if (!d->convoy_since)
        d->convoy_since = CURRENT_FRAME;
    /* Call the nearest idle ground units on this side, each to the docked transport with the most room. */
    /* room left in each docked transport, by size, less what is already walking over to it */
    int called = 0, distant = 0, busy = 0, room[DIR_CONVOY], npool = 0, pool_dist[256], boarding[DIR_CONVOY] = { 0 };
    BYTE *pool[256];
    for (int k = 0; k < DIR_CONVOY; k++)
        room[k] = d->ferry[k] && d->ferry_state[k] == 0 && d->ferry_docked[k] ? dir_free_room(d->ferry[k]) : 0;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i];
        int what;
        if (!dir_boardable(house, d, o, &what))
            continue;
        if (FIELD(o, COMBAT_MISSION, int) == MISSION_ENTER && dir_is_ferry(d, FIELD(o, COMBAT_DESTINATION, BYTE *))) {
            called++;
            for (int k = 0; k < DIR_CONVOY; k++)
                if (d->ferry[k] == FIELD(o, COMBAT_DESTINATION, BYTE *)) {
                    room[k] -= dir_size(o);
                    boarding[k] |= dir_dist2(object_cell(o), object_cell(d->ferry[k])) <= 2 * 2;
                }
            continue;
        }
        if (!dir_poolable(o, what)) {
            busy++;
            continue;
        }
        CellXY c = object_cell(o);
        int dist = dir_dist2(c, d->dock);
        if (dist > 40 * 40) {
            distant++;
            continue;
        }
        if (npool < (int)(sizeof pool / sizeof *pool)) {
            pool[npool] = o;
            pool_dist[npool++] = dist;
        }
    }
    /* nearest first: called in array order, units from the back of the army pushed through the
     * rest down a ramp to the beach and the whole crowd jammed there */
    for (int n = 0; n < npool; n++) {
        int pick = n;
        for (int j = n + 1; j < npool; j++)
            if (pool_dist[j] < pool_dist[pick])
                pick = j;
        BYTE *o = pool[pick];
        pool[pick] = pool[n];
        pool_dist[pick] = pool_dist[n];
        /* the transport with the most room that it fits in; called beyond the room, units crowded
         * round full transports at the dock, gave up and jammed the way down for the rest */
        int best = -1, size = dir_size(o);
        for (int k = 0; k < DIR_CONVOY; k++)
            if (room[k] >= size && (best < 0 || room[k] > room[best]))
                best = k;
        if (best < 0)
            continue;   /* a smaller unit may still fit */
        room[best] -= size;
        called++;
        /* Told to board from afar, the unit has the transport drive out to meet it: with callers
         * all over the base, transports left the dock again and again, chasing first one and then
         * another, and nobody got aboard in 20000 frames. Units walk over first and board from
         * within 7 cells (any of them once the convoy has waited 1500 frames). */
        CellXY tc = object_cell(d->ferry[best]);
        if (dir_dist2(object_cell(o), tc) > 7 * 7 && CURRENT_FRAME - d->convoy_since < 1500) {
            if (!dir_recent_order(o, dir_cell(tc), 300))
                dir_order(o, MISSION_MOVE, NULL, dir_cell(tc));
        } else if (!dir_recent_order(o, d->ferry[best], 300)) {
            /* Entering a transport follows Destination, not Target */
            ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(o, VT_QUEUEMISSION))(o, MISSION_ENTER, 0);
            ((void (GTHISCALL *)(BYTE *, BYTE *, char))VFUNC(o, COMBAT_SET_DESTINATION))(o, d->ferry[best], 1);
        }
    }
    if (bench_file && CURRENT_FRAME % 900 < 15) {
        int free = 0;
        for (int k = 0; k < DIR_CONVOY; k++)
            free += room[k] > 0 ? room[k] : 0;
        logmsg("director: house %d frame %d: convoy %d docked, %d ready, %d coming; %d called, %d in the pool, %d "
               "far, %d busy; room left %d, waited %d", FIELD(house, 0x30, int), CURRENT_FRAME, docked, ready, coming,
               called, npool, distant, busy, free, CURRENT_FRAME - d->convoy_since);
    }
    /* Nobody aboard any docked transport 3000 frames after the first call, with units called: they
     * can't get to it (four landing craft clumped on a beach pocket sat empty for 45000 frames with
     * 20 called). The dock moves (away from where they stand), the transports go there and the
     * called units are let go; what the units were doing is logged. */
    int aboard_docked = 0;
    for (int k = 0; k < DIR_CONVOY; k++)
        if (d->ferry[k] && d->ferry_state[k] == 0 && d->ferry_docked[k])
            aboard_docked += FIELD(d->ferry[k], T_PASSENGERS, int);
    if (called && !aboard_docked && CURRENT_FRAME - d->convoy_since > 3000) {
        BYTE *t0 = NULL;
        for (int k = 0; k < DIR_CONVOY && !t0; k++)
            if (d->ferry[k] && d->ferry_state[k] == 0 && d->ferry_docked[k])
                t0 = d->ferry[k];
        CellXY stuck = t0 ? object_cell(t0) : d->dock;
        char seen[200];
        int len = 0, shown = 0;
        seen[0] = 0;
        for (int i = 0; i < v->Count; i++) {
            BYTE *o = v->Items[i];
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house)
                continue;
            int m = FIELD(o, COMBAT_MISSION, int);
            BYTE *dest = FIELD(o, COMBAT_DESTINATION, BYTE *);
            int to_ferry = m == MISSION_ENTER && dir_is_ferry(d, dest);
            /* not one already at the transport: called off in the middle of boarding, it was counted
             * aboard but never joined the passengers (lists broken that way, and Isolation games
             * crashed walking them) */
            if (to_ferry && dir_dist2(object_cell(o), object_cell(dest)) > 3 * 3) {
                ((void (GTHISCALL *)(BYTE *, BYTE *, char))VFUNC(o, COMBAT_SET_DESTINATION))(o, NULL, 1);
                ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(o, VT_QUEUEMISSION))(o, MISSION_AREA_GUARD, 1);
            }
            if ((to_ferry || (m == MISSION_MOVE && dir_dist2(object_cell(o), stuck) < 20 * 20)) && shown < 6 && len < 160) {
                CellXY c = object_cell(o);
                len += sprintf(seen + len, " %.6s@%d,%d/m%d/%dc", (char *)dir_type(o) + T_ID, c.X, c.Y, m,
                               dir_isqrt(dir_dist2(c, to_ferry && dest ? object_cell(dest) : stuck)));
                shown++;
            }
        }
        CellXY to = dir_beach_near(d->rally, d->rally, stuck);
        logmsg("director: house %d frame %d: convoy stalled, %d called and nobody aboard (transport at %d,%d, dock "
               "%d,%d); dock moved to %d,%d; units:%s", FIELD(house, 0x30, int), CURRENT_FRAME, called, stuck.X, stuck.Y,
               d->dock.X, d->dock.Y, to.X, to.Y, seen);
        d->dock_failed = stuck;
        if (to.X)
            d->dock = to;
        d->dock_frame = CURRENT_FRAME + 6000;
        for (int k = 0; k < DIR_CONVOY; k++)
            if (d->ferry[k] && d->ferry_state[k] == 0 && d->ferry_docked[k]) {
                d->ferry_docked[k] = 0;
                d->ferry_frame[k] = CURRENT_FRAME;
                dir_order(d->ferry[k], MISSION_MOVE, NULL, dir_cell(d->dock));
            }
        d->convoy_since = 0;
        return;
    }
    /* The convoy sails together: when every docked transport is ready, none is still on its way to
     * the dock and nobody is still walking over to board (each waited for up to 900 and 1800
     * frames), or when nobody else will come, or after 3000 frames at the dock. */
    int waited = CURRENT_FRAME - d->convoy_since;
    int together = ready == docked && (!coming || waited >= 900) && (!called || waited >= 1800);
    if (!ready || (waited < 3000 && !together && !(!called && waited > 300)))
        return;
    CellXY out = dir_landing_spot(d, dir_house_center(d->enemy));
    if (out.X <= 0)
        out = d->landing;   /* crowded now: the last landing spot */
    if (out.X <= 0) {
        static int last_log;
        if (waited > 1200 && CURRENT_FRAME - last_log > 1500 && (last_log = CURRENT_FRAME))
            logmsg("director: house %d frame %d: the ferry finds no landing near the enemy", FIELD(house, 0x30, int),
                   CURRENT_FRAME);
        return;
    }
    d->landing = out;
    d->convoy_since = 0;
    int ships = 0, aboard = 0;
    for (int k = 0; k < DIR_CONVOY; k++) {
        BYTE *t = d->ferry[k];
        /* not while a unit beside it is getting in: sailing off mid-boarding left it counted aboard
         * but out of the passenger list */
        if (!t || d->ferry_state[k] != 0 || !d->ferry_docked[k] || !FIELD(t, T_PASSENGERS, int) || boarding[k])
            continue;
        ships++;
        aboard += FIELD(t, T_PASSENGERS, int);
        d->ferry_state[k] = 1;
        d->ferry_frame[k] = CURRENT_FRAME;
        dir_order(t, MISSION_MOVE, NULL, dir_cell(out));
    }
    logmsg("director: house %d frame %d: convoy of %d sails with %d aboard to %d,%d (left behind: %d still coming, "
           "%d over 40 cells away, %d busy; %d on this side, %d transports wanted)", FIELD(house, 0x30, int),
           CURRENT_FRAME, ships, aboard, out.X, out.Y, called, distant, busy, d->home_ground, d->ferry_want);
    /* units still on their way in hold a transport where it is: release them */
    char late[200];
    int nlate = 0, len = 0;
    late[0] = 0;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i];
        BYTE *dest = oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && FIELD(o, COMBAT_MISSION, int) == MISSION_ENTER
            ? FIELD(o, COMBAT_DESTINATION, BYTE *) : NULL;
        if (dest && dir_is_ferry(d, dest) && nlate++ < 8 && len < 170) {
            CellXY c = object_cell(o);
            len += sprintf(late + len, " %d,%d", c.X, c.Y);
        }
        for (int k = 0; dest && k < DIR_CONVOY; k++)
            if (dest == d->ferry[k] && d->ferry_state[k] == 1 && dir_dist2(object_cell(o), object_cell(dest)) > 3 * 3) {
                ((void (GTHISCALL *)(BYTE *, BYTE *, char))VFUNC(o, COMBAT_SET_DESTINATION))(o, NULL, 1);
                ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(o, VT_QUEUEMISSION))(o, MISSION_AREA_GUARD, 1);
            }
    }
    if (nlate && bench_file)
        logmsg("director: house %d frame %d: %d left walking to board, at%s (dock %d,%d, rally %d,%d)",
               FIELD(house, 0x30, int), CURRENT_FRAME, nlate, late, d->dock.X, d->dock.Y, d->rally.X, d->rally.Y);
    if (bench_file && npool > 2 * nlate + 4) {   /* left on this side: what they do */
        len = 0;
        for (int n = 0; n < npool && n < 6 && len < 150; n++) {
            BYTE *o = pool[n];
            if (!dir_object_listed(OIL_TECHNO_ARRAY, o) || !oil_live(o))
                continue;
            CellXY c = object_cell(o);
            len += sprintf(late + len, " %.6s@%d,%d/m%d", (char *)dir_type(o) + T_ID, c.X, c.Y, FIELD(o, COMBAT_MISSION, int));
        }
        logmsg("director: house %d frame %d: %d left in the pool, nearest:%s", FIELD(house, 0x30, int), CURRENT_FRAME, npool, late);
    }
}

/* ---- navy ----
 * Stock AI leaves its ships to trigger teams, which rarely answer ships shelling the base. While
 * enemy ships are near our buildings, every armed ship of ours attacks the nearest one it can hit,
 * and the yard builds warships until our fleet matches them. */
/* Within r cells of open water: a structure ships can shell. */
static int dir_near_water(CellXY c, int r)
{
    for (int dy = -r; dy <= r; dy += 2)
        for (int dx = -r; dx <= r; dx += 2) {
            BYTE *cell = dir_cell((CellXY){ (short)(c.X + dx), (short)(c.Y + dy) });
            if (cell && FIELD(cell, C_LANDTYPE, int) == 2)
                return 1;
        }
    return 0;
}

/* Open water the fleet can sail to: a flood fill over water cells from a ship's own cell. */
static unsigned char dir_sea[512 * 512 / 8];
static int dir_sea_queue[512 * 512], dir_sea_log = -100000;

static int dir_is_sea(CellXY c)
{
    return c.X > 0 && c.Y > 0 && c.X < 512 && c.Y < 512 && (dir_sea[(c.Y * 512 + c.X) >> 3] >> (c.X & 7) & 1);
}

static int dir_fill_sea(CellXY from)
{
    memset(dir_sea, 0, sizeof dir_sea);
    BYTE *start = dir_cell(from);
    if (!start)
        return 0;
    int head = 0, tail = 0;
    dir_sea_queue[tail++] = from.Y * 512 + from.X;
    dir_sea[(from.Y * 512 + from.X) >> 3] |= 1 << (from.X & 7);
    while (head < tail) {
        int i = dir_sea_queue[head++], x = i % 512, y = i / 512;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                CellXY n = { (short)(x + dx), (short)(y + dy) };
                BYTE *cell = dir_cell(n);
                if (!cell || dir_is_sea(n) || FIELD(cell, C_LANDTYPE, int) != 2)
                    continue;
                dir_sea[(n.Y * 512 + n.X) >> 3] |= 1 << (n.X & 7);
                dir_sea_queue[tail++] = n.Y * 512 + n.X;
            }
    }
    if (bench_file && CURRENT_FRAME - dir_sea_log > 3000) {
        dir_sea_log = CURRENT_FRAME;
        logmsg("director: frame %d: sea from %d,%d (land %d): %d water cells", CURRENT_FRAME, from.X, from.Y,
               FIELD(start, C_LANDTYPE, int), tail);
    }
    return tail;
}

static int dir_near_sea(CellXY c, int r)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r && dir_is_sea((CellXY){ (short)(c.X + dx), (short)(c.Y + dy) }))
                return 1;
    return 0;
}

/* The fleet's next target: the nearest enemy ship on our water, else the target enemy's (then
 * anyone's) structure within gun range of it. Targets it could not get at are skipped. */
static DirEnemy *dir_navy_pick(DirState *d, CellXY from, CellXY ship)
{
    dir_fill_sea(ship);
    DirEnemy *best = NULL;
    int best_score = 0x7FFFFFFF;
    for (int pass = 0; pass < 3 && !best; pass++)
        for (int i = 0; i < dir_enemy_count; i++) {
            DirEnemy *e = &dir_enemies[i];
            int bad = 0;
            for (int k = 0; k < d->navy_bad_count; k++)
                bad |= d->navy_bad[k] == e->obj;
            if (bad || e->air || e->capturable)
                continue;
            if (pass == 0 ? !e->naval || !dir_near_sea(e->at, 2)
                : !e->building || (pass == 1 && FIELD(e->obj, O_OWNER, BYTE *) != d->enemy) || !dir_near_sea(e->at, 6))
                continue;
            int score = dir_dist2(from, e->at);
            if (score < best_score) {
                best_score = score;
                best = e;
            }
        }
    return best;
}

static void dir_navy(BYTE *house, DirState *d)
{
    dir_why = "navy";
    dir_flags = director_enabled(house);
    BYTE *ships[64];
    int n = 0, fleet = 0, sx = 0, sy = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count && n < 64; i++) {
        BYTE *o = v->Items[i];
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 1 || !dir_naval(o)
            || !dir_armed(o))
            continue;
        ships[n++] = o;
        fleet += dir_cost(dir_type(o));
        CellXY c = object_cell(o);
        sx += c.X;
        sy += c.Y;
    }
    CellXY centre = n ? (CellXY){ (short)(sx / n), (short)(sy / n) } : d->base;
    d->fleet_value = fleet;
    d->fleet_at = centre;
    /* Submarines caught on the surface by aircraft with no anti-air about break off: they dive and
     * come round at an angle, then attack again from there. Surfaced to fire, a Boomer sat under
     * Siege Choppers until it sank. */
    static struct { BYTE *unit; int until; } evade[64];
    int m = 0;
    d->fleet_air = 0;
    for (int i = 0; i < n; i++) {
        BYTE *o = ships[i];
        CellXY at = object_cell(o);
        int air = 0, ax = 0, ay = 0, na = 0, aa = 0;
        for (int k = 0; k < dir_enemy_count; k++)
            if (dir_enemies[k].air && dir_enemies[k].armed && dir_dist2(dir_enemies[k].at, at) <= 7 * 7) {
                air += dir_enemies[k].value;
                ax += dir_enemies[k].at.X;
                ay += dir_enemies[k].at.Y;
                na++;
            }
        d->fleet_air += air;
        int slot = -1, busy = 0;
        for (int k = 0; k < 64; k++) {
            if (evade[k].unit == o)
                slot = k, busy = CURRENT_FRAME < evade[k].until;
            else if (slot < 0 && CURRENT_FRAME >= evade[k].until)
                slot = k;
        }
        if (busy)
            continue;   /* breaking off: no fleet orders */
        if (na && air >= 1000 && in_list("SUB,BSUB", (char *)dir_type(o) + T_ID) && slot >= 0) {
            DynVec *tv = OIL_TECHNO_ARRAY;
            for (int k = 0; k < tv->Count; k++) {
                BYTE *u = tv->Items[k], *ut;
                if (oil_live(u) && FIELD(u, O_OWNER, BYTE *) == house && (ut = dir_type(u))
                    && in_list("AEGIS,HYD,DISK,FV,HTK,YTNK,GGI,FLAKT", (char *)ut + T_ID)
                    && dir_dist2(object_cell(u), at) <= 8 * 8)
                    aa += dir_cost(ut);
            }
            if (aa < air) {
                /* away from the aircraft, turned 60 degrees, 9 cells: the first water cell found */
                int vx = at.X - ax / na, vy = at.Y - ay / na, len = dir_isqrt(vx * vx + vy * vy);
                if (!len)
                    vx = 1, vy = 0, len = 1;
                int sign = (CURRENT_FRAME / 450) & 1 ? 1 : -1;
                int rx = (vx * 500 - sign * vy * 866) / 1000, ry = (vy * 500 + sign * vx * 866) / 1000;
                for (int r = 9; r >= 4; r--) {
                    CellXY to = { (short)(at.X + rx * r / len), (short)(at.Y + ry * r / len) };
                    BYTE *cell = dir_cell(to);
                    if (cell && FIELD(cell, C_LANDTYPE, int) == 2) {
                        dir_why = "sub evades";
                        ((void (GTHISCALL *)(BYTE *, BYTE *))VFUNC(o, 0x3C8))(o, NULL);   /* stop firing */
                        dir_order(o, MISSION_MOVE, NULL, cell);
                        evade[slot].unit = o;
                        evade[slot].until = CURRENT_FRAME + 450;
                        logmsg("director: house %d frame %d: %.24s under air attack (%d, anti-air %d) dives toward %d,%d",
                               FIELD(house, 0x30, int), CURRENT_FRAME, (char *)dir_type(o) + T_ID, air, aa, to.X, to.Y);
                        break;
                    }
                }
                if (evade[slot].unit == o && CURRENT_FRAME < evade[slot].until)
                    continue;
            }
        }
        ships[m++] = o;
    }
    n = m;
    /* Yuri has no anti-air ship: Floating Discs, whose lasers hit aircraft, fly cover over the fleet */
    if (FIELD(house, OIL_H_SIDE, int) == 2 && d->fleet_air >= 1500 && CURRENT_FRAME - d->cover_frame > 150) {
        d->cover_frame = CURRENT_FRAME;
        DynVec *tv = OIL_TECHNO_ARRAY;
        int k = 0;
        for (int i = 0; i < tv->Count && k < 3; i++) {
            BYTE *u = tv->Items[i];
            if (oil_live(u) && FIELD(u, O_OWNER, BYTE *) == house && !_stricmp((char *)dir_type(u) + T_ID, "DISK")
                && dir_whatami(u) == 1) {
                d->cover_unit[k++] = u;
                dir_why = "fleet cover";
                dir_command(u, centre, NULL, 1);
            }
        }
        while (k < 3)
            d->cover_unit[k++] = NULL;
    }
    int want = d->naval_threat >= 600 && fleet < d->naval_threat;
    if (want != d->want_navy)
        logmsg("director: house %d frame %d: enemy ships %d near the base, fleet %d%s", FIELD(house, 0x30, int),
               CURRENT_FRAME, d->naval_threat, fleet, want ? ", building warships" : "");
    d->want_navy = want;

    /* Defence first: ships shelling the base. */
    if (d->naval_threat >= 600) {
        for (int i = 0; i < n; i++) {
            BYTE *o = ships[i];
            dir_take_from_team(o);
            CellXY at = object_cell(o);
            DirEnemy *best = NULL;
            int best_d = 0x7FFFFFFF;
            for (int k = 0; k < dir_enemy_count; k++) {
                DirEnemy *e = &dir_enemies[k];
                int dd = dir_dist2(at, e->at);
                if (e->threat && e->naval && dd < best_d && dir_can_fire_at(o, e->obj)) {
                    best_d = dd;
                    best = e;
                }
            }
            if (best && FIELD(o, O_TARGET, BYTE *) != best->obj && !dir_recent_order(o, best->obj, 150))
                dir_order(o, MISSION_ATTACK, best->obj, NULL);
        }
        return;
    }

    /* Offence: a fleet of three or more that outvalues the enemy ships around the target goes
     * hunting ships, then shells structures by the water. */
    if (d->navy_state == 0) {
        int enemy_ships = 0;
        CellXY target_base = d->enemy ? dir_house_center(d->enemy) : centre;
        for (int k = 0; k < dir_enemy_count; k++)
            if (dir_enemies[k].naval && dir_enemies[k].armed
                && (FIELD(dir_enemies[k].obj, O_OWNER, BYTE *) == d->enemy || dir_dist2(dir_enemies[k].at, target_base) <= 30 * 30))
                enemy_ships += dir_enemies[k].value;
        if (n < 3 || fleet < dir_levers(d)->fleet_launch || (long long)fleet * 10 < (long long)enemy_ships * 12)
            return;
        DirEnemy *t = dir_navy_pick(d, centre, object_cell(ships[0]));
        if (!t)
            return;
        d->navy_state = 1;
        d->navy_launch = fleet;
        d->navy_target = t->obj;
        d->navy_target_at = t->at;
        d->navy_best = 0x7FFFFFFF;
        d->navy_progress = CURRENT_FRAME;
        logmsg("director: house %d frame %d: fleet of %d (%d) sails for %.24s at %d,%d", FIELD(house, 0x30, int),
               CURRENT_FRAME, n, fleet, (char *)dir_type(t->obj) + T_ID, t->at.X, t->at.Y);
    }
    if (n == 0 || fleet * 10 < d->navy_launch * 4) {   /* bled out: home to the yard */
        logmsg("director: house %d frame %d: fleet down to %d of %d, returning", FIELD(house, 0x30, int), CURRENT_FRAME,
               fleet, d->navy_launch);
        d->navy_state = 0;
        for (int i = 0; i < n; i++)
            dir_command(ships[i], d->base, NULL, 0);
        return;
    }
    int alive = 0;
    for (int k = 0; k < dir_enemy_count; k++)
        alive |= dir_enemies[k].obj == d->navy_target;
    if (!alive || CURRENT_FRAME % 300 < 15) {
        DirEnemy *t = dir_navy_pick(d, centre, object_cell(ships[0]));
        if (!t) {
            d->navy_state = 0;
            return;
        }
        if (t->obj != d->navy_target)
            d->navy_best = 0x7FFFFFFF;
        d->navy_target = t->obj;
        d->navy_target_at = t->at;
    }
    /* progress: the closest ship gets closer, or ships are fighting near the target */
    int dd = 0x7FFFFFFF, fighting = 0;
    for (int i = 0; i < n; i++) {
        int u = dir_dist2(object_cell(ships[i]), d->navy_target_at);
        dd = u < dd ? u : dd;
        fighting |= FIELD(ships[i], O_TARGET, BYTE *) != NULL && u <= 20 * 20;
    }
    if (fighting || (long long)dd * 10 < (long long)d->navy_best * 9) {
        d->navy_best = dd < d->navy_best ? dd : d->navy_best;
        d->navy_progress = CURRENT_FRAME;
    } else if (CURRENT_FRAME - d->navy_progress > 1500) {
        logmsg("director: house %d frame %d: fleet can't reach %.24s at %d,%d", FIELD(house, 0x30, int), CURRENT_FRAME,
               (char *)dir_type(d->navy_target) + T_ID, d->navy_target_at.X, d->navy_target_at.Y);
        d->navy_bad[d->navy_bad_count++ % 8] = d->navy_target;
        if (d->navy_bad_count > 8)
            d->navy_bad_count = 8;
        d->navy_target = NULL;
        d->navy_progress = CURRENT_FRAME;
        d->navy_best = 0x7FFFFFFF;
        return;
    }
    for (int i = 0; i < n; i++) {
        dir_take_from_team(ships[i]);
        /* ships that can hit the target attack it; the rest (subs against buildings) escort and
         * fight what they meet */
        int can = dir_can_fire_at(ships[i], d->navy_target);
        dir_command(ships[i], d->navy_target_at, can ? d->navy_target : NULL, 1);
    }
    if (CURRENT_FRAME - d->last_navy_log > 1500) {
        d->last_navy_log = CURRENT_FRAME;
        logmsg("director: house %d frame %d: fleet %d ships (%d) attacking %.24s at %d,%d, closest %d cells",
               FIELD(house, 0x30, int), CURRENT_FRAME, n, fleet, (char *)dir_type(d->navy_target) + T_ID,
               d->navy_target_at.X, d->navy_target_at.Y, dir_isqrt(dd));
    }
}

/* Benchmark diagnostics: our units next to a protected human's buildings, and why they are there. */
static void dir_report_intruders(BYTE *house)
{
    DynVec *bv = OIL_BUILDING_ARRAY, *tv = OIL_TECHNO_ARRAY;
    for (int i = 0; i < tv->Count; i++) {
        BYTE *o = tv->Items[i];
        int what;
        if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || ((what = dir_whatami(o)) != 1 && what != 15))
            continue;
        CellXY at = object_cell(o);
        for (int k = 0; k < bv->Count; k++) {
            BYTE *b = bv->Items[k], *owner = FIELD(b, O_OWNER, BYTE *);
            if (!oil_live(b) || !owner || !owner[H_ISHUMAN] || dir_dist2(object_cell(b), at) > 6 * 6)
                continue;
            BYTE *team = FIELD(o, F_TEAM, BYTE *), *dest = FIELD(o, COMBAT_DESTINATION, BYTE *);
            BYTE *tt = team ? FIELD(team, TEAM_TYPE, BYTE *) : NULL, *script = team ? FIELD(team, TEAM_SCRIPT, BYTE *) : NULL;
            BYTE *st = tt ? FIELD(tt, TT_SCRIPT, BYTE *) : NULL;
            CellXY dc = dest && is_cell(dest) ? FIELD(dest, C_MAPCOORDS, CellXY) : dest ? object_cell(dest) : (CellXY){ -1, -1 };
            logmsg("director: house %d frame %d: %.24s at %d,%d in the human base: mission %d dest %d,%d team %.24s "
                   "script %.24s line %d", FIELD(house, 0x30, int), CURRENT_FRAME, (char *)dir_type(o) + T_ID, at.X,
                   at.Y, FIELD(o, COMBAT_MISSION, int), dc.X, dc.Y, tt ? (char *)tt + T_ID : "-",
                   st ? (char *)st + T_ID : "-", script ? FIELD(script, SCRIPT_LINE, int) : -1);
            break;
        }
    }
}

/* Stock trigger teams pick their own production. Two picks are vetoed: more Kirovs once four are
 * out or while the base is being hit (slow and costly, they left bases undefended), and more
 * infantry when the enemy is cut off by water (they can only wait at home). */
static void dir_veto_production(BYTE *house, DirState *d)
{
    int air = FIELD(house, H_PRODUCING_AIR, int);
    DynVec *at = AIRCRAFTTYPE_ARRAY;
    if (air >= 0 && air < at->Count && !_stricmp((char *)at->Items[air] + T_ID, "ZEP")
        && (d->threat_value >= 1500 || dir_owned_of(house, at->Items[air]) >= 4)) {
        FIELD(house, H_PRODUCING_AIR, int) = -1;
        if (CURRENT_FRAME - d->last_veto_log > 1500) {
            d->last_veto_log = CURRENT_FRAME;
            logmsg("director: house %d frame %d: no more Kirovs (%s)", FIELD(house, 0x30, int), CURRENT_FRAME,
                   d->threat_value >= 1500 ? "base under attack" : "four out already");
        }
    }
    /* stock AI with MCV repacking buys MCVs of its own and parks extra yards side by side: two
     * yards (the base and one expansion) are enough; a lost yard can still be replaced */
    int pick = FIELD(house, H_PRODUCING_UNIT, int);
    DynVec *uts = UNITTYPE_ARRAY;
    if (pick >= 0 && pick < uts->Count && in_list(dir_mcvs, (char *)uts->Items[pick] + T_ID) && !d->want_mcv
        && pick != d->unit_request && combat_building_count(house, "GACNST,NACNST,YACNST") >= 2)
        FIELD(house, H_PRODUCING_UNIT, int) = -1;
    /* a stock pick the house can't build would hold the vehicle queue for good */
    if (pick >= 0 && pick < uts->Count && pick != d->unit_request
        && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, uts->Items[pick], 0, 1) <= 0)
        FIELD(house, H_PRODUCING_UNIT, int) = -1;
    /* the mod's Allied AI teams order Liberators in threes; a pair is all that still pays */
    int unit = FIELD(house, H_PRODUCING_UNIT, int);
    DynVec *ut = UNITTYPE_ARRAY;
    if (unit >= 0 && unit < ut->Count && !_stricmp((char *)ut->Items[unit] + T_ID, "ATTNK")
        && dir_owned_of(house, ut->Items[unit]) >= 2)
        FIELD(house, H_PRODUCING_UNIT, int) = -1;
    /* Destroyers traded 0.12 on Isolation (240k lost for 29k destroyed): a pair for subs at most */
    unit = FIELD(house, H_PRODUCING_UNIT, int);
    if (unit >= 0 && unit < ut->Count && unit != d->unit_request && !_stricmp((char *)ut->Items[unit] + T_ID, "DEST")
        && dir_owned_of(house, ut->Items[unit]) >= 2)
        FIELD(house, H_PRODUCING_UNIT, int) = -1;
    /* cut off by water, stock teams' ground vehicles past the home garrison (dir_ground_full);
     * transports, harvesters, MCVs, ships, hover/air units and anti-air during a raid still go */
    unit = FIELD(house, H_PRODUCING_UNIT, int);
    if (unit >= 0 && unit < ut->Count && unit != d->unit_request) {
        BYTE *type = ut->Items[unit];
        const char *id = (char *)type + T_ID;
        int side = FIELD(house, OIL_H_SIDE, int);
        if (!type[TT_NAVAL] && !in_list("HARV,CMIN,SMIN,LCRF,SAPC,YHVR,ROBO,ZEP,SCHP,DISK", id) && !in_list(dir_mcvs, id)
            && !(d->want_aa && side >= 0 && side <= 2 && in_list(dir_aa_vehicles[side], id)) && dir_ground_full(house, d)) {
            FIELD(house, H_PRODUCING_UNIT, int) = -1;
            if (CURRENT_FRAME - d->last_veto_log > 1500) {
                d->last_veto_log = CURRENT_FRAME;
                logmsg("director: house %d frame %d: no more %s (cut off by water, %d ground vehicles at home)",
                       FIELD(house, 0x30, int), CURRENT_FRAME, id, DIR_HOME_VEHICLES);
            }
        }
    }
    int inf = FIELD(house, H_PRODUCING_INF, int);
    DynVec *it = INFANTRYTYPE_ARRAY;
    if ((d->blocked || d->island) && inf >= 0 && inf < it->Count && FIELD(house, H_OWNED_INFANTRY, int) >= 24
        && !d->want_engineer && !in_list("ENGINEER,SENGINEER,YENGINEER", (char *)it->Items[inf] + T_ID))
        FIELD(house, H_PRODUCING_INF, int) = -1;
}

/* ---- air defence ----
 * Kirovs and Floating Discs ran up the best kill ratios of all because the AIs facing them built
 * little anti-air. Enemy aircraft near home or the army are remembered for a while (1% decay per
 * tick); while they outvalue our anti-air units, every side answers the same way: anti-air vehicles
 * and infantry in place of stock picks, and an anti-air defence at home every 3000 frames. */
static void dir_air_defense(BYTE *house, DirState *d)
{
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2)
        return;
    d->air_seen = d->air_near > d->air_seen - d->air_seen / 100 ? d->air_near : d->air_seen - d->air_seen / 100;
    d->own_aa = 0;
    DynVec *v = OIL_TECHNO_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *o = v->Items[i], *type;
        if (oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && (type = dir_type(o))
            && (in_list(dir_aa_vehicles[side], (char *)type + T_ID) || in_list(dir_aa_infantry[side], (char *)type + T_ID)))
            d->own_aa += dir_cost(type);
    }
    int want = d->air_seen >= 1500 && (long long)d->own_aa * 10 < (long long)d->air_seen * 12;
    if (want != d->want_aa)
        logmsg("director: house %d frame %d: enemy air %d nearby, our anti-air %d: %s", FIELD(house, 0x30, int),
               CURRENT_FRAME, d->air_seen, d->own_aa, want ? "building anti-air" : "enough anti-air");
    d->want_aa = want;
    if (CURRENT_FRAME < d->next_aa_defense || FIELD(house, OIL_H_PRODUCING, int) != -1)
        return;
    BYTE *type = find_type(BUILDINGTYPE_ARRAY, dir_aa_defenses[side]);
    /* a standing pair at home whatever the sky looks like (Flak Cannons for the Soviets), once
     * the house can spare the money; more while aircraft are actually raiding */
    int standing = type && CURRENT_FRAME >= 9000 && combat_building_count(house, dir_aa_defenses[side]) < 2
        && FIELD(house, OIL_H_CASH, int) >= dir_cost(type) + 2000;
    if (!standing && (!want || d->air_near < 1500))
        return;
    if (type && FIELD(house, OIL_H_CASH, int) >= dir_cost(type)
        && ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, type, 0, 1) > 0) {
        FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
        d->next_aa_defense = CURRENT_FRAME + 3000;
        logmsg("director: house %d frame %d: queued %s against air", FIELD(house, 0x30, int), CURRENT_FRAME,
               dir_aa_defenses[side]);
    }
}

/* ---- Allied infantry posts ----
 * GIs and Guardian GIs are at their best dug in. Six of them hold posts at the base edge facing
 * the enemy: about 9 cells from the base centre towards the rally, spread across that line. Each
 * walks to its post and deploys there; a dead one is replaced from the army. */
static void dir_posts(BYTE *house, DirState *d)
{
    if (FIELD(house, OIL_H_SIDE, int) != 0 || CURRENT_FRAME < d->next_posts || !d->enemy)
        return;
    d->next_posts = CURRENT_FRAME + 150;
    dir_why = "post";
    if (d->post_rally.X != d->rally.X || d->post_rally.Y != d->rally.Y) {   /* lay out the posts */
        d->post_rally = d->rally;
        int dx = d->rally.X - d->base.X, dy = d->rally.Y - d->base.Y, len = dir_isqrt(dx * dx + dy * dy);
        if (!len)
            return;
        static const int across[6] = { -2, 2, -5, 5, -8, 8 };   /* the centre posts fill first */
        for (int k = 0; k < 6; k++) {
            CellXY c = { (short)(d->base.X + dx * 9 / len - dy * across[k] / len),
                         (short)(d->base.Y + dy * 9 / len + dx * across[k] / len) };
            BYTE *cell = dir_cell(c);
            int land = cell ? FIELD(cell, C_LANDTYPE, int) : 2;
            d->post_cell[k] = cell && land != 2 && land != 3 && land != 4 && !(FIELD(cell, C_OCCUPATION, DWORD) & 0x80)
                ? c : (CellXY){ 0, 0 };
        }
    }
    DynVec *tv = OIL_TECHNO_ARRAY;
    /* as many posts as the enemy army calls for: two, plus one per 8000 of its value, at most six;
     * soldiers of posts no longer needed rejoin the army (which packs them up) */
    int wanted = 2 + d->target_army / 8000;
    wanted = wanted > 6 ? 6 : wanted;
    for (int k = wanted; k < 6; k++)
        d->post_unit[k] = NULL;
    for (int k = 0; k < wanted; k++) {
        if (!d->post_cell[k].X)
            continue;
        BYTE *u = d->post_unit[k];
        if (u && (!dir_object_listed(tv, u) || !oil_live(u) || FIELD(u, O_OWNER, BYTE *) != house))
            u = d->post_unit[k] = NULL;
        if (!u) {   /* the nearest free GI or Guardian GI of the army */
            int best_d = 40 * 40 + 1;
            for (int i = 0; i < tv->Count; i++) {
                BYTE *o = tv->Items[i], *ot;
                if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 15 || !(ot = dir_type(o))
                    || !in_list(dir_deployers, (char *)ot + T_ID) || !dir_poolable(o, 15))
                    continue;
                int taken = 0;
                for (int m = 0; m < 6; m++)
                    taken |= d->post_unit[m] == o;
                int dd = dir_dist2(object_cell(o), d->post_cell[k]);
                if (!taken && dd < best_d) {
                    best_d = dd;
                    u = o;
                }
            }
            if (!u)
                continue;
            d->post_unit[k] = u;
            d->post_toggled[k] = -100000;
        }
        CellXY at = object_cell(u);
        if (dir_deployed(u))
            continue;
        if (dir_dist2(at, d->post_cell[k]) <= 1) {
            if (CURRENT_FRAME - d->post_toggled[k] >= 300) {
                dir_order(u, MISSION_UNLOAD, NULL, NULL);   /* dig in */
                logmsg("director: house %d frame %d: %.24s digs in at post %d (%d,%d)", FIELD(house, 0x30, int),
                       CURRENT_FRAME, (char *)dir_type(u) + T_ID, k, at.X, at.Y);
                d->post_toggled[k] = CURRENT_FRAME;
            }
        } else if (!dir_recent_order(u, dir_cell(d->post_cell[k]), 450))
            dir_order(u, MISSION_MOVE, NULL, dir_cell(d->post_cell[k]));
    }
}

/* ---- Battle Fortresses ----
 * Five infantry fire out of a Battle Fortress (OpenTopped). Every fortress of ours is kept full of
 * GIs and Guardian GIs: the nearest idle ones within 25 cells board it, as onto a ferry (Enter, with
 * the fortress as the destination). */
static void dir_man_fortresses(BYTE *house, DirState *d)
{
    if (FIELD(house, OIL_H_SIDE, int) != 0)
        return;
    dir_why = "fortress";
    DynVec *tv = OIL_TECHNO_ARRAY;
    int sent = 0;
    for (int i = 0; i < tv->Count && sent < 3; i++) {
        BYTE *f = tv->Items[i], *ft;
        if (!oil_live(f) || FIELD(f, O_OWNER, BYTE *) != house || dir_whatami(f) != 1 || !(ft = dir_type(f))
            || _stricmp((char *)ft + T_ID, "BFRT"))
            continue;
        int pending = 0;
        for (int k = 0; k < 24; k++)
            pending += d->garrison_site[k] == f && CURRENT_FRAME - d->garrison_frame[k] < 600;
        int need = 5 - FIELD(f, T_PASSENGERS, int) - pending;
        CellXY at = object_cell(f);
        while (need-- > 0 && sent < 3) {
            BYTE *best = NULL;
            int best_d = 25 * 25 + 1;
            for (int k = 0; k < tv->Count; k++) {
                BYTE *o = tv->Items[k], *ot;
                if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 15 || !(ot = dir_type(o))
                    || !in_list(dir_deployers, (char *)ot + T_ID) || !dir_poolable(o, 15) || dir_deployed(o))
                    continue;
                int busy = 0;
                for (int m = 0; m < 24; m++)
                    busy |= d->garrison_unit[m] == o && CURRENT_FRAME - d->garrison_frame[m] < 900;
                for (int m = 0; m < 6; m++)
                    busy |= d->post_unit[m] == o;
                int dd = dir_dist2(object_cell(o), at);
                if (!busy && dd < best_d) {
                    best_d = dd;
                    best = o;
                }
            }
            if (!best)
                break;
            int g = d->garrison_next++ % 24;
            d->garrison_unit[g] = best;
            d->garrison_site[g] = f;
            d->garrison_frame[g] = CURRENT_FRAME;
            ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(best, VT_QUEUEMISSION))(best, MISSION_ENTER, 0);
            ((void (GTHISCALL *)(BYTE *, BYTE *, char))VFUNC(best, COMBAT_SET_DESTINATION))(best, f, 1);
            sent++;
            static int last_log;
            if (CURRENT_FRAME - last_log > 600) {
                last_log = CURRENT_FRAME;
                logmsg("director: house %d frame %d: %.24s boards the Battle Fortress at %d,%d (%d aboard)",
                       FIELD(house, 0x30, int), CURRENT_FRAME, (char *)dir_type(best) + T_ID, at.X, at.Y,
                       FIELD(f, T_PASSENGERS, int));
            }
        }
    }
}

/* ---- civilian buildings ----
 * Garrisoned, a civilian building is a free bunker for whoever holds it; shooting it down only
 * denies it. Every 300 frames the director sends the nearest idle garrison infantry (Occupier=yes)
 * into empty civilian buildings within 30 cells of home or the rally, one soldier each, up to six.
 * The order is a player's enter click: Enter with the building as the destination. */
#define BT_CAN_BE_OCCUPIED 0x157B       /* BuildingTypeClass::CanBeOccupied (INI loader 0x4600CB) */
#define B_OCCUPANT_COUNT 0x694          /* BuildingClass::Occupants.Count (KillOccupants 0x4585D9) */
#define IT_OCCUPIER 0xEB4               /* InfantryTypeClass::Occupier (INI loader 0x5244CE) */
#define BT_MAX_OCCUPANTS 0x1580         /* BuildingTypeClass::MaxNumberOccupants */

/* The enter order into our own bunker names it as the target too. A soldier that finds it full
 * leaves the Enter mission but keeps the target, and opens fire on our own bunker. Every unit of
 * ours aiming at one of our garrisonable buildings drops it when that is full, or when the unit has
 * stopped going in (anything but Move, Enter or Capture: stock teams send soldiers in that way). */
/* The stock base planner places each building at its node in the base plan, nudged a little. On a
 * cramped island it found no room for the Battle Lab, and the AI's building queue then waited on
 * it for good: no defences, factories or labs again (and no Floating Discs, which need the lab),
 * with the money piling up. Where the planner finds nothing, the nearest spot to our base centre
 * that the engine's own placement check accepts is taken instead. */
/* Ground height of a cell, in leptons (one level is 104): a cliff is several levels. */
static int dir_height(CellXY c)
{
    Coord at = { c.X * 256 + 128, c.Y * 256 + 128, 0 };
    return ((int (GTHISCALL *)(void *, Coord *))MAP_FLOOR_HEIGHT)(MAP_INSTANCE, &at);
}

/* A building type's footprint, from BuildingTypeClass::Foundation (+0xEF0, as LoadFromINI stores it at
 * 0x461248) and YRpp's Foundation enum. */
#define BT_FOUNDATION 0xEF0
static void dir_foundation(BYTE *type, int *w, int *h)
{
    static const unsigned char size[22][2] = { {1,1}, {2,1}, {1,2}, {2,2}, {2,3}, {3,2}, {3,3}, {3,5}, {4,2}, {3,3},
        {1,3}, {3,1}, {4,3}, {1,4}, {1,5}, {2,6}, {2,5}, {5,3}, {4,4}, {3,4}, {6,4}, {1,1} };
    int f = FIELD(type, BT_FOUNDATION, int);
    if (f < 0 || f >= 22)
        f = 18;   /* unknown: assume 4x4 */
    *w = size[f][0];
    *h = size[f][1];
}

/* A building at tl would cut a passage: with its cells blocked, the open cells around it no longer
 * all connect within 10 cells. Buildings put in a ramp or a gap between cliffs sealed bases off
 * from their beach, and armies from the way out. */
static int dir_blocks_passage(BYTE *type, CellXY tl)
{
    enum { R = 10, W = 2 * R + 1 };
    static unsigned char seen[W * W];
    static CellXY queue[W * W];
    int w, h;
    dir_foundation(type, &w, &h);
    CellXY mid = { (short)(tl.X + w / 2), (short)(tl.Y + h / 2) };
    #define DIR_IN_FOOT(c) ((c).X >= tl.X && (c).X < tl.X + w && (c).Y >= tl.Y && (c).Y < tl.Y + h)
    #define DIR_OPEN(c, cell) ((cell) && FIELD(cell, C_LANDTYPE, int) != 2 && FIELD(cell, C_LANDTYPE, int) != 3 \
        && !dir_wall_cell(cell) && !(FIELD(cell, C_OCCUPATION, DWORD) & 0x80) && !DIR_IN_FOOT(c))
    CellXY ring[64];
    int nring = 0;
    for (int y = tl.Y - 1; y <= tl.Y + h; y++)
        for (int x = tl.X - 1; x <= tl.X + w; x++) {
            CellXY c = { (short)x, (short)y };
            BYTE *cell = dir_cell(c);
            if (!DIR_IN_FOOT(c) && DIR_OPEN(c, cell) && nring < 64)
                ring[nring++] = c;
        }
    /* by a ramp: walkable ground within 2 cells of it at another height (half a level or more; cliffs
     * are rock, which isn't walkable). A building at a ramp's top or bottom squeezed the only way down
     * to a crawl even where it didn't cut it. */
    int level = dir_height(tl);
    for (int y = tl.Y - 2; y <= tl.Y + h + 1; y++)
        for (int x = tl.X - 2; x <= tl.X + w + 1; x++) {
            CellXY c = { (short)x, (short)y };
            BYTE *cell = dir_cell(c);
            if (!DIR_IN_FOOT(c) && DIR_OPEN(c, cell) && abs(dir_height(c) - level) >= 52)
                return 1;
        }
    if (nring < 2)
        return 0;
    memset(seen, 0, sizeof seen);
    int head = 0, tail = 0;
    queue[tail++] = ring[0];
    seen[(ring[0].Y - mid.Y + R) * W + ring[0].X - mid.X + R] = 1;
    while (head < tail) {
        CellXY c = queue[head++];
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                CellXY n = { (short)(c.X + dx), (short)(c.Y + dy) };
                int bx = n.X - mid.X + R, by = n.Y - mid.Y + R;
                if ((!dx && !dy) || bx < 0 || by < 0 || bx >= W || by >= W || seen[by * W + bx])
                    continue;
                BYTE *cell = dir_cell(n);
                if (!DIR_OPEN(n, cell))
                    continue;
                seen[by * W + bx] = 1;
                queue[tail++] = n;
            }
    }
    for (int i = 1; i < nring; i++)
        if (!seen[(ring[i].Y - mid.Y + R) * W + ring[i].X - mid.X + R])
            return 1;
    return 0;
    #undef DIR_IN_FOOT
    #undef DIR_OPEN
}

/* The nearest spot to `base` (from-to cells) that the engine's placement check accepts, that cuts
 * no passage and that is within a level of `level`. */
static int dir_spot_near(BYTE *house, BYTE *type, CellXY base, int level, int from, int to, CellXY *out)
{
    for (int r = from; r <= to; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;
                CellXY c = { (short)(base.X + dx), (short)(base.Y + dy) };
                if (dir_cell(c) && abs(dir_height(c) - level) <= 104
                    && ((char (GTHISCALL *)(BYTE *, CellXY *, BYTE *))BTYPE_CAN_PLACE)(type, &c, house)
                    && !dir_blocks_passage(type, c)) {
                    *out = c;
                    return 1;
                }
            }
    return 0;
}

/* ... near the base centre (2-20 cells) and on the base's own level (not down a cliff on the beach below) */
static int dir_fallback_spot(BYTE *house, BYTE *type, CellXY *out)
{
    CellXY base = dir_house_center(house);
    return dir_spot_near(house, type, base, dir_height(base), 2, 20, out);
}

/* The stock planner's spot would cut a passage (a ramp, a gap between cliffs): the director's own
 * nearest spot that doesn't, or none (the building waits) when there is no such spot. Walls too:
 * the stock AI rings its refineries with them, and a ring by a ramp's mouth squeezed the only way
 * down to the beach to one cell; a wall piece goes at most 3 cells aside, or nowhere. */
#define BT_FACTORY 0xEB8   /* BuildingTypeClass::Factory, an RTTI (read at 0x450326); 0 for none */
/* A factory (or a plant or lab) down the cliff from the base: new units climbed the ramp against
 * the army coming down it, and the beach below filled up where transports dock. Refineries go by
 * the ore, defences where they're put, shipyards on the water. */
static int dir_off_level(BYTE *house, BYTE *type, CellXY at)
{
    return !in_list(DIR_WALLS, (char *)type + T_ID) && (FIELD(type, BT_FACTORY, int)
        || in_list("GAPOWR,NAPOWR,NANRCT,YAPOWR,GATECH,NATECH,YATECH,AMRADR,NARADR,NAPSIS,GAOREP,YAGRND,NACLON",
                   (char *)type + T_ID))
        && abs(dir_height(at) - dir_height(dir_house_center(house))) > 104;
}

/* A building's base plan node already has a cell: the AI puts it there when HouseClass 0x50B760
 * accepts the cell (called at 0x444FBA), and only otherwise asks FindBuildLocation, which the
 * passage check sits on. Walls and most planned buildings went down unchecked: a node that cuts
 * a passage, squeezes a ramp or is off the base's level is refused here, so the checked search
 * runs instead (and its spot becomes the node's). */
#define DIR_NODE_CELL_CALL 0x444FBA
#define DIR_NODE_CELL_OK 0x50B760
static char GFASTCALL dir_node_cell_ok(BYTE *house, void *unused, BYTE *type, CellXY *cell)
{
    (void)unused;
    char ok = ((char (GTHISCALL *)(BYTE *, BYTE *, CellXY *))DIR_NODE_CELL_OK)(house, type, cell);
    if (!ok || !cell || cell->X <= 0 || !dir_active(house) || !(director_enabled(house) & DIR_F_ECONOMY)
        || type[TT_NAVAL] || in_list("GAYARD,NAYARD,YAYARD", (char *)type + T_ID))
        return ok;
    if (!dir_off_level(house, type, *cell) && !dir_blocks_passage(type, *cell))
        return ok;
    static int last_log[32];
    int idx = FIELD(house, 0x30, int) & 31;
    if (CURRENT_FRAME - last_log[idx] > 600) {
        last_log[idx] = CURRENT_FRAME;
        logmsg("director: house %d frame %d: base plan spot %d,%d for %.24s refused (%s)", idx, CURRENT_FRAME,
               cell->X, cell->Y, (char *)type + T_ID, dir_off_level(house, type, *cell) ? "off level" : "passage");
    }
    return 0;
}

static void dir_check_passage(BYTE *house, BYTE *type, CellXY *out)
{
    if (out->X <= 0 || !dir_active(house) || !(director_enabled(house) & DIR_F_ECONOMY)
        || in_list("GAYARD,NAYARD,YAYARD", (char *)type + T_ID) || type[TT_NAVAL])
        return;   /* shipyards sit on water, by a beach (the ramp rule took that for a ramp) */
    int wall = in_list(DIR_WALLS, (char *)type + T_ID);
    int off_level = dir_off_level(house, type, *out);
    if (!off_level && !dir_blocks_passage(type, *out))
        return;
    CellXY was = *out;
    /* near where the planner wanted it first (a defence moved to the far side of the base guards
     * nothing it was meant to), then near the base centre */
    if (!dir_spot_near(house, type, was, dir_height(off_level ? dir_house_center(house) : was), 1, wall ? 3 : 6, out)
        && (wall || !dir_fallback_spot(house, type, out)))
        *out = off_level && !dir_blocks_passage(type, was) ? was : (CellXY){ 0, 0 };   /* off level beats never */
    static int last_log[32];
    int idx = FIELD(house, 0x30, int) & 31;
    if (CURRENT_FRAME - last_log[idx] > 600) {
        last_log[idx] = CURRENT_FRAME;
        logmsg("director: house %d frame %d: %.24s at %d,%d would %s, %s %d,%d", idx, CURRENT_FRAME,
               (char *)type + T_ID, was.X, was.Y, off_level ? "be off the base's level" : "cut a passage", out->X ? "placed at" : "no other spot", out->X, out->Y);
    }
}

static void dir_note_placement(BYTE *house, BYTE *type, CellXY *out)
{
    static int last_try[32], last_log[32];
    int idx = FIELD(house, 0x30, int) & 31;
    if (out->X > 0 || !dir_active(house) || !(director_enabled(house) & DIR_F_ECONOMY)
        || CURRENT_FRAME - last_try[idx] < 30 || in_list(DIR_WALLS, (char *)type + T_ID))
        return;   /* a wall piece the passage check refused stays unbuilt */
    last_try[idx] = CURRENT_FRAME;
    if (dir_fallback_spot(house, type, out)) {
        logmsg("director: house %d frame %d: no room for %.24s in the base plan, placed at %d,%d", idx,
               CURRENT_FRAME, (char *)type + T_ID, out->X, out->Y);
        return;
    }
    if (CURRENT_FRAME - last_log[idx] > 1500) {
        last_log[idx] = CURRENT_FRAME;
        logmsg("director: house %d frame %d: no room for %.24s anywhere near the base", idx, CURRENT_FRAME,
               (char *)type + T_ID);
    }
}

static void dir_drop_own_targets(BYTE *house, DirState *d, int all)
{
    DynVec *tv = OIL_TECHNO_ARRAY, *bv = OIL_BUILDING_ARRAY;
    for (int i = 0; i < (all ? tv->Count : 24); i++) {
        BYTE *o = all ? tv->Items[i] : d->garrison_unit[i], *t;
        /* a fresh order: the Enter mission takes over only on the unit's next mission update */
        if (!o || (!all && (CURRENT_FRAME - d->garrison_frame[i] < 90 || !dir_object_listed(tv, o)))
            || !oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house)
            continue;
        int what = dir_whatami(o), mission = FIELD(o, COMBAT_MISSION, int);
        BYTE *tt;
        if ((what != 1 && what != 15) || !(t = FIELD(o, O_TARGET, BYTE *)) || !dir_object_listed(bv, t)
            || FIELD(t, O_OWNER, BYTE *) != house || !(tt = FIELD(t, B_TYPE, BYTE *)) || !tt[BT_CAN_BE_OCCUPIED])
            continue;
        if ((mission == MISSION_MOVE || mission == MISSION_ENTER || mission == MISSION_CAPTURE)
            && FIELD(t, B_OCCUPANT_COUNT, int) < FIELD(tt, BT_MAX_OCCUPANTS, int))
            continue;
        CellXY c = object_cell(o), tc = object_cell(t);
        logmsg("director: house %d frame %d: %.24s at %d,%d stops firing on our %.24s at %d,%d (mission %d)",
               FIELD(house, 0x30, int), CURRENT_FRAME, (char *)dir_type(o) + T_ID, c.X, c.Y,
               (char *)dir_type(t) + T_ID, tc.X, tc.Y, mission);
        ((void (GTHISCALL *)(BYTE *, BYTE *))VFUNC(o, VT_SETTARGET))(o, NULL);
        ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(o, VT_QUEUEMISSION))(o, MISSION_AREA_GUARD, 1);
    }
}

static void dir_garrison(BYTE *house, DirState *d)
{
    dir_why = "garrison";
    dir_drop_own_targets(house, d, 0);   /* the soldiers just sent: at once */
    if (CURRENT_FRAME < d->next_garrison)
        return;
    d->next_garrison = CURRENT_FRAME + 300;
    dir_drop_own_targets(house, d, 1);   /* any other, as the slots are reused */
    DynVec *bv = OIL_BUILDING_ARRAY, *tv = OIL_TECHNO_ARRAY;
    int held = 0, sent = 0;
    for (int i = 0; i < bv->Count; i++) {
        BYTE *b = bv->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
        held += oil_live(b) && type && type[BT_CAN_BE_OCCUPIED] && FIELD(b, O_OWNER, BYTE *) == house
            && FIELD(b, B_OCCUPANT_COUNT, int) > 0 && !_strnicmp((char *)type + T_ID, "CA", 2);
    }
    /* our own garrisonable defences (the Soviet Battle Bunker) are filled to capacity; with no
     * garrison infantry free, the barracks trains some (Tesla Troopers can't garrison) */
    d->want_occupier = 0;
    for (int i = 0; i < bv->Count && sent < 3; i++) {
        BYTE *b = bv->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
        if (!oil_live(b) || !type || !type[BT_CAN_BE_OCCUPIED] || FIELD(b, O_OWNER, BYTE *) != house
            || !_strnicmp((char *)type + T_ID, "CA", 2))
            continue;
        /* on the way: still outside and still entering, however long the walk (a timed window let
         * slow walkers lapse, and the extra soldiers sent after them found the bunker full); one
         * stuck outside for 1800 frames no longer holds the place */
        int pending = 0;
        for (int k = 0; k < 24; k++) {
            BYTE *u = d->garrison_unit[k];
            pending += d->garrison_site[k] == b && u && CURRENT_FRAME - d->garrison_frame[k] < 1800
                && dir_object_listed(tv, u) && oil_live(u) && FIELD(u, COMBAT_MISSION, int) == MISSION_ENTER;
        }
        int need = FIELD(type, BT_MAX_OCCUPANTS, int) - FIELD(b, B_OCCUPANT_COUNT, int) - pending;
        CellXY at = object_cell(b);
        while (need-- > 0 && sent < 3) {
            BYTE *best = NULL;
            int best_d = 30 * 30 + 1;
            for (int k = 0; k < tv->Count; k++) {
                BYTE *o = tv->Items[k], *ot;
                if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 15 || !(ot = dir_type(o))
                    || !ot[IT_OCCUPIER] || !dir_poolable(o, 15) || FIELD(o, O_TARGET, BYTE *))
                    continue;
                int busy = 0;
                for (int m = 0; m < 24; m++)
                    busy |= d->garrison_unit[m] == o && CURRENT_FRAME - d->garrison_frame[m] < 900;
                int dd = dir_dist2(object_cell(o), at);
                if (!busy && dd < best_d) {
                    best_d = dd;
                    best = o;
                }
            }
            if (!best) {
                d->want_occupier = 1;
                break;
            }
            int g = d->garrison_next++ % 24;
            d->garrison_unit[g] = best;
            d->garrison_site[g] = b;
            d->garrison_frame[g] = CURRENT_FRAME;
            dir_order(best, MISSION_ENTER, b, b);
            sent++;
            logmsg("director: house %d frame %d: %.24s into our %.24s at %d,%d (%d of %d inside)", FIELD(house, 0x30, int),
                   CURRENT_FRAME, (char *)dir_type(best) + T_ID, (char *)type + T_ID, at.X, at.Y,
                   FIELD(b, B_OCCUPANT_COUNT, int), FIELD(type, BT_MAX_OCCUPANTS, int));
        }
    }
    if (held != d->garrison_held) {
        logmsg("director: house %d frame %d: %d civilian buildings garrisoned", FIELD(house, 0x30, int), CURRENT_FRAME, held);
        d->garrison_held = held;
    }
    for (int i = 0; i < bv->Count && held + sent < 6 && sent < 2; i++) {
        BYTE *b = bv->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
        if (!oil_live(b) || !type || !type[BT_CAN_BE_OCCUPIED] || !dir_passive(FIELD(b, O_OWNER, BYTE *))
            || FIELD(b, B_OCCUPANT_COUNT, int) > 0)
            continue;
        CellXY at = object_cell(b);
        if (dir_dist2(at, d->base) > 30 * 30 && dir_dist2(at, d->rally) > 30 * 30)
            continue;
        /* one soldier per building: someone is on the way, or one went and never got in (out of
         * reach); a building that was taken is no longer civilian */
        int pending = 0;
        for (int k = 0; k < 24; k++)
            pending |= d->garrison_site[k] == b && CURRENT_FRAME - d->garrison_frame[k] < 9000;
        if (pending)
            continue;
        int threatened = 0;   /* not into a building the enemy is already contesting */
        for (int k = 0; k < dir_enemy_count && !threatened; k++)
            threatened = dir_enemies[k].armed && dir_dist2(dir_enemies[k].at, at) <= 8 * 8;
        if (threatened)
            continue;
        BYTE *best = NULL;
        int best_d = 25 * 25 + 1;
        for (int k = 0; k < tv->Count; k++) {
            BYTE *o = tv->Items[k], *ot;
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 15 || !(ot = dir_type(o))
                || !ot[IT_OCCUPIER] || !dir_poolable(o, 15) || FIELD(o, O_TARGET, BYTE *))
                continue;
            int dd = dir_dist2(object_cell(o), at);
            if (dd < best_d) {
                best_d = dd;
                best = o;
            }
        }
        if (!best || dir_recent_order(best, b, 900))
            continue;
        sent++;
        int g = d->garrison_next++ % 24;
        d->garrison_unit[g] = best;   /* out of the army until it is inside */
        d->garrison_site[g] = b;
        d->garrison_frame[g] = CURRENT_FRAME;
        dir_order(best, MISSION_ENTER, NULL, b);
        logmsg("director: house %d frame %d: %.24s garrisons the %.24s at %d,%d (%d held)", FIELD(house, 0x30, int),
               CURRENT_FRAME, (char *)dir_type(best) + T_ID, (char *)type + T_ID, at.X, at.Y, held);
    }
}

/* ---- tank bunkers ----
 * Yuri's base plan (ThirdBaseDefenses) builds Tank Bunkers, but no AI ever drives a tank into one,
 * so they stood empty. Between attacks the director garrisons each empty bunker with the nearest
 * main battle tank, up to a third of the army; a tank in a bunker leaves the army pool. */
static const char *dir_bunker_tanks = "MTNK,TNKD,FV,HTNK,APOC,HTK,LTNK,YTNK";

static void dir_bunkers(BYTE *house, DirState *d)
{
    dir_why = "bunker";
    if (CURRENT_FRAME < d->next_bunker)
        return;
    d->next_bunker = CURRENT_FRAME + 150;
    int garrisoned = 0;
    for (int k = 0; k < 8; k++) {   /* orders that arrived, died or never got there */
        BYTE *u = d->bunker_unit[k];
        if (!u)
            continue;
        int live = dir_object_listed(OIL_TECHNO_ARRAY, u) && oil_live(u);
        if (live && FIELD(u, T_BUNKER_LINK, BYTE *)) {
            CellXY c = object_cell(d->bunker_site[k]);
            logmsg("director: house %d frame %d: %.24s is in the tank bunker at %d,%d", FIELD(house, 0x30, int),
                   CURRENT_FRAME, (char *)dir_type(u) + T_ID, c.X, c.Y);
            d->bunker_unit[k] = NULL;
        } else if (!live || !dir_object_listed(OIL_BUILDING_ARRAY, d->bunker_site[k]) || !oil_live(d->bunker_site[k])
                   || FIELD(d->bunker_site[k], T_BUNKER_LINK, BYTE *) || CURRENT_FRAME - d->bunker_frame[k] > 900) {
            if (live) {
                CellXY c = object_cell(u), bc = object_cell(d->bunker_site[k]);
                logmsg("director: house %d frame %d: %.24s at %d,%d never reached the bunker at %d,%d (mission %d, "
                       "link %p, bunker link %p, dest %p target %p bunker %p)", FIELD(house, 0x30, int), CURRENT_FRAME, (char *)dir_type(u) + T_ID,
                       c.X, c.Y, bc.X, bc.Y, FIELD(u, COMBAT_MISSION, int), FIELD(u, T_BUNKER_LINK, BYTE *),
                       FIELD(d->bunker_site[k], T_BUNKER_LINK, BYTE *), FIELD(u, COMBAT_DESTINATION, BYTE *),
                       FIELD(u, O_TARGET, BYTE *), d->bunker_site[k]);
            }
            if (live && !FIELD(u, T_BUNKER_LINK, BYTE *)) {
                ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(u, VT_QUEUEMISSION))(u, MISSION_AREA_GUARD, 1);
                int f = d->bunker_failed_next++ % 8;
                d->bunker_failed[f] = u;
                d->bunker_failed_frame[f] = CURRENT_FRAME;
            }
            d->bunker_unit[k] = NULL;
        }
    }
    if (d->state == DIR_ATTACK)
        return;
    DynVec *bv = OIL_BUILDING_ARRAY, *tv = OIL_TECHNO_ARRAY;
    for (int i = 0; i < tv->Count; i++) {
        BYTE *o = tv->Items[i];
        garrisoned += oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && dir_whatami(o) == 1
            && FIELD(o, T_BUNKER_LINK, BYTE *);
    }
    for (int i = 0; i < bv->Count; i++) {
        BYTE *b = bv->Items[i], *type;
        if (!oil_live(b) || FIELD(b, O_OWNER, BYTE *) != house || !(type = dir_type(b))
            || _stricmp((char *)type + T_ID, "NATBNK") || FIELD(b, T_BUNKER_LINK, BYTE *))
            continue;
        int slot = -1, pending = 0;
        for (int k = 0; k < 8; k++) {
            pending |= d->bunker_unit[k] && d->bunker_site[k] == b;
            if (!d->bunker_unit[k] && slot < 0)
                slot = k;
        }
        if (pending || slot < 0 || (garrisoned + 1) * 3 > d->army_count)
            continue;
        CellXY at = object_cell(b);
        BYTE *best = NULL;
        int best_d = 30 * 30 + 1;
        for (int k = 0; k < tv->Count; k++) {
            BYTE *o = tv->Items[k], *ot;
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 1 || !(ot = dir_type(o))
                || !in_list(dir_bunker_tanks, (char *)ot + T_ID) || !dir_poolable(o, 1) || FIELD(o, O_TARGET, BYTE *))
                continue;
            int busy = 0;
            for (int m = 0; m < 8; m++)
                busy |= d->bunker_unit[m] == o
                    || (d->bunker_failed[m] == o && CURRENT_FRAME - d->bunker_failed_frame[m] < 6000);
            int dd = dir_dist2(object_cell(o), at);
            if (!busy && dd < best_d) {
                best_d = dd;
                best = o;
            }
        }
        if (!best)
            continue;
        d->bunker_unit[slot] = best;
        d->bunker_site[slot] = b;
        d->bunker_frame[slot] = CURRENT_FRAME;
        garrisoned++;
        /* what a player's enter click does (FootClass::ClickedAction, Action::Enter at 0x4D76F6):
         * ClickedMission(Enter) with no target and the bunker as the destination */
        dir_order(best, MISSION_ENTER, NULL, b);
        logmsg("director: house %d frame %d: %.24s garrisons the tank bunker at %d,%d", FIELD(house, 0x30, int),
               CURRENT_FRAME, (char *)dir_type(best) + T_ID, at.X, at.Y);
    }
}

/* Benchmark fixture: warships for the first AI house on the open water nearest its base. */
static void dir_fleet_fixture(BYTE *house, DirState *d)
{
    static const char *ships[3] = { "DEST", "HYD", "BSUB" };
    int side = FIELD(house, OIL_H_SIDE, int);
    BYTE *type = side >= 0 && side <= 2 ? find_type(UNITTYPE_ARRAY, ships[side]) : NULL;
    int placed = 0;
    for (int r = 4; r < 60 && placed < bench_fleet && type; r++)
        for (int dy = -r; dy <= r && placed < bench_fleet; dy += 2)
            for (int dx = -r; dx <= r && placed < bench_fleet; dx += 2) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;
                CellXY c = { (short)(d->base.X + dx), (short)(d->base.Y + dy) };
                BYTE *cell = dir_cell(c);
                if (cell && FIELD(cell, C_LANDTYPE, int) == 2 && dir_near_water(c, 2) && put_object(type, house, c.X, c.Y, 0))
                    placed++;
            }
    logmsg("director: fleet fixture: %d %s for house %d", placed, type ? ships[side] : "-", FIELD(house, 0x30, int));
}

/* ---- land reachability ----
 * Land (and bridge decks) reachable from a cell by ground units: a flood fill over the whole map,
 * kept in a bitmap. Buildings are ignored: they come and go, water and cliffs don't. */
static unsigned char dir_land[512 * 512 / 8];

static int dir_is_land(CellXY c)
{
    return c.X > 0 && c.Y > 0 && c.X < 512 && c.Y < 512 && (dir_land[(c.Y * 512 + c.X) >> 3] >> (c.X & 7) & 1);
}

static void dir_fill_land_ex(CellXY from, int buildings_block);
static void dir_fill_land(CellXY from)
{
    dir_fill_land_ex(from, 0);
}

/* The same with buildings in the way: where our units can walk now. Convoy docks were picked on
 * land the army could reach in principle, and units called there stood 15-17 cells off behind
 * their own base, never arriving. */
static void dir_fill_walk(CellXY from)
{
    dir_fill_land_ex(from, 1);
}

static void dir_fill_land_ex(CellXY from, int buildings_block)
{
    memset(dir_land, 0, sizeof dir_land);
    if (!dir_cell(from))
        return;
    int head = 0, tail = 0;
    dir_sea_queue[tail++] = from.Y * 512 + from.X;
    dir_land[(from.Y * 512 + from.X) >> 3] |= 1 << (from.X & 7);
    while (head < tail) {
        int i = dir_sea_queue[head++], x = i % 512, y = i / 512;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                CellXY n = { (short)(x + dx), (short)(y + dy) };
                BYTE *cell = dir_cell(n);
                if (!cell || dir_is_land(n))
                    continue;
                int land = FIELD(cell, C_LANDTYPE, int);
                if ((land == 2 || land == 3) && !(FIELD(cell, C_FLAGS, DWORD) & 0x100))
                    continue;
                if (buildings_block && (FIELD(cell, C_OCCUPATION, DWORD) & 0x80))
                    continue;
                dir_land[(n.Y * 512 + n.X) >> 3] |= 1 << (n.X & 7);
                dir_sea_queue[tail++] = n.Y * 512 + n.X;
            }
    }
}

static int dir_land_reachable(CellXY c)
{
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if (dir_is_land((CellXY){ (short)(c.X + dx), (short)(c.Y + dy) }))
                return 1;
    return 0;
}

/* Some building of the house stands on the land filled last (dir_fill_land). */
static int dir_house_on_land(BYTE *h)
{
    DynVec *bv = OIL_BUILDING_ARRAY;
    for (int i = 0; i < bv->Count; i++) {
        BYTE *b = bv->Items[i];
        if (oil_live(b) && FIELD(b, O_OWNER, BYTE *) == h && dir_land_reachable(object_cell(b)))
            return 1;
    }
    return 0;
}

/* Our ground units can walk to the enemy: some building of theirs is on our land. Without a base
 * of ours on the map yet, or buildings of theirs, there is nothing to tell: assume they can. */
static int dir_land_house(DirState *d)
{
    dir_fill_land(d->base);
    if (!dir_is_land(d->base))
        return 1;
    DynVec *bv = OIL_BUILDING_ARRAY;
    int any = 0;
    for (int i = 0; i < bv->Count && !any; i++)
        any = oil_live(bv->Items[i]) && FIELD(bv->Items[i], O_OWNER, BYTE *) == d->enemy;
    return !any || dir_house_on_land(d->enemy);
}

/* ---- colonising islands ----
 * A civilian tech building (oil derrick first) on land our ground units can't reach is taken by
 * sea: an engineer boards an amphibious transport, which drives over, unloads beside the building
 * and goes home while the engineer captures it. The ore-outpost logic then fortifies the new
 * holding (and builds a refinery when there is ore). Each step has 3000 frames. */
static void dir_colonize(BYTE *house, DirState *d)
{
    int side = FIELD(house, OIL_H_SIDE, int);
    if (side < 0 || side > 2)
        return;
    dir_why = "colonise";
    d->want_col_ferry = 0;
    DynVec *tv = OIL_TECHNO_ARRAY, *bv = OIL_BUILDING_ARRAY;
    /* a transport sent home after a failed attempt: unload at the rally (an engineer left aboard was lost
     * to the house, and the transport with it: convoys skip loaded transports) */
    BYTE *rt = d->col_return;
    if (rt && (!dir_object_listed(tv, rt) || !oil_live(rt) || !FIELD(rt, T_PASSENGERS, int)
               || CURRENT_FRAME - d->col_return_frame > 4000)) {
        if (rt == d->col_ferry)
            d->col_ferry = NULL;
        d->col_return = rt = NULL;
    }
    if (rt) {
        if (dir_dist2(object_cell(rt), d->dock) <= 5 * 5) {
            if (!dir_recent_order(rt, (BYTE *)3, 300))
                dir_order(rt, MISSION_UNLOAD, NULL, NULL);
        } else if (!dir_recent_order(rt, dir_cell(d->dock), 450))
            dir_order(rt, MISSION_MOVE, NULL, dir_cell(d->dock));
    }
    BYTE *towner = d->col_target && dir_object_listed(bv, d->col_target) ? FIELD(d->col_target, O_OWNER, BYTE *) : NULL;
    if (d->col_state && (CURRENT_FRAME - d->col_frame > 3000 || !d->col_target
                         || !dir_object_listed(bv, d->col_target) || !oil_live(d->col_target)
                         || (towner != house && !dir_passive(towner)))) {   /* taken by someone else first */
        logmsg("director: house %d frame %d: colonising given up (step %d)", FIELD(house, 0x30, int), CURRENT_FRAME,
               d->col_state);
        if (d->col_target)
            d->col_failed[d->col_failed_count++ % 8] = d->col_target;
        BYTE *ft = d->col_ferry;
        if (ft && dir_object_listed(tv, ft) && oil_live(ft) && FIELD(ft, T_PASSENGERS, int)) {
            d->col_return = ft;   /* bring the engineer home */
            d->col_return_frame = CURRENT_FRAME;
            dir_order(ft, MISSION_MOVE, NULL, dir_cell(d->dock.X ? d->dock : d->rally));
        } else
            d->col_ferry = NULL;
        d->col_eng = NULL;
        d->col_state = 0;
        d->next_col = CURRENT_FRAME + 3000;
        return;
    }
    if (d->col_state == 0) {
        if (CURRENT_FRAME < d->next_col || CURRENT_FRAME < 6000)
            return;
        d->next_col = CURRENT_FRAME + 3000;
        dir_fill_land(d->rally);
        BYTE *best = NULL;
        int best_d = 90 * 90 + 1;
        for (int i = 0; i < bv->Count; i++) {
            BYTE *b = bv->Items[i], *type = FIELD(b, B_TYPE, BYTE *);
            if (!oil_live(b) || !type || !dir_passive(FIELD(b, O_OWNER, BYTE *))
                || !in_list("CAOILD,CAAIRP,CATHOSP,CAOUTP,CAMACH,CAPOWR", (char *)type + T_ID))
                continue;
            CellXY at = object_cell(b);
            int failed = 0, armed = 0;
            for (int k = 0; k < 8; k++)
                failed |= d->col_failed[k] == b;
            for (int k = 0; k < dir_enemy_count && !armed; k++)
                armed = dir_enemies[k].armed && dir_dist2(dir_enemies[k].at, at) <= 12 * 12;
            int dd = dir_dist2(at, d->base) * (_stricmp((char *)type + T_ID, "CAOILD") ? 2 : 1);   /* oil first */
            if (failed || armed || dir_land_reachable(at) || dd >= best_d)
                continue;
            best_d = dd;
            best = b;
        }
        if (!best)
            return;
        d->col_target = best;
        d->col_ferry = d->col_eng = NULL;
        d->col_state = 1;
        d->col_tries = 0;
        d->col_frame = CURRENT_FRAME;
        CellXY at = object_cell(best);
        logmsg("director: house %d frame %d: colonising the %.24s at %d,%d across the water", FIELD(house, 0x30, int),
               CURRENT_FRAME, (char *)FIELD(best, B_TYPE, BYTE *) + T_ID, at.X, at.Y);
        return;
    }
    BYTE *t = d->col_ferry, *e = d->col_eng;
    if (t && (!dir_object_listed(tv, t) || !oil_live(t)))
        t = d->col_ferry = NULL;
    if (e && d->col_state < 3 && (!dir_object_listed(tv, e) || !oil_live(e)) && !(t && FIELD(t, T_PASSENGERS, int)))
        e = d->col_eng = NULL;
    CellXY target = object_cell(d->col_target);
    if (d->col_state == 1) {   /* an engineer and a transport, the engineer aboard */
        for (int i = 0; i < tv->Count && (!t || !e); i++) {
            BYTE *o = tv->Items[i], *ot;
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || !(ot = dir_type(o)))
                continue;
            int m = FIELD(o, COMBAT_MISSION, int);
            if (!t && !dir_is_ferry(d, o) && dir_whatami(o) == 1 && !_stricmp((char *)ot + T_ID, dir_transports[side])
                && !FIELD(o, T_PASSENGERS, int))
                t = d->col_ferry = o;
            else if (!e && o != d->repair_engineer && dir_is_engineer(o) && m != MISSION_ENTER && m != MISSION_CAPTURE)
                e = d->col_eng = o;
        }
        if (!e)
            d->want_engineer = 1;
        if (!t)
            d->want_col_ferry = 1;
        if (!t || !e)
            return;
        if (FIELD(t, T_PASSENGERS, int)) {   /* aboard: sail to the nearest open cell by the target */
            CellXY best = { 0, 0 };
            int best_d = 0x7FFFFFFF;
            for (int dy = -4; dy <= 4; dy++)
                for (int dx = -4; dx <= 4; dx++) {
                    CellXY c = { (short)(target.X + dx), (short)(target.Y + dy) };
                    BYTE *cell = dir_cell(c);
                    int land = cell ? FIELD(cell, C_LANDTYPE, int) : 2;
                    int dd = dir_dist2(c, object_cell(t));
                    if (cell && land != 2 && land != 3 && !(FIELD(cell, C_OCCUPATION, DWORD) & 0x80) && dd < best_d) {
                        best_d = dd;
                        best = c;
                    }
                }
            if (!best.X)
                return;
            d->col_landing = best;
            d->col_state = 2;
            d->col_frame = CURRENT_FRAME;
            dir_order(t, MISSION_MOVE, NULL, dir_cell(best));
            logmsg("director: house %d frame %d: engineer aboard, crossing to %d,%d", FIELD(house, 0x30, int),
                   CURRENT_FRAME, best.X, best.Y);
            return;
        }
        CellXY ea = object_cell(e);
        if (dir_dist2(object_cell(t), ea) > 3 * 3 && !dir_recent_order(t, dir_cell(ea), 300))
            dir_order(t, MISSION_MOVE, NULL, dir_cell(ea));
        if (!dir_recent_order(e, t, 300)) {
            ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(e, VT_QUEUEMISSION))(e, MISSION_ENTER, 0);
            ((void (GTHISCALL *)(BYTE *, BYTE *, char))VFUNC(e, COMBAT_SET_DESTINATION))(e, t, 1);
        }
        return;
    }
    if (d->col_state == 2) {   /* crossing; unload on arrival */
        if (!t)
            return;
        if (dir_dist2(object_cell(t), d->col_landing) <= 3 * 3) {
            dir_order(t, MISSION_UNLOAD, NULL, NULL);
            d->col_state = 3;
            d->col_frame = CURRENT_FRAME;
        } else if (!dir_recent_order(t, dir_cell(d->col_landing), 450))
            dir_order(t, MISSION_MOVE, NULL, dir_cell(d->col_landing));
        return;
    }
    if (d->col_state == 3) {   /* ashore: capture, and the transport goes home */
        if (t && FIELD(t, T_PASSENGERS, int)) {
            /* still aboard: the unload order again, and after 900 frames another landing cell (the shore
             * there was no place to unload); otherwise transports sat by the island, engineer inside */
            int waited = CURRENT_FRAME - d->col_frame;
            if (waited > 900 && waited % 900 < 15 && d->col_tries >= 3)
                d->col_frame = CURRENT_FRAME - 3001;   /* no landing works: give up (and bring it home) */
            else if (waited > 900 && waited % 900 < 15) {
                d->col_tries++;
                CellXY at = object_cell(t), alt = { 0, 0 };
                int best_d = 0x7FFFFFFF;
                for (int dy = -5; dy <= 5; dy++)
                    for (int dx = -5; dx <= 5; dx++) {
                        CellXY c = { (short)(target.X + dx), (short)(target.Y + dy) };
                        BYTE *cell = dir_cell(c);
                        int land = cell ? FIELD(cell, C_LANDTYPE, int) : 2, dd = dir_dist2(c, at);
                        if (cell && land != 2 && land != 3 && !(FIELD(cell, C_OCCUPATION, DWORD) & 0x80)
                            && dir_dist2(c, d->col_landing) >= 2 * 2 && dd < best_d) {
                            best_d = dd;
                            alt = c;
                        }
                    }
                if (alt.X) {
                    logmsg("director: house %d frame %d: can't unload at %d,%d, trying %d,%d", FIELD(house, 0x30, int),
                           CURRENT_FRAME, d->col_landing.X, d->col_landing.Y, alt.X, alt.Y);
                    d->col_landing = alt;
                    d->col_state = 2;
                    dir_order(t, MISSION_MOVE, NULL, dir_cell(alt));
                }
            } else if (!dir_recent_order(t, (BYTE *)4, 300))
                dir_order(t, MISSION_UNLOAD, NULL, NULL);
            return;
        }
        if (e && dir_object_listed(tv, e) && oil_live(e)) {
            dir_order(e, MISSION_CAPTURE, d->col_target, NULL);
            if (t)
                dir_order(t, MISSION_MOVE, NULL, dir_cell(d->rally));
            d->col_state = 4;
            d->col_frame = CURRENT_FRAME;
        }
        return;
    }
    if (d->col_state == 4 && FIELD(d->col_target, O_OWNER, BYTE *) == house) {
        logmsg("director: house %d frame %d: island %.24s at %d,%d taken", FIELD(house, 0x30, int), CURRENT_FRAME,
               (char *)FIELD(d->col_target, B_TYPE, BYTE *) + T_ID, target.X, target.Y);
        d->col_state = 0;
        d->next_outpost = 0;   /* fortify it next */
    }
}

/* ---- Yuri's Slave Miners ----
 * Yuri's refinery is the Slave Miner, deployed (YAREFN, UndeploysInto=SMIN). Stock AI leaves it
 * where it first deployed, so miners camp by a couple of weak ore drills while rich fields lie
 * unused. A miner whose surroundings have run thin packs up (the selling mission undeploys a
 * building with UndeploysInto that isn't a construction yard: BuildingClass::Mission_Selling,
 * 0x449CF0) and is sent to the richest field within 60 cells with a player's harvest click; a
 * harvesting Slave Miner deploys by the ore by itself. One move at a time, every 300 frames. */
static struct { BYTE *b; int seen; } dir_miner_seen[64];

static void dir_slave_miners(BYTE *house, DirState *d)
{
    if (FIELD(house, OIL_H_SIDE, int) != 2 || CURRENT_FRAME < d->next_miner)
        return;
    d->next_miner = CURRENT_FRAME + 300;
    DynVec *v = OIL_TECHNO_ARRAY, *bv = OIL_BUILDING_ARRAY;
    if (d->miner_site.X > 0) {   /* packed up: send the mobile miner to the field */
        if (CURRENT_FRAME - d->miner_frame > 3000) {
            d->miner_site = (CellXY){ 0, 0 };
            return;
        }
        for (int i = 0; i < v->Count; i++) {
            BYTE *o = v->Items[i];
            if (oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house && !_stricmp((char *)dir_type(o) + T_ID, "SMIN")
                && dir_dist2(object_cell(o), d->miner_from) <= 6 * 6) {
                dir_order(o, 10, NULL, dir_cell(d->miner_site));   /* Harvest, at the cell */
                logmsg("director: house %d frame %d: Slave Miner from %d,%d sent to the ore at %d,%d",
                       FIELD(house, 0x30, int), CURRENT_FRAME, d->miner_from.X, d->miner_from.Y, d->miner_site.X,
                       d->miner_site.Y);
                d->miner_site = (CellXY){ 0, 0 };
                return;
            }
        }
        return;
    }
    for (int i = 0; i < bv->Count; i++) {
        BYTE *b = bv->Items[i];
        if (!oil_live(b) || FIELD(b, O_OWNER, BYTE *) != house || _stricmp((char *)dir_type(b) + T_ID, "YAREFN"))
            continue;
        int k = 0, free_slot = -1;
        while (k < 64 && dir_miner_seen[k].b != b) {
            if (free_slot < 0 && (!dir_miner_seen[k].b || !dir_object_listed(bv, dir_miner_seen[k].b)))
                free_slot = k;
            k++;
        }
        if (k == 64) {   /* first seen now: it has just deployed */
            if (free_slot >= 0)
                dir_miner_seen[free_slot] = (typeof(dir_miner_seen[0])){ b, CURRENT_FRAME };
            CellXY at = object_cell(b);
            if (CURRENT_FRAME > 600)
                logmsg("director: house %d frame %d: Slave Miner deployed at %d,%d (ore around it %d)",
                       FIELD(house, 0x30, int), CURRENT_FRAME, at.X, at.Y, dir_ore_near(at, 8));
            continue;
        }
        if (CURRENT_FRAME - dir_miner_seen[k].seen < 4500)
            continue;
        CellXY c = object_cell(b), site;
        int here = dir_ore_near(c, 8);
        if (!dir_find_ore(house, c, &site) || dir_dist2(site, c) < 12 * 12)
            continue;
        int there = dir_ore_near(site, 8), taken = 0;
        for (int j = 0; j < bv->Count && !taken; j++) {
            BYTE *o = bv->Items[j];
            taken = o != b && oil_live(o) && FIELD(o, O_OWNER, BYTE *) == house
                && !_stricmp((char *)dir_type(o) + T_ID, "YAREFN") && dir_dist2(object_cell(o), site) <= 10 * 10;
        }
        if (taken || there < here * 4 || there < 1500)
            continue;
        logmsg("director: house %d frame %d: Slave Miner at %d,%d packs up (ore around it %d, at %d,%d %d)",
               FIELD(house, 0x30, int), CURRENT_FRAME, c.X, c.Y, here, site.X, site.Y, there);
        dir_sell(b);
        dir_miner_seen[k].seen = CURRENT_FRAME;
        d->miner_from = c;
        d->miner_site = site;
        d->miner_frame = CURRENT_FRAME;
        return;
    }
}

/* ---- strategy ----
 * At the start each house reads the setup (walking distance to the nearest enemy, how many enemies,
 * its starting base) and draws a plan from weights that fit it, with its own random stream seeded
 * from the match seed: the same setup can still bring a different plan, and a rerun with the same
 * seed brings the same one. [AIn] DirectorPlan=N forces one, for tests. */
static unsigned short dir_steps[512 * 512];

/* Walking distance, in cells, from `from` to the nearest base of a hostile house over land and
 * bridge decks; -1 when none can be walked to. */
static int dir_land_steps(BYTE *house, CellXY from)
{
    CellXY targets[32];
    int nt = 0;
    DynVec *hv = HOUSE_ARRAY;
    for (int i = 0; i < hv->Count && nt < 32; i++) {
        BYTE *h = hv->Items[i];
        if (dir_hostile(house, h) && dir_house_alive(h) && !h[H_ISHUMAN])
            targets[nt++] = dir_house_center(h);
    }
    if (!nt || !dir_cell(from))
        return -1;
    memset(dir_steps, 0xFF, sizeof dir_steps);
    int head = 0, tail = 0;
    dir_sea_queue[tail++] = from.Y * 512 + from.X;
    dir_steps[from.Y * 512 + from.X] = 0;
    while (head < tail) {
        int i = dir_sea_queue[head++], x = i % 512, y = i / 512, steps = dir_steps[i];
        for (int k = 0; k < nt; k++)
            if (abs(targets[k].X - x) <= 3 && abs(targets[k].Y - y) <= 3)
                return steps;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                CellXY n = { (short)(x + dx), (short)(y + dy) };
                BYTE *cell = dir_cell(n);
                if (!cell || dir_steps[n.Y * 512 + n.X] != 0xFFFF)
                    continue;
                int land = FIELD(cell, C_LANDTYPE, int);
                if ((land == 2 || land == 3) && !(FIELD(cell, C_FLAGS, DWORD) & 0x100))
                    continue;
                dir_steps[n.Y * 512 + n.X] = (unsigned short)(steps + 1);
                dir_sea_queue[tail++] = n.Y * 512 + n.X;
            }
    }
    return -1;
}

/* Water within 15 cells of home whose sea (a flood fill over water) comes within 6 cells of some
 * hostile house's building: a fleet from here could shell someone. */
static int dir_sea_reaches_enemy(BYTE *house, CellXY base)
{
    CellXY water = { 0, 0 };
    for (int r = 1; r <= 15 && !water.X; r++)
        for (int dy = -r; dy <= r && !water.X; dy++)
            for (int dx = -r; dx <= r; dx++) {
                CellXY c = { (short)(base.X + dx), (short)(base.Y + dy) };
                BYTE *cell = dir_cell(c);
                if ((abs(dx) == r || abs(dy) == r) && cell && FIELD(cell, C_LANDTYPE, int) == 2) {
                    water = c;
                    break;
                }
            }
    if (!water.X || dir_fill_sea(water) < 400)   /* no water, or a pond */
        return 0;
    DynVec *bv = OIL_BUILDING_ARRAY;
    for (int i = 0; i < bv->Count; i++) {
        BYTE *b = bv->Items[i];
        if (oil_live(b) && dir_hostile(house, FIELD(b, O_OWNER, BYTE *)) && dir_near_sea(object_cell(b), 6))
            return 1;
    }
    return 0;
}

/* For an observer (RevealMap=1): the plan and posture changes of each AI in the game's message
 * list, in the house's colour. MessageListClass::AddMessage (0x5D3BA0, instance 0xA8BC60) takes
 * name, id, message, colour scheme, style, timeout and single-player, as at 0x4C6E9F. */
#define MESSAGE_LIST ((void *)0xA8BC60)
#define MESSAGE_ADD 0x5D3BA0
#define H_COLOR_SCHEME 0x16054
static int dir_announce_timeout = 900;
static void dir_announce(BYTE *house, const char *fmt, ...)
{
    static const char *names[][2] = { { "Americans", "America" }, { "Alliance", "Korea" }, { "French", "France" },
                                      { "Germans", "Germany" }, { "British", "Britain" }, { "Africans", "Libya" },
                                      { "Arabs", "Iraq" }, { "Confederation", "Cuba" }, { "Russians", "Russia" },
                                      { "YuriCountry", "Yuri" } };
    if (!reveal_map)
        return;
    const char *id = (char *)FIELD(house, H_TYPE, BYTE *) + T_ID, *name = id;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(id, names[i][0]))
            name = names[i][1];
    char text[160];
    int n = snprintf(text, sizeof text, "%s (%d): ", name, FIELD(house, 0x30, int));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text + n, sizeof text - n, fmt, ap);
    va_end(ap);
    wchar_t wide[160];
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 160);
    ((void *(GTHISCALL *)(void *, const wchar_t *, int, const wchar_t *, int, int, int, char))MESSAGE_ADD)(
        MESSAGE_LIST, NULL, 0, wide, FIELD(house, H_COLOR_SCHEME, int), 0x4046, dir_announce_timeout, 0);
}

/* For an observer: every 1500 frames a line per living AI with its money, economy and forces, timed
 * to last until the next one. */
#define REPORT_INTERVAL 1500
static void dir_observer_report(void)
{
    static int next_report = 600;
    if (!reveal_map || CURRENT_FRAME < next_report)
        return;
    next_report = CURRENT_FRAME + REPORT_INTERVAL;
    dir_house_strengths();
    dir_announce_timeout = REPORT_INTERVAL;
    DynVec *hv = HOUSE_ARRAY;
    int players = *GAME_PLAYERCOUNT + SESSION->Config.AIPlayers;
    for (int i = 0; i < hv->Count && i < players; i++) {
        BYTE *h = hv->Items[i];
        if (h[H_ISHUMAN] || h[OIL_H_DEFEATED] || !dir_house_alive(h))
            continue;
        int idx = FIELD(h, 0x30, int) & 31;
        DirState *d = &dir_state[idx];
        int strategic = d->house == h && d->plan_chosen;
        int units = FIELD(h, H_OWNED_UNITS, int) + FIELD(h, H_OWNED_INFANTRY, int) + FIELD(h, H_OWNED_AIRCRAFT, int);
        dir_announce(h, "$%d.%dk, %d ref %d harv, army $%d.%dk (%d), def $%d.%dk, %d bldg%s%s%s%s",
                     FIELD(h, OIL_H_CASH, int) / 1000, FIELD(h, OIL_H_CASH, int) % 1000 / 100,
                     FIELD(h, OIL_H_REFINERIES, int), FIELD(h, H_HARVESTERS, int), dir_house_army[idx] / 1000,
                     dir_house_army[idx] % 1000 / 100, units, dir_house_def[idx] / 1000, dir_house_def[idx] % 1000 / 100,
                     FIELD(h, H_OWNED_BUILDINGS, int), strategic ? ", " : "", strategic ? dir_plan_names[d->plan] : "",
                     strategic ? "/" : "", strategic ? dir_posture_names[d->posture] : "");
    }
    dir_announce_timeout = 900;
}

/* The next number of the house's random stream (xorshift). */
static unsigned dir_next_roll(DirState *d)
{
    unsigned x = (unsigned)d->plan_roll;
    for (int i = 0; i < 4; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    d->plan_roll = (int)(x ? x : 0x2545F491u);
    return x;
}

static void dir_choose_plan(BYTE *house, DirState *d)
{
    int idx = FIELD(house, 0x30, int) & 31;
    d->plan_chosen = 1;
    d->land_steps = dir_land_steps(house, d->base);
    int enemies = dir_hostile_houses(house);
    int tier = combat_building_count(house, "GATECH,NATECH,YATECH") ? 3
        : combat_building_count(house, "GAWEAP,NAWEAP,YAWEAP") ? 2 : FIELD(house, H_OWNED_BUILDINGS, int) ? 1 : 0;
    d->sea = dir_sea_reaches_enemy(house, d->base);
    int w[PLAN_COUNT];
    dir_plan_weights(d->land_steps, enemies, tier, d->sea, w);
    if (FIELD(house, OIL_H_SIDE, int) == 0)
        w[PLAN_NAVAL] /= 2;   /* an Allied fleet is Carriers only: dear, and slow to build up */
    /* the house's own stream, seeded from the match seed and the house index */
    d->plan_roll = *GAME_SEED ^ (int)(0x9E3779B9u * (unsigned)(idx + 1));
    int forced = director_plan_override(house);
    d->plan = forced >= 0 && forced < PLAN_COUNT ? forced : dir_plan_pick(w, dir_next_roll(d));
    d->opening = d->plan;
    d->plan_frame = CURRENT_FRAME;
    d->next_replan = CURRENT_FRAME + 9000;
    if (d->land_steps >= 0)
        dir_announce(house, "plan %s (enemy %d cells away on foot, %d enemies)", dir_plan_names[d->plan], d->land_steps,
                     enemies);
    else
        dir_announce(house, "plan %s (enemies across the water, %d enemies)", dir_plan_names[d->plan], enemies);
    logmsg("director: house %d plan %s%s (walk %d cells to the nearest enemy, %d enemies, tier %d, sea %d; "
           "weights %d/%d/%d/%d/%d)", idx, dir_plan_names[d->plan], forced >= 0 ? " (forced)" : "", d->land_steps,
           enemies, tier, d->sea, w[0], w[1], w[2], w[3], w[4]);
}

/* Plan again when a phased plan runs out, and every 9000 frames, from the state of the game. A
 * forced plan (DirectorPlan) stays. */
static void dir_replan(BYTE *house, DirState *d)
{
    /* a phased plan (rush, boom) runs its course first: cut short it pays the costs and never
     * collects (booms dropped at frame 9000 lost 5 of 5) */
    if (director_plan_override(house) >= 0 || (dir_plan_levers[d->plan].phase ? !dir_plan_over(d)
                                                                             : CURRENT_FRAME < d->next_replan))
        return;
    d->next_replan = CURRENT_FRAME + 9000;
    int refineries = FIELD(house, OIL_H_REFINERIES, int), theirs = 0;
    DynVec *hv = HOUSE_ARRAY;
    for (int i = 0; i < hv->Count; i++) {
        BYTE *h = hv->Items[i];
        if (dir_hostile(house, h) && dir_house_alive(h) && FIELD(h, OIL_H_REFINERIES, int) > theirs)
            theirs = FIELD(h, OIL_H_REFINERIES, int);
    }
    int w[PLAN_COUNT];
    dir_replan_weights(d->enemy_def, d->target_army, refineries, theirs, d->land_steps, d->sea, dir_hostile_houses(house), w);
    if (FIELD(house, OIL_H_SIDE, int) == 0)
        w[PLAN_NAVAL] /= 2;
    int plan = dir_plan_pick(w, dir_next_roll(d));
    logmsg("director: house %d frame %d re-plan: %s -> %s (target defences %d, army %d, refineries %d vs %d; "
           "weights %d/%d/%d/%d/%d)", FIELD(house, 0x30, int), CURRENT_FRAME, dir_plan_names[d->plan],
           dir_plan_names[plan], d->enemy_def, d->target_army, refineries, theirs, w[0], w[1], w[2], w[3], w[4]);
    if (plan != d->plan)
        dir_announce(house, "new plan %s", dir_plan_names[plan]);
    d->plan = plan;
    d->plan_frame = CURRENT_FRAME;
    d->plan_dropped = 0;
}

/* Every 450 frames: press an advantage, hold against a stronger neighbour, or strike a target whose
 * army is away. A posture lasts at least 900 frames, except that a threat to home ends any other. */
static void dir_update_posture(BYTE *house, DirState *d)
{
    if (CURRENT_FRAME < d->next_posture)
        return;
    d->next_posture = CURRENT_FRAME + 450;
    int opposition = d->target_army + d->third_party + d->enemy_def / 2;
    int next = dir_posture(d->posture, d->army_value, opposition, d->threat_near, d->target_army, d->target_home,
                           dir_hostile_houses(house));
    if (next == d->posture || (CURRENT_FRAME - d->posture_frame < 900 && next != POSTURE_HOLD))
        return;
    logmsg("director: house %d frame %d posture %s -> %s (army %d, opposition %d, near home %d, target home %d/%d)",
           FIELD(house, 0x30, int), CURRENT_FRAME, dir_posture_names[d->posture], dir_posture_names[next],
           d->army_value, opposition, d->threat_near, d->target_home, d->target_army);
    static const char *said[4] = { "back to normal", "pressing the attack", "holding at home",
                                   "striking while the enemy army is away" };
    dir_announce(house, "%s", said[next]);
    d->posture = next;
    d->posture_frame = CURRENT_FRAME;
}

/* Holding against a stronger neighbour: a ground defence at home every 2400 frames while the money
 * lasts (Prism Tower or Pillbox, Tesla Coil or Sentry Gun, Gatling Cannon). The engine's defence
 * placement puts it on the side facing the enemy. */
static void dir_hold_defenses(BYTE *house, DirState *d)
{
    static const char *defenses[3] = { "ATESLA,GAPILL", "TESLA,NALASR", "YAGGUN" };
    int side = FIELD(house, OIL_H_SIDE, int);
    if (d->posture != POSTURE_HOLD || side < 0 || side > 2 || CURRENT_FRAME < d->next_hold_defense
        || FIELD(house, OIL_H_PRODUCING, int) != -1)
        return;
    BYTE *type = dir_first_buildable(house, BUILDINGTYPE_ARRAY, defenses[side], 1500);
    if (!type)
        return;
    FIELD(house, OIL_H_PRODUCING, int) = building_type_index(type);
    d->next_hold_defense = CURRENT_FRAME + 2400;
    logmsg("director: house %d frame %d: holding, queued %.24s", FIELD(house, 0x30, int), CURRENT_FRAME,
           (char *)type + T_ID);
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
    if (bench_fleet && CURRENT_FRAME >= 300 && FIELD(house, 0x30, int) == 1) {
        dir_fleet_fixture(house, d);
        bench_fleet = 0;
    }
    if (!d->enemy || !dir_hostile(house, d->enemy) || !dir_house_alive(d->enemy)) {
        d->enemy = dir_pick_enemy(house, d->base);
        d->unreachable_count = 0;
    }
    /* Cut off by water, from the start or because a bridge fell (the engine re-zones the map then).
     * Re-checked often: a repaired bridge brings the ground war back. */
    if (CURRENT_FRAME >= d->next_island) {
        d->next_island = CURRENT_FRAME + 600;
        /* Engine zone labels proved unreliable as a connectivity test (one landmass reads several
         * labels), so being cut off is decided from evidence: an attack that could reach no objective
         * sets `blocked`. It is retried every 6000 frames, e.g. once engineers have mended a bridge. */
        /* Before any of that, a flood fill over land and bridge decks from our base: when no
         * building of the enemy stands on it, the enemy is across the water from the start, and
         * the army is planned for that at once instead of after five failed attacks. Another
         * enemy we can walk to is fought first. */
        int island = d->enemy && (bench_force_island || !dir_land_house(d));
        if (island && !bench_force_island) {
            dir_house_strengths();
            BYTE *alt = NULL;
            int best = 0x7FFFFFFF;
            DynVec *hv = HOUSE_ARRAY;
            for (int i = 0; i < hv->Count; i++) {
                BYTE *h = hv->Items[i];
                int score;
                if (h != d->enemy && dir_hostile(house, h) && dir_house_alive(h) && dir_house_on_land(h)
                    && (score = dir_enemy_rating(h, d->base)) < best) {
                    best = score;
                    alt = h;
                }
            }
            if (alt) {
                logmsg("director: house %d frame %d: house %d is across the water, fighting house %d by land instead",
                       FIELD(house, 0x30, int), CURRENT_FRAME, FIELD(d->enemy, 0x30, int), FIELD(alt, 0x30, int));
                d->enemy = alt;
                d->unreachable_count = 0;
                d->objective = NULL;
                island = 0;
            }
        }
        if (d->blocked && CURRENT_FRAME - d->blocked_frame > 6000) {
            d->blocked = 0;
            logmsg("director: house %d frame %d: retrying the ground route", FIELD(house, 0x30, int), CURRENT_FRAME);
        }
        if (island != d->island)
            logmsg("director: house %d frame %d: enemy base %s", FIELD(house, 0x30, int), CURRENT_FRAME,
                   island ? "cut off by water: ferrying troops, building hover/air units" : "reachable by land again");
        d->island = island;
    }
    if (bench_file && CURRENT_FRAME % 1500 < 15) {   /* what the queues hold, and whether it can be built */
        int bi = FIELD(house, OIL_H_PRODUCING, int), ui = FIELD(house, H_PRODUCING_UNIT, int);
        DynVec *bt = BUILDINGTYPE_ARRAY, *ut = UNITTYPE_ARRAY;
        BYTE *b = bi >= 0 && bi < bt->Count ? bt->Items[bi] : NULL, *u = ui >= 0 && ui < ut->Count ? ut->Items[ui] : NULL;
        logmsg("director: house %d frame %d queues: building %s (can %d) unit %s (can %d) cash %d",
               FIELD(house, 0x30, int), CURRENT_FRAME, b ? (char *)b + T_ID : "-",
               b ? ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, b, 0, 1) : 0,
               u ? (char *)u + T_ID : "-", u ? ((int (GTHISCALL *)(BYTE *, BYTE *, char, char))OIL_H_CAN_BUILD)(house, u, 0, 1) : 0,
               FIELD(house, OIL_H_CASH, int));
    }
    static int huts_noted;
    if (CURRENT_FRAME >= 300 && !huts_noted) {
        huts_noted = 1;
        dir_note_closed_bridges();
        DynVec *bv = OIL_BUILDING_ARRAY;   /* benchmark: which tech buildings stand behind fences */
        for (int i = 0; bench_file && i < bv->Count; i++) {
            BYTE *b = bv->Items[i];
            CellXY gap = { 0, 0 }, at = object_cell(b);
            int fenced = oil_live(b) && in_list("CAOILD,CAAIRP,CATHOSP,CAOUTP,CAMACH,CAPOWR", (char *)dir_type(b) + T_ID)
                ? dir_fenced(b, d->base, &gap) : 0;
            if (fenced == 2) {   /* closed by cliffs nearby: sealed only if no land route at all */
                dir_fill_land(d->base);
                fenced = dir_land_reachable(at) ? 0 : 2;
            }
            if (fenced)
                logmsg("director: %.24s at %d,%d is %s (gap toward house %d at %d,%d)", (char *)dir_type(b) + T_ID,
                       at.X, at.Y, fenced == 1 ? "fenced in" : "sealed in", FIELD(house, 0x30, int), gap.X, gap.Y);
        }
    }
    if (bench_file && CURRENT_FRAME < 1000 && !d->last_regroup_log) {   /* cell offsets sanity check */
        d->last_regroup_log = 1;
        BYTE *cell = dir_cell(d->base);
        if (cell)
            logmsg("director: house %d cell %d,%d land %d occupation %#x", FIELD(house, 0x30, int), d->base.X,
                   d->base.Y, FIELD(cell, C_LANDTYPE, int), FIELD(cell, C_OCCUPATION, DWORD));
    }
    dir_scan_enemies(house, d);
    if (director_enabled(house) & DIR_F_STRATEGY) {
        if (!d->plan_chosen)
            dir_choose_plan(house, d);
        dir_replan(house, d);
        dir_update_posture(house, d);
        dir_hold_defenses(house, d);
    }
    /* Free-for-all: between attacks, turn on whoever has become the nearest weak enemy. */
    if (d->state == DIR_GATHER && CURRENT_FRAME >= d->next_repick) {
        d->next_repick = CURRENT_FRAME + 3000;
        BYTE *best = dir_pick_enemy(house, d->base);
        if (best && best != d->enemy && (!d->enemy || (long long)dir_enemy_rating(best, d->base) * 4
                                                      < (long long)dir_enemy_rating(d->enemy, d->base) * 3)) {
            logmsg("director: house %d frame %d: new target house %d (was %d)", FIELD(house, 0x30, int), CURRENT_FRAME,
                   FIELD(best, 0x30, int), d->enemy ? FIELD(d->enemy, 0x30, int) : -1);
            d->enemy = best;
            d->unreachable_count = 0;
            dir_scan_enemies(house, d);
        }
    }
    /* after the scan: the rally keeps clear of this house's buildings, which the scan collects */
    if (d->enemy != d->rally_enemy || CURRENT_FRAME >= d->rally_frame) {
        d->rally_enemy = d->enemy;
        d->rally_frame = CURRENT_FRAME + 3000;
        d->rally = dir_pick_rally(d);
        d->home_zone = dir_zone(d->rally);   /* the rally is clear land, unlike a built-over base centre */
    }
    if (bench_file && human_in_peace && CURRENT_FRAME % 600 < 15)
        dir_report_intruders(house);
    if (director_enabled(house) & DIR_F_PRODUCTION) {
        dir_veto_production(house, d);
        dir_air_defense(house, d);
    }
    if (director_enabled(house) & DIR_F_EXPANSION)
        dir_colonize(house, d);
    if (director_enabled(house) & DIR_F_NAVY)
        dir_navy(house, d);
    dir_army(house, d);
    /* every engineer of ours, stock team members included: none walks into the hut of a bridge
     * that is whole, or of one that can't be mended (barrier-gated bridges) */
    if (CURRENT_FRAME % 90 < 15) {
        DynVec *tv = OIL_TECHNO_ARRAY;
        for (int i = 0; i < tv->Count; i++) {
            BYTE *o = tv->Items[i];
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || dir_whatami(o) != 15 || !dir_is_engineer(o))
                continue;
            BYTE *goal = FIELD(o, COMBAT_DESTINATION, BYTE *);
            if (!goal || is_cell(goal))
                goal = FIELD(o, O_TARGET, BYTE *);
            BYTE *gt = goal && !is_cell(goal) && dir_object_listed(OIL_BUILDING_ARRAY, goal) ? FIELD(goal, B_TYPE, BYTE *) : NULL;
            if (!gt || !gt[BT_BRIDGE_HUT] || (dir_bridge_down(goal) && !dir_hut_hopeless(goal)))
                continue;
            CellXY c = object_cell(goal);
            logmsg("director: house %d frame %d: engineer called off the hut at %d,%d (bridge %s)", FIELD(house, 0x30, int),
                   CURRENT_FRAME, c.X, c.Y, dir_bridge_down(goal) ? "can't be mended" : "is whole");
            dir_why = "engineer home";
            dir_command(o, d->base, NULL, 0);
            if (o == d->repair_engineer)
                d->repair_hut = d->repair_engineer = NULL;
        }
    }
    /* every unit of ours, team members included: standing on a bridge deck never helps anyone */
    if (CURRENT_FRAME % 150 < 15) {
        DynVec *tv = OIL_TECHNO_ARRAY;
        for (int i = 0; i < tv->Count; i++) {
            BYTE *o = tv->Items[i];
            int what, m;
            if (!oil_live(o) || FIELD(o, O_OWNER, BYTE *) != house || ((what = dir_whatami(o)) != 1 && what != 15)
                || dir_naval(o) || dir_crosses(o) || (m = FIELD(o, COMBAT_MISSION, int)) == 7 || m == 8 || m == 10 || m == 16)
                continue;
            dir_bridge_evac(o, object_cell(o));
        }
    }
    if (director_enabled(house) & DIR_F_BUNKERS) {
        dir_bunkers(house, d);
        dir_garrison(house, d);
        dir_posts(house, d);
        if (CURRENT_FRAME % 300 < 15)
            dir_man_fortresses(house, d);
    }
    if (CURRENT_FRAME % 150 < 15 && (director_enabled(house) & DIR_F_ENGINEERS))
        dir_engineers(house, d);
    dir_economy(house, d);
    if (director_enabled(house) & DIR_F_ECONOMY) {
        dir_slave_miners(house, d);
        dir_yard_escape(house, d);
    }
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

static void bench_kills_install(void);

static void patch_director(void)
{
    const BYTE unit[] = { 0x81, 0xEC, 0xDC, 0x04, 0x00, 0x00 };
    if (!patch_checked("director unit production", DIR_UNIT_PRODUCTION, unit, sizeof unit)
        || !patch_checked("director infantry production", DIR_INF_PRODUCTION, unit, sizeof unit))
        return;
    BYTE *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
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
    const BYTE hunt_call[5] = { 0xE8, 0x07, 0x53, 0xDD, 0xFF };   /* call 0x50B730 at 0x736424 */
    if (patch_checked("director escape hunt", 0x736424, hunt_call, sizeof hunt_call))
        patch_rel(0x736424, 0xE8, (DWORD)dir_mcv_left_alone);
    const BYTE deploy_entry[] = { 0x83, 0xEC, 0x18, 0x53, 0x55 };
    if (patch_checked("director escape deploy", UNIT_TRY_DEPLOY, deploy_entry, sizeof deploy_entry)) {
        memcpy(tramp + 32, deploy_entry, sizeof deploy_entry);
        patch_rel((DWORD)tramp + 32 + sizeof deploy_entry, 0xE9, UNIT_TRY_DEPLOY + sizeof deploy_entry);
        dir_try_deploy_original = (char (GTHISCALL *)(BYTE *))(tramp + 32);
        patch_rel(UNIT_TRY_DEPLOY, 0xE9, (DWORD)dir_try_deploy);
    }
    /* every "may the owner undeploy" call in Mission_Selling (0x449C30) */
    static const DWORD undeploy_calls[] = { 0x449D29, 0x44A554, 0x44A802, 0x44A912, 0x44A99D };
    for (unsigned i = 0; i < sizeof undeploy_calls / sizeof undeploy_calls[0]; i++) {
        DWORD at = undeploy_calls[i], rel = 0x50B730 - (at + 5);
        const BYTE call[5] = { 0xE8, (BYTE)rel, (BYTE)(rel >> 8), (BYTE)(rel >> 16), (BYTE)(rel >> 24) };
        if (patch_checked("director yard escape", at, call, sizeof call))
            patch_rel(at, 0xE8, (DWORD)dir_may_undeploy);
    }
    {
        DWORD rel = DIR_NODE_CELL_OK - (DIR_NODE_CELL_CALL + 5);
        const BYTE call[5] = { 0xE8, (BYTE)rel, (BYTE)(rel >> 8), (BYTE)(rel >> 16), (BYTE)(rel >> 24) };
        if (patch_checked("director base plan spots", DIR_NODE_CELL_CALL, call, sizeof call))
            patch_rel(DIR_NODE_CELL_CALL, 0xE8, (DWORD)dir_node_cell_ok);
    }
    /* The base planner (0x5054B0) lists building types to place, with negative codes for other
     * entries, and tests "a type" as >= 0 (cmp edi,0 / jge at 0x505DA8): a null entry (a role the
     * house had no type for after its yard fled and set up again) was read at +0xDF8 and crashed
     * the game. Null entries are skipped: test edi,edi / jz next / jg type / jmp code. */
    {
        const BYTE stock[] = { 0x3B, 0xF8, 0x0F, 0x8D, 0x83, 0x00, 0x00, 0x00 };   /* cmp edi,eax / jge 0x505E33 */
        BYTE *cave = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (cave && patch_checked("director base planner null types", 0x505DA8, stock, sizeof stock)) {
            BYTE *c = cave;
            DWORD rel;
            *c++ = 0x85; *c++ = 0xFF;                                   /* test edi,edi */
            *c++ = 0x0F; *c++ = 0x84; rel = 0x505EAD - (DWORD)(c + 4); memcpy(c, &rel, 4); c += 4;   /* jz next entry */
            *c++ = 0x0F; *c++ = 0x8F; rel = 0x505E33 - (DWORD)(c + 4); memcpy(c, &rel, 4); c += 4;   /* jg a type */
            *c++ = 0xE9; rel = 0x505DB0 - (DWORD)(c + 4); memcpy(c, &rel, 4); c += 4;               /* the codes */
            FlushInstructionCache(GetCurrentProcess(), cave, 32);
            const BYTE nops[3] = { 0x90, 0x90, 0x90 };
            patch_rel(0x505DA8, 0xE9, (DWORD)cave);
            patch(0x505DAD, nops, 3);
        }
    }
    const BYTE pick_call[] = { 0xE8, 0x4E, 0xBA, 0x0A, 0x00 };   /* call 0x4FBD80 */
    if (patch_checked("director naval yard orders", DIR_FACTORY_PICK_CALL, pick_call, sizeof pick_call))
        patch_rel(DIR_FACTORY_PICK_CALL, 0xE8, (DWORD)dir_factory_pick);
    logmsg("director: production patches applied (Brutal skirmish only)");
    bench_kills_install();
}

/* Benchmark CSV tail: war factories, current vehicle and infantry orders, director state and army. */
static void bench_row_extra(BYTE *h)
{
    int unit = FIELD(h, H_PRODUCING_UNIT, int), inf = FIELD(h, H_PRODUCING_INF, int), idx = FIELD(h, 0x30, int) & 31;
    DynVec *ut = UNITTYPE_ARRAY, *it = INFANTRYTYPE_ARRAY;
    DirState *d = &dir_state[idx];
    int building = FIELD(h, OIL_H_PRODUCING, int);
    DynVec *bt = BUILDINGTYPE_ARRAY;
    int strategic = d->house == h && d->plan_chosen;
    fprintf(bench_file, ",%d,%.24s,%.24s,%.24s,%d,%d,%d,%s,%s,%s\n", combat_building_count(h, "GAWEAP,NAWEAP,YAWEAP"),
            building >= 0 && building < bt->Count ? (char *)bt->Items[building] + T_ID : "-",
            unit >= 0 && unit < ut->Count ? (char *)ut->Items[unit] + T_ID : "-",
            inf >= 0 && inf < it->Count ? (char *)it->Items[inf] + T_ID : "-",
            d->house == h ? d->state : -1, d->house == h ? d->army_value : 0, director_house[idx],
            strategic ? dir_plan_names[d->opening] : "-", strategic ? dir_posture_names[d->posture] : "-",
            strategic ? dir_plan_names[d->plan] : "-");
}

/* Benchmark camera: centre the view on a house's army front (TacticalClass::SetTacticalPosition). */
static void bench_camera_update(void)
{
    CellXY c = { (short)bench_camera_x, (short)bench_camera_y };
    if (!bench_camera_x) {
        int h = bench_camera >= 100 ? bench_camera - 100 : bench_camera;   /* 100+N: house N's base */
        if (h < 0 || h >= 32 || !dir_state[h].house)
            return;
        c = dir_state[h].army_count && bench_camera < 100 ? dir_state[h].front : dir_state[h].base;
    }
    Coord at = { c.X * 256 + 128, c.Y * 256 + 128, 0 };
    at.Z = ((int (GTHISCALL *)(void *, Coord *))MAP_FLOOR_HEIGHT)(MAP_INSTANCE, &at);
    ((void (GTHISCALL *)(void *, Coord *))0x6D6070)(*(void **)0x887324, &at);
}

/* ---- benchmark kill statistics ----
 * Every destroyed object reports its destroyer through ObjectClass::RegisterDestruction (vtable 0xE0;
 * TechnoClass 0x702D40, UnitClass 0x744720). In benchmark matches a wrapper credits the victim's cost
 * to the killer's type, and the loss to the victim's type; yspawn-kills.csv gets the totals. */
#define VT_REGISTER_DESTRUCTION 0xE0
static struct { BYTE *type; int kills, killed_value, deaths, lost_value; } kill_stats[512];
static void *kill_original[4];

static int kill_slot(BYTE *type)
{
    for (int i = 0; i < 512; i++) {
        if (kill_stats[i].type == type)
            return i;
        if (!kill_stats[i].type) {
            kill_stats[i].type = type;
            return i;
        }
    }
    return -1;
}

static void kill_note(BYTE *victim, BYTE *destroyer)
{
    BYTE *vt = victim && (FIELD(victim, 0x14, DWORD) & 1) ? dir_type(victim) : NULL;
    if (!vt)
        return;
    int cost = dir_cost(vt), v = kill_slot(vt);
    if (v >= 0) {
        kill_stats[v].deaths++;
        kill_stats[v].lost_value += cost;
    }
    BYTE *kt = destroyer && (FIELD(destroyer, 0x14, DWORD) & 1) ? dir_type(destroyer) : NULL;
    BYTE *owner = destroyer ? FIELD(destroyer, O_OWNER, BYTE *) : NULL;
    if (!kt || !owner || owner == FIELD(victim, O_OWNER, BYTE *))
        return;
    int k = kill_slot(kt);
    if (k >= 0) {
        kill_stats[k].kills++;
        kill_stats[k].killed_value += cost;
    }
}

#define KILL_WRAP(n) \
static void GTHISCALL kill_wrap_##n(BYTE *self, BYTE *destroyer) \
{ \
    kill_note(self, destroyer); \
    ((void (GTHISCALL *)(BYTE *, BYTE *))kill_original[n])(self, destroyer); \
}
KILL_WRAP(0)
KILL_WRAP(1)
KILL_WRAP(2)
KILL_WRAP(3)
#undef KILL_WRAP

static void bench_kills_install(void)
{
    if (!ini_int("Settings", "Benchmark", 0))
        return;
    static const DWORD vtables[4] = { 0x7E22A4, 0x7E3EBC, 0x7EB058, 0x7F5C70 };   /* aircraft, building, infantry, unit */
    static const DWORD expected[4] = { 0x702D40, 0x702D40, 0x702D40, 0x744720 };
    void *wraps[4] = { kill_wrap_0, kill_wrap_1, kill_wrap_2, kill_wrap_3 };
    for (int i = 0; i < 4; i++)
        if (!patch_checked("benchmark kill stats", vtables[i] + VT_REGISTER_DESTRUCTION, (BYTE *)&expected[i], 4))
            return;
    for (int i = 0; i < 4; i++) {
        kill_original[i] = (void *)expected[i];
        patch(vtables[i] + VT_REGISTER_DESTRUCTION, (BYTE *)&wraps[i], 4);
    }
    logmsg("benchmark: kill statistics on");
}

static void bench_kills_dump(void)
{
    FILE *f = fopen("yspawn-kills.csv", "w");
    if (!f)
        return;
    fputs("type,cost,kills,killed_value,deaths,lost_value\n", f);
    for (int i = 0; i < 512 && kill_stats[i].type; i++)
        fprintf(f, "%.24s,%d,%d,%d,%d,%d\n", (char *)kill_stats[i].type + T_ID, dir_cost(kill_stats[i].type),
                kill_stats[i].kills, kill_stats[i].killed_value, kill_stats[i].deaths, kill_stats[i].lost_value);
    fclose(f);
}
