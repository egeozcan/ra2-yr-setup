/* Optional AI-vs-AI benchmark for measuring Brutal AI changes.
 * [Settings] Benchmark=1 writes yspawn-bench.csv beside the exe: one row per player house every
 * BENCH_INTERVAL frames, then a result row when one side is left (or FrameLimit= is reached),
 * after which the game exits. [AIn] Director=0 turns the strategy director (director.h) off for that
 * AI (default on), so old and new Brutal logic can fight in the same match; DirectorFlags= keeps only
 * some director features on (DIR_F_* bits, for ablation runs).
 * HouseClass offsets: YRpp field order, anchored on known fields (OwnedBuildings 0x2F0,
 * Balance 0x30C, PowerOutput 0x53A4, BaseSpawnCell 0x5490, ProducingBuildingTypeIndex 0x564C). */
#define BENCH_INTERVAL 300
#define H_OWNED_UNITS 0x2E8
#define H_OWNED_NAVY 0x2EC
#define H_OWNED_BUILDINGS 0x2F0
#define H_OWNED_INFANTRY 0x2F4
#define H_OWNED_AIRCRAFT 0x2F8
#define H_HARVESTERS 0x158
#define H_KILLED_UNITS 0x5434
#define H_KILLED_BUILDINGS 0x5488
#define H_COST_INFANTRY 0x160A8
#define H_COST_VEHICLES 0x160AC
#define H_COST_AIRCRAFT 0x160B0
#define H_ALLIES 0x5788

#include "director-policy.h"
static FILE *bench_file;
static void bench_row_extra(BYTE *house);
static int bench_fleet;
static int bench_limit, bench_next, bench_over, bench_players, reveal_map, reveal_next, bench_camera = -1, bench_camera_x,
    bench_camera_y;
static int bench_force_island, bench_force_expand, bench_test_escape;   /* tests: force ferries / expansion */   /* test: treat every enemy as cut off by water, to exercise ferries */
static void bench_camera_update(void);
static void bench_kills_dump(void);
static DWORD bench_start_ms;
static int director_slot[8] = { DIR_F_DEFAULT, DIR_F_DEFAULT, DIR_F_DEFAULT, DIR_F_DEFAULT, DIR_F_DEFAULT, DIR_F_DEFAULT, DIR_F_DEFAULT, DIR_F_DEFAULT };
static int director_house[32], director_mapped;
static int director_plan_slot[8] = { -1, -1, -1, -1, -1, -1, -1, -1 }, director_plan_house[32];

static void bench_init(void)
{
    for (int i = 1; i < 8; i++) {
        char sec[8];
        snprintf(sec, sizeof sec, "AI%d", i);
        director_slot[i] = ini_int(sec, "Director", 1) ? ini_int(sec, "DirectorFlags", DIR_F_DEFAULT) : 0;
        director_plan_slot[i] = ini_int(sec, "DirectorPlan", -1);   /* force a strategy plan (tests) */
    }
    reveal_map = ini_int("Settings", "RevealMap", 0);        /* the human sees the whole map, any launch */
    /* watching 7 AIs: the message list (MessageListClass::Init at 0x4A8BB8, push 6 at 0x4A8BAF) holds
     * 12 lines instead of 6, so a resource line per AI fits beside plan news; its buffers hold 14 */
    if (reveal_map && patch_checked("message list lines", 0x4A8BAF, (const BYTE[]){ 0x6A, 0x06 }, 2))
        patch(0x4A8BB0, (const BYTE[]){ 12 }, 1);
    if (!ini_int("Settings", "Benchmark", 0))
        return;
    bench_limit = ini_int("Settings", "FrameLimit", 0);
    bench_camera = ini_int("Settings", "Camera", -1);        /* follow this house's army front */
    bench_fleet = ini_int("Settings", "FleetTest", 0);       /* N warships for AI house 1 at frame 300 */
    bench_camera_x = ini_int("Settings", "CameraX", 0);      /* or watch one cell */
    bench_camera_y = ini_int("Settings", "CameraY", 0);
    bench_force_island = ini_int("Settings", "ForceIsland", 0);
    bench_force_expand = ini_int("Settings", "ForceExpand", 0);
    bench_test_escape = ini_int("Settings", "TestEscape", 0);   /* AI house 1's yard flees at frame 600 */
    bench_file = fopen("yspawn-bench.csv", "w");
    if (!bench_file) {
        logmsg("benchmark: could not open yspawn-bench.csv");
        return;
    }
    fputs("frame,ms,house,country,human,director,defeated,units,infantry,aircraft,navy,buildings,cash,"
          "harvesters,refineries,killed_units,killed_buildings,cost_infantry,cost_vehicles,cost_aircraft,power,drain,"
          "war_factories,building_order,unit_order,infantry_order,state,army_value,flags,plan,posture,plan_now\n",
          bench_file);
    logmsg("benchmark: enabled, frame limit %d", bench_limit);
}

/* Houses 0..players-1 are the match's players, in the order AssignHouses created them:
 * checked in yspawn.log's "benchmark: house" lines. AI slots follow [AI1]..[AI7] order. */
static void bench_map_houses(void)
{
    DynVec *v = HOUSE_ARRAY;
    int slot = 1;
    director_mapped = 1;
    for (int i = 0; i < 32; i++)
        director_plan_house[i] = -1;
    bench_players = *GAME_PLAYERCOUNT + SESSION->Config.AIPlayers;
    for (int i = 0; i < v->Count && i < 32; i++) {
        BYTE *h = v->Items[i];
        if (i >= bench_players)
            break;
        if (!h[H_ISHUMAN]) {
            while (slot < 8 && SESSION->Config.Slots.Countries[slot] < 0)
                slot++;
            director_house[i] = slot < 8 ? director_slot[slot] : 1;
            director_plan_house[i] = slot < 8 ? director_plan_slot[slot] : -1;
            slot++;
        }
        logmsg("house %d %s human=%d director=%#x", i, (char *)FIELD(h, H_TYPE, BYTE *) + T_ID,
               h[H_ISHUMAN] != 0, director_house[i]);
    }
}

/* [AIn] DirectorPlan for the house, or -1 for a plan drawn from the setup. */
static int director_plan_override(BYTE *house)
{
    if (!director_mapped)
        bench_map_houses();
    int idx = FIELD(house, 0x30, int);
    return idx >= 0 && idx < 32 ? director_plan_house[idx] : -1;
}

/* Director feature bits for the house (0: director off). Every match maps houses on first use. */
static int director_enabled(BYTE *house)
{
    if (!director_mapped)
        bench_map_houses();
    int idx = FIELD(house, 0x30, int);
    return idx >= 0 && idx < 32 ? director_house[idx] : 0;
}

static void bench_row(BYTE *h)
{
    fprintf(bench_file, "%d,%lu,%d,%.24s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
            CURRENT_FRAME, (unsigned long)(GetTickCount() - bench_start_ms), FIELD(h, 0x30, int),
            (char *)FIELD(h, H_TYPE, BYTE *) + T_ID, h[H_ISHUMAN] != 0, director_house[FIELD(h, 0x30, int) & 31] != 0,
            h[OIL_H_DEFEATED] != 0, FIELD(h, H_OWNED_UNITS, int), FIELD(h, H_OWNED_INFANTRY, int),
            FIELD(h, H_OWNED_AIRCRAFT, int), FIELD(h, H_OWNED_NAVY, int), FIELD(h, H_OWNED_BUILDINGS, int),
            FIELD(h, OIL_H_CASH, int), FIELD(h, H_HARVESTERS, int), FIELD(h, OIL_H_REFINERIES, int),
            FIELD(h, H_KILLED_UNITS, int), FIELD(h, H_KILLED_BUILDINGS, int), FIELD(h, H_COST_INFANTRY, int),
            FIELD(h, H_COST_VEHICLES, int), FIELD(h, H_COST_AIRCRAFT, int), FIELD(h, OIL_H_POWER, int),
            FIELD(h, OIL_H_DRAIN, int));
    bench_row_extra(h);   /* production and director columns, from director.h */
}

/* [Settings] RevealMap=1: the human player sees the whole map, for watching AI matches. The reveal
 * is MapClass::Reveal, the Spy Satellite's own call. It is one-shot, and Gap Generators shroud
 * their area again, so it is repeated every REVEAL_INTERVAL frames. Called from the per-house AI
 * update, like bench_sample, so it also runs in ordinary (non-benchmark) launches. */
#define REVEAL_INTERVAL 150
static void observer_reveal(void)
{
    if (!reveal_map || CURRENT_FRAME < reveal_next)
        return;
    reveal_next = CURRENT_FRAME + REVEAL_INTERVAL;
    DynVec *hv = HOUSE_ARRAY;
    int players = *GAME_PLAYERCOUNT + SESSION->Config.AIPlayers;
    for (int i = 0; i < hv->Count && i < players; i++)
        if (((BYTE *)hv->Items[i])[H_ISHUMAN])
            ((void (GTHISCALL *)(void *, BYTE *))0x577D90)(MAP_INSTANCE, hv->Items[i]);
}

/* Called from the per-house AI update; samples every player house once per interval. */
static void bench_sample(void)
{
    if (!bench_file || bench_over || CURRENT_FRAME < bench_next)
        return;
    if (!bench_next) {
        bench_start_ms = GetTickCount();
        if (!director_mapped)
            bench_map_houses();
    }
    bench_camera_update();
    bench_next = CURRENT_FRAME + BENCH_INTERVAL;
    DynVec *v = HOUSE_ARRAY;
    BYTE *alive[32];
    int n = 0;
    for (int i = 0; i < v->Count && i < bench_players; i++) {
        BYTE *h = v->Items[i];
        bench_row(h);
        if (!h[H_ISHUMAN] && !h[OIL_H_DEFEATED])
            alive[n++] = h;
    }
    /* Over when every surviving AI is allied with the first one (ignoring the idle human). */
    int sides = n > 0;
    for (int i = 1; i < n; i++)
        if (!(FIELD(alive[0], H_ALLIES, DWORD) & (1u << FIELD(alive[i], 0x30, int))))
            sides = 2;
    int timeout = bench_limit && CURRENT_FRAME >= bench_limit;
    if (sides <= 1 || timeout) {
        bench_over = 1;
        bench_kills_dump();
        fprintf(bench_file, "result,%d,%s,%d\n", CURRENT_FRAME, timeout && sides > 1 ? "timeout" : "win",
                n ? FIELD(alive[0], 0x30, int) : -1);
        fclose(bench_file);
        bench_file = NULL;
        logmsg("benchmark: %s at frame %d", sides <= 1 ? "one side left" : "frame limit", CURRENT_FRAME);
        if (logf)
            fflush(logf);
        ExitProcess(0);
    }
    fflush(bench_file);
}
