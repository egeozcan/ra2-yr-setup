/* Decisions shared by the engine hook and the native regression checks. */
#ifndef OIL_DEFENSE_POLICY_H
#define OIL_DEFENSE_POLICY_H

#define OIL_THREAT_RADIUS 12
#define OIL_DEFENSE_RADIUS 6
#define OIL_DEFENSE_LIMIT 3
#define OIL_BUILD_INTERVAL 450
#define OIL_CASH_RESERVE 1500

enum { OIL_NONE, OIL_GROUND, OIL_AIR };

static int oil_ai_active(int mode, int human, int difficulty, int defeated,
                         int production, int conyards, int refineries)
{
    return mode == 5 && !human && difficulty == 0 && !defeated
        && production && conyards > 0 && refineries > 0;
}

static int oil_near(int ax, int ay, int bx, int by, int radius)
{
    int dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy <= radius * radius;
}

static int oil_defense_need(int ground_threat, int air_threat, int ground_defenses,
                            int air_defenses, int total_defenses)
{
    if (total_defenses >= OIL_DEFENSE_LIMIT)
        return OIL_NONE;
    if (air_threat && !air_defenses)
        return OIL_AIR;
    if (ground_threat && ground_defenses < 2)
        return OIL_GROUND;
    return OIL_NONE;
}

static int oil_can_budget(int cash, int cost, int power_output, int power_drain,
                          int new_drain, int buildable)
{
    return buildable > 0 && cash - cost >= OIL_CASH_RESERVE
        && power_output - power_drain >= new_drain;
}

#endif
