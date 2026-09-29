#ifndef COMBAT_AI_POLICY_H
#define COMBAT_AI_POLICY_H

/* Extra production is a per-match choice, independent of the game RNG stream. */
static unsigned combat_choices(unsigned seed, unsigned house)
{
    unsigned n = seed ^ ((house + 1u) * 0x9E3779B9u);
    n ^= n >> 16;
    n *= 0x85EBCA6Bu;
    n ^= n >> 13;
    return n & 3u; /* bit 0 war factory; bit 1 Allied airbase */
}

static int combat_expand(int frame, int buildings, int refineries, int cash,
                          int power, int cost, int drain, int count, int selected)
{
    return selected && frame >= 2700 && buildings >= 10 && refineries >= 2
        && count == 1 && cash >= 8000 && cash - cost >= 3000 && power >= drain + 50;
}

static int combat_siege_mission(int mission)
{
    return mission == 1 || mission == 2 || mission == 15 || mission == 29;
}

#endif
