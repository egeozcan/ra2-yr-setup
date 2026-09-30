#ifndef DIRECTOR_POLICY_H
#define DIRECTOR_POLICY_H

/* Pure decisions for the Brutal strategy director (director.h); unit-tested in test_director.py. */

enum { DIR_GATHER, DIR_ATTACK, DIR_DEFEND, DIR_RETREAT };
/* [AIn] DirectorFlags bits, for ablation runs; all are on by default. */
enum { DIR_F_PRODUCTION = 1, DIR_F_ARMY = 2, DIR_F_TAKEOVER = 4, DIR_F_FOCUS = 8, DIR_F_ECONOMY = 16 };
enum { ROLE_MAIN, ROLE_AA, ROLE_SIEGE, ROLE_SUPPORT, ROLE_COUNT };

/* Launch once the army can beat what the enemy fields (its mobile army plus half its static
 * defenses), with a floor that rises over time. A maxed-out army goes regardless. */
static int dir_should_launch(int army_value, int army_count, int enemy_army_value, int enemy_defense, int frame)
{
    if (army_count < 6)
        return 0;
    if (army_value >= 60000)
        return 1;
    int floor = frame < 12000 ? 5000 : frame < 24000 ? 8000 : 12000;
    long long opposition = enemy_army_value + enemy_defense / 2;
    return army_value >= floor && (long long)army_value * 10 >= opposition * 12;
}

/* Fall back when the attack has been bled and the local fight is lost. */
static int dir_should_retreat(int army_value, int launch_value, int local_enemy_value)
{
    return army_value * 100 < launch_value * 40 && local_enemy_value * 10 > army_value * 12;
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

/* More refineries once money runs short: each brings a harvester and a shorter ore run. */
static int dir_want_refinery(int frame, int refineries, int harvesters, int cash)
{
    int wanted = frame < 6000 ? 0 : frame < 15000 ? 3 : frame < 30000 ? 4 : 5;
    return refineries < wanted && harvesters >= refineries && cash < 12000;
}

/* Keep producing while money lasts; keep a reserve for buildings. */
static int dir_can_spend(int cash, int cost, int reserve)
{
    return cash - cost >= reserve;
}

#endif
