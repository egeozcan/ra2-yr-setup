#ifndef COMBAT_AI_POLICY_H
#define COMBAT_AI_POLICY_H

/* Extra production follows queued units rather than a per-match coin flip. */
static int combat_need_factory(int missing_vehicles)
{
    return missing_vehicles >= 8;
}

static int combat_need_airbase(int owned_planes, int missing_planes)
{
    /* One base has four docks. A single unbuilt four-plane team fits there. */
    return owned_planes >= 4 || owned_planes + missing_planes > 4;
}

static int combat_expand(int frame, int buildings, int refineries, int cash,
                          int power, int cost, int drain, int count, int needed)
{
    return needed && frame >= 1800 && buildings >= 8 && refineries >= 2
        && count == 1 && cash >= 6500 && cash - cost >= 3000 && power >= drain + 50;
}

static int combat_siege_mission(int mission)
{
    return mission == 1 || mission == 2 || mission == 15 || mission == 29;
}

#endif
