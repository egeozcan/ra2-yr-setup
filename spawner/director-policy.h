#ifndef DIRECTOR_POLICY_H
#define DIRECTOR_POLICY_H

/* Pure decisions for the Brutal strategy director (director.h); unit-tested in test_director.py. */

enum { DIR_GATHER, DIR_ATTACK, DIR_DEFEND, DIR_RETREAT };
/* [AIn] DirectorFlags bits, for ablation runs; all are on by default. */
enum { DIR_F_PRODUCTION = 1, DIR_F_ARMY = 2, DIR_F_TAKEOVER = 4, DIR_F_FOCUS = 8, DIR_F_ECONOMY = 16,
       DIR_F_ANSWER = 32, DIR_F_DEFENSES_FIRST = 64, DIR_F_COHESION = 128, DIR_F_ENGINEERS = 256,
       DIR_F_EXPANSION = 512, DIR_F_REGROUP = 1024, DIR_F_NAVY = 2048, DIR_F_UNSTICK = 4096,
       DIR_F_BUNKERS = 8192, DIR_F_STRATEGY = 16384, DIR_F_ALL = 32767,
       /* defenses-first objectives lost 3 of 4 director-vs-director ablation matches: off */
       DIR_F_DEFAULT = DIR_F_ALL & ~DIR_F_DEFENSES_FIRST };
enum { ROLE_MAIN, ROLE_AA, ROLE_SIEGE, ROLE_SUPPORT, ROLE_COUNT };

/* Launch once the army can beat what the enemy fields (its mobile army plus half its static
 * defenses), with a floor that rises over time. A maxed-out army goes regardless, and the longer
 * the army has waited the smaller the edge it asks for: a stalemate is a loss in slow motion. */
static int dir_should_launch_plan(int army_value, int army_count, int enemy_army_value, int enemy_defense, int frame,
                                  int waited, int floor_pct, int edge, int min_units);
__attribute__((unused)) static int dir_should_launch(int army_value, int army_count, int enemy_army_value,
                                                     int enemy_defense, int frame, int waited)
{
    return dir_should_launch_plan(army_value, army_count, enemy_army_value, enemy_defense, frame, waited, 100, 12, 6);
}

/* Which enemy to fight: near and weak beats far and weak or near and strong. strength is the
 * house's mobile army plus half its defenses; each cell of distance counts as 150 credits. */
static int dir_enemy_score(int distance_cells, int strength)
{
    return strength + 150 * distance_cells;
}

/* A forward section meeting overwhelming force falls back to the main body, when the body is
 * well behind it and the army as a whole could win that fight together. */
static int dir_should_regroup(int local_ours, int local_enemy, int cells_ahead, int group_value)
{
    return cells_ahead >= 8 && local_enemy > 1500 && local_enemy > local_ours * 2
        && group_value * 10 > local_enemy * 12;
}

/* Fall back when the fight at the front is being lost after real losses, or when the attack
 * has been bled out and still meets resistance. Clean-up against no resistance continues. */
static int dir_should_retreat(int army_value, int launch_value, int local_ours, int local_enemy)
{
    if (local_enemy == 0)
        return 0;
    if (army_value * 100 < launch_value * 30)
        return 1;
    return army_value * 100 < launch_value * 70 && local_enemy * 10 > local_ours * 13;
}

/* Base defense: a real raid, not a lone scout. */
static int dir_base_threat(int threat_value, int army_value, int state)
{
    /* hysteresis: a defense in progress continues until the raid is mostly gone */
    if (threat_value < (state == DIR_DEFEND ? 800 : 1500))
        return 0;
    /* While attacking, only come home for raids that matter relative to the army. */
    return state != DIR_ATTACK || threat_value * 2 > army_value;
}

/* Desired share (percent) of each role in the army, from the enemy's composition.
 * enemy_* are value totals of enemy air units, infantry, vehicles and armed buildings. */
static void dir_role_shares(int enemy_air, int enemy_infantry, int enemy_armor, int enemy_defense,
                            int shares[ROLE_COUNT])
{
    int total = enemy_air + enemy_infantry + enemy_armor + 1;
    int aa = 10 + 40 * enemy_air / total;
    int support = 10 + 30 * enemy_infantry / total;
    int siege = enemy_defense > 6000 ? 25 : enemy_defense > 2500 ? 18 : 10;
    shares[ROLE_AA] = aa;
    shares[ROLE_SUPPORT] = support;
    shares[ROLE_SIEGE] = siege;
    shares[ROLE_MAIN] = 100 - aa - support - siege;
    if (shares[ROLE_MAIN] < 25)
        shares[ROLE_MAIN] = 25;
}

/* The role furthest below its share of the current army value. */
static int dir_pick_role(const int shares[ROLE_COUNT], const int have[ROLE_COUNT], const int available[ROLE_COUNT])
{
    int total = 0, best = -1, best_gap = -1000000;
    for (int r = 0; r < ROLE_COUNT; r++)
        total += have[r];
    for (int r = 0; r < ROLE_COUNT; r++) {
        if (!available[r])
            continue;
        int gap = shares[r] * (total + 1000) - have[r] * 100;
        if (gap > best_gap) {
            best_gap = gap;
            best = r;
        }
    }
    return best;
}

/* War factories to aim for: production, not money, limits Brutal armies. */
static int dir_wanted_factories(int cash, int frame, int refineries)
{
    if (frame < 1800 || refineries < 1)
        return 1;
    if (cash >= 25000 && refineries >= 2)
        return 4;
    if (cash >= 12000)
        return 3;
    return cash >= 5000 ? 2 : 1;
}

/* More refineries once money runs short: each brings a harvester and a shorter ore run. Not while
 * harvesters stand idle: their ore is exhausted or cut off, and another refinery would not help. */
static int dir_want_refinery_plan(int frame, int refineries, int harvesters, int idle_harvesters, int cash, int bonus,
                                  int early);
__attribute__((unused)) static int dir_want_refinery(int frame, int refineries, int harvesters, int idle_harvesters,
                                                     int cash)
{
    return dir_want_refinery_plan(frame, refineries, harvesters, idle_harvesters, cash, 0, 0);
}

/* Keep producing while money lasts; keep a reserve for buildings. */
static int dir_can_spend(int cash, int cost, int reserve)
{
    return cash - cost >= reserve;
}

/* ---- strategy: a plan per house, chosen at the start from the setup, and a posture that follows
 * the game ----
 * A plan is a set of numbers for the levers the director already has: when the army launches,
 * the unit mix, refineries, factories, expansion timing and what an attack goes for first. A rush
 * or boom lasts a phase; then, and every 9000 frames, the house plans again from the state of the
 * game (dir_replan_weights). */
enum { PLAN_BALANCED, PLAN_RUSH, PLAN_BOOM, PLAN_SIEGE, PLAN_NAVAL, PLAN_COUNT };
static const char *const dir_plan_names[PLAN_COUNT] = { "balanced", "rush", "boom", "siege", "naval" };

typedef struct {
    int floor_pct;        /* scales the launch floor (100: as is) */
    int edge;             /* edge over the opposition the launch asks for, tenths (12: 1.2x) */
    int min_units;        /* fewest units in a launch */
    int share_siege, share_support, share_aa;   /* added to the enemy-derived role shares (percent) */
    int refinery_bonus;   /* refineries wanted beyond the stock schedule */
    int refinery_early;   /* frames the refinery schedule is brought forward */
    int factory_cash_pct; /* scales the cash thresholds for extra war factories (lower: sooner) */
    int expand_frame;     /* earliest expansion */
    int expand_idle;      /* expansion waits for idle harvesters */
    int eco_targets;      /* attacks go for refineries, factories and yards first */
    int phase;            /* frames the plan lasts before the house plans again (0: until re-planned) */
    int navy_share;       /* warships kept at this percent of the army's value (0: only to answer ships) */
    int fleet_launch;     /* fleet value before it sails on the enemy (as before: 3000) */
} DirPlanLevers;

static const DirPlanLevers dir_plan_levers[PLAN_COUNT] = {
    /*            floor edge min  siege sup aa  ref early fact expand idle eco  phase navy fleet */
    [PLAN_BALANCED] = { 100, 12, 6,   0,  0,  0,  0,    0, 100, 9000, 1, 0,     0,   0, 3000 },
    /* rush: a smaller army goes early for the enemy's production and income, from more factories
     * and no economy beyond the opening one; lasts 14000 frames or until its first attack ends.
     * The edge stays 1.2: at parity the defender won (3 of 12 rushes won the MCV duels) */
    [PLAN_RUSH]     = {  45, 12, 5,  -5,  5,  0,  0,    0,  50, 20000, 1, 1, 14000,  0, 3000 },
    /* boom: refineries early, an expansion as soon as the yard can pay for one, a late and heavy
     * first strike, and factories to spend the money afterwards; 20000 frames */
    [PLAN_BOOM]     = { 160, 13, 8,   0,  0,  0,  1, 3000, 120, 4500, 0, 0, 20000,  0, 3000 },
    /* siege: more siege in the mix (V3s, Prism tanks, Magnetrons) against static defences */
    [PLAN_SIEGE]    = { 110, 12, 6,  15, -5,  0,  0,    0, 100, 9000, 1, 0,     0,  0, 3000 },
    /* naval: our sea reaches enemy buildings: a war fleet worth 40% of the army (Carriers and
     * Destroyers, Dreadnoughts, Boomers) that sails once it is worth 8000; the army as balanced */
    [PLAN_NAVAL]    = { 100, 12, 6,   0,  0,  0,  0,    0, 100, 9000, 1, 0,     0, 40, 8000 },
};

/* How much each plan fits the setup, in weights for a random draw. land_steps is the walking
 * distance to the nearest enemy base (-1: none reachable on foot), enemies the hostile players,
 * tier the starting base (0: MCV). */
static void dir_plan_weights(int land_steps, int enemies, int tier, int sea, int w[PLAN_COUNT])
{
    w[PLAN_BALANCED] = 30;
    /* rush won 11 of 26 in the A/B runs (balanced 25 of 43, siege 10 of 15): a gamble, kept rare */
    w[PLAN_RUSH] = land_steps < 0 ? 0 : land_steps <= 70 ? 15 : land_steps <= 110 ? 8 : 3;
    if (enemies >= 3)
        w[PLAN_RUSH] /= 3;   /* in a free-for-all a rush leaves the base to everyone else */
    /* from a built base both armies grow alike and parity never comes: 4 of 5 rushes lost in the
     * tier-3 A/B never launched at all */
    if (tier >= 2)
        w[PLAN_RUSH] /= 3;
    /* a late first strike loses duels to an early attack (0 of 5 from MCV starts at 74 cells, 0 of
     * 3 again later): boom is for long walks and crowded maps, never a duel within reach */
    w[PLAN_BOOM] = (enemies >= 2 ? 5 : 0) + (land_steps < 0 || land_steps > 110 ? 20 : 0) + (enemies >= 3 ? 15 : 0);
    /* nobody has defences at the start: siege as an opening is a hunch the re-plan (40 against a
     * real fortress) does better; as an opening it placed 0.55 from built bases (balanced 0.38) */
    w[PLAN_SIEGE] = 10;
    /* sea: our water reaches some enemy's buildings; across water a fleet is the way over */
    w[PLAN_NAVAL] = !sea ? 0 : land_steps < 0 ? 35 : 10;
}

/* Later plans, from the state of the game: when an opening plan runs out, and every 9000 frames.
 * def is the target's static defence, army its mobile army; refineries ours and the strongest
 * enemy's. A rush is an opening only. */
static void dir_replan_weights(int def, int army, int refineries, int their_refineries, int land_steps, int sea,
                               int enemies, int w[PLAN_COUNT])
{
    w[PLAN_BALANCED] = 30;
    w[PLAN_RUSH] = 0;
    /* in a duel within reach, booming mid-game handed the enemy the initiative: 0 of 7 won */
    w[PLAN_BOOM] = enemies < 2 && land_steps >= 0 ? 0 : refineries < their_refineries ? 30 : 5;
    w[PLAN_SIEGE] = def > 4000 && def * 2 > army ? 40 : 10;
    w[PLAN_NAVAL] = !sea ? 0 : land_steps < 0 ? 35 : 10;
}

/* A weighted draw: roll is any random number. */
static int dir_plan_pick(const int w[PLAN_COUNT], unsigned roll)
{
    int total = 0;
    for (int i = 0; i < PLAN_COUNT; i++)
        total += w[i] > 0 ? w[i] : 0;
    if (total <= 0)
        return PLAN_BALANCED;
    int r = (int)(roll % (unsigned)total);
    for (int i = 0; i < PLAN_COUNT; i++) {
        if (w[i] <= 0)
            continue;
        if (r < w[i])
            return i;
        r -= w[i];
    }
    return PLAN_BALANCED;
}

/* Posture: how the plan bends to the state of the game.
 *  PRESS:   clearly ahead of the target: launch at a smaller edge, keep pushing.
 *  HOLD:    a neighbour's army near home outclasses ours: no launch, defences, army home.
 *  OPPORTUNITY: in a free-for-all, the target's army is away from its base and not near ours
 *           (fighting someone else): strike the base against what is there, not its whole army. */
enum { POSTURE_NORMAL, POSTURE_PRESS, POSTURE_HOLD, POSTURE_OPPORTUNITY };
static const char *const dir_posture_names[4] = { "normal", "press", "hold", "opportunity" };

static int dir_posture(int prev, int army, int opposition, int threat_near, int target_army, int target_home,
                       int enemies)
{
    /* hold first: a stronger army within reach of home matters more than the target far away */
    if (threat_near > 4000 && (long long)threat_near * 10 > (long long)army * (prev == POSTURE_HOLD ? 11 : 15))
        return POSTURE_HOLD;
    if (opposition > 0 && (long long)army * 10 >= (long long)opposition * (prev == POSTURE_PRESS ? 13 : 16))
        return POSTURE_PRESS;
    /* only with a third player about: in a duel an army away from home is coming for us */
    if (enemies >= 2 && target_army >= 3000 && threat_near * 4 < target_army
        && target_home * 100 < target_army * (prev == POSTURE_OPPORTUNITY ? 50 : 35))
        return POSTURE_OPPORTUNITY;
    return POSTURE_NORMAL;
}

/* Launch with plan levers: floor scaled by floor_pct, edge in tenths. The waiting discount of
 * dir_should_launch still applies, never below 0.8. */
static int dir_should_launch_plan(int army_value, int army_count, int enemy_army_value, int enemy_defense, int frame,
                                  int waited, int floor_pct, int edge, int min_units)
{
    if (army_count < min_units)
        return 0;
    int floor = (frame < 12000 ? 5000 : frame < 24000 ? 8000 : 12000) * floor_pct / 100;
    int e = edge - (waited >= 12000 ? 4 : waited >= 6000 ? 2 : 0);
    /* A huge army breaks a stalemate at a lower edge: parity from 100000, 0.8 from 150000. It used
     * to launch at 60000 whatever it faced: the faster-growing side got there first and threw itself
     * at an equal army in that army's base, which the attacker loses (in the MCV A/B, 8 of 9
     * strategy houses whose first attack went so lost the duel, at 0.94-1.14 of the defender). */
    if (army_value >= 150000 && e > 8)
        e = 8;
    else if (army_value >= 100000 && e > 10)
        e = 10;
    if (e < 8)
        e = 8;
    long long opposition = enemy_army_value + enemy_defense / 2;
    /* late on, a broken enemy is finished off by what we have: the 12000 floor kept 8600 at home
     * against 2600 until the time limit. Twice the opposition will do, from 4000 up. */
    if (frame >= 24000 && opposition * 2 < floor)
        floor = opposition * 2 > 4000 ? (int)(opposition * 2) : 4000;
    return army_value >= floor && (long long)army_value * 10 >= opposition * e;
}

/* Role shares bent by the plan, re-normalised: the main role takes up the difference, never
 * below a quarter. */
static void dir_plan_shares(const DirPlanLevers *p, int shares[ROLE_COUNT])
{
    shares[ROLE_SIEGE] += p->share_siege;
    shares[ROLE_SUPPORT] += p->share_support;
    shares[ROLE_AA] += p->share_aa;
    for (int r = ROLE_AA; r < ROLE_COUNT; r++)
        if (shares[r] < 5)
            shares[r] = 5;
    shares[ROLE_MAIN] = 100 - shares[ROLE_AA] - shares[ROLE_SUPPORT] - shares[ROLE_SIEGE];
    if (shares[ROLE_MAIN] < 25)
        shares[ROLE_MAIN] = 25;
}

/* The refinery schedule with plan levers. */
static int dir_want_refinery_plan(int frame, int refineries, int harvesters, int idle_harvesters, int cash, int bonus,
                                  int early)
{
    int f = frame + early;
    int wanted = f < 6000 ? 0 : f < 15000 ? 3 : f < 30000 ? 4 : 5;
    if (wanted)
        wanted += bonus;
    /* "money is short" (under 12000) except with a single refinery or in a boom: a rich start
     * (Isolation hands out 50000) otherwise spent it all on units and stayed on one refinery */
    /* idle harvesters mean the ore is gone, except with no refinery left: then they idle for want
     * of one (a house whose only refinery fell never built another, and went broke) */
    if (!refineries && frame >= 3000)
        return 1;
    return refineries < wanted && harvesters >= refineries && idle_harvesters == 0
        && (refineries < 2 || bonus > 0 || cash < 12000);
}

#endif
