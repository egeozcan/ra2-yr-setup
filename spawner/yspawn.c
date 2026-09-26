/* yspawn.dll - start Yuri's Revenge (gamemd.exe 1.001) straight into a skirmish, skipping the menus.
 *
 * Loaded through an extra import added to a copy of the exe (gamemd-spawn.exe, see add_import.py).
 * DllMain only rewrites code; the real work happens in spawn_start(), which replaces the game's
 * call to the main menu and fills in the skirmish settings the menu would have set, from yspawn.ini.
 *
 * Addresses and layouts come from the YRpp headers and CnCNet's yrpp-spawner (both GPL-3,
 * github.com/Phobos-developers/YRpp, github.com/CnCNet/yrpp-spawner); this is a small skirmish-only
 * reimplementation of the spawner's start-game path.
 */
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define GFASTCALL __attribute__((fastcall))
#define GTHISCALL __attribute__((thiscall))

/* ---- game addresses ---- */
#define CALL_MAINMENU_1   0x48CDD3  /* Main_Game: call Select_Game (0x52D9A0) */
#define CALL_MAINMENU_2   0x48CFAA  /* Main_Game: call Select_Game after a match */
#define SKIP_INTRO        0x52CB50  /* -> 0x52CB6E */
#define SKIP_LOGO         0x52C5E0  /* -> 0x52C5F8 */
#define AI_GANG_UP        0x501640  /* allies all computer players with each other, see DllMain */

#define GAME_ISACTIVE     ((char *)0xA8E9A0)
#define PCX_INITIALIZED   ((char *)0xAC48D4)
#define GAME_SEED         ((int *)0xA8ED94)
#define GAME_TECHLEVEL    ((int *)0x822CF4)
#define GAME_PLAYERCOUNT  ((int *)0xA8B54C)
#define GAME_PLAYERCOLOR  ((int *)0xA8B394)
#define GAME_SCENARIONAME ((char *)0xA8B8E0)   /* char[0x200] */
#define OPTIONS_GAMESPEED ((int *)0xA8EB60)    /* GameOptionsClass::Instance.GameSpeed */
#define SESSION           ((Session *)0xA8B238)
#define RULES             (*(void **)0x8871E0)  /* RulesClass::Instance */
#define INI_RULES         (*(void **)0x887048)  /* CCINIClass::INI_Rules */
#define HOUSETYPE_ARRAY   ((DynVec *)0xA83C98)
#define NODE_ARRAY        ((DynVec *)0xA8DA74)  /* DynamicVectorClass<NodeNameType*> */

typedef void (*void_fn)(void);
typedef void *(*alloc_fn)(size_t);                     /* game operator new, cdecl */
typedef void *(GFASTCALL *mpmode_get_fn)(int);
typedef void (GTHISCALL *read_descs_fn)(void *);
typedef char (GTHISCALL *rules_read_fn)(void *, void *);
typedef char (GFASTCALL *start_scenario_fn)(const char *, int, int);

#define GAME_ALLOC            ((alloc_fn)0x7C8E17)
#define INIT_COMMON_DIALOGS   ((void_fn)0x600560)
#define INIT_UI_COLORSHIFTS   ((void_fn)0x61F190)
#define LOAD_PCX_FILES        ((void_fn)0x61F210)
#define INIT_RANDOM           ((void_fn)0x52FC20)
#define MPGAMEMODE_GET        ((mpmode_get_fn)0x5D5F30)
#define READ_SCENARIO_DESCS   ((read_descs_fn)0x699980)
#define RULES_READ_COUNTRIES  ((rules_read_fn)0x6722F0)
#define RULES_READ_SIDES      ((rules_read_fn)0x672440)
#define HOUSETYPE_LOADFROMINI ((rules_read_fn)0x511850)   /* vtable slot 25 */
#define START_SCENARIO        ((start_scenario_fn)0x683AB0)

/* ---- game structures (YRpp) ---- */
typedef struct {
    int Difficulties[8], Countries[8], Colors[8], Starts[8], Allies[8];
} AISlots;

typedef struct {
    int MPModeIndex;
    int ScenarioIndex;
    char Bases;
    int Money;
    char BridgeDestruction, Crates, ShortGame, SWAllowed, BuildOffAlly;
    int GameSpeed;
    char MultiEngineer;
    int UnitCount;
    int AIPlayers;
    int AIDifficulty;
    AISlots Slots;
    char AlliesAllowed, HarvesterTruce, CaptureTheFlag, FogOfWar, MCVRedeploy;
    wchar_t MapDescription[45];
} GameModeOptions;

typedef struct {
    int GameMode;          /* 5 = skirmish */
    void *MPGameMode;
    DWORD unknown[3];
    int CommProtocol;
    GameModeOptions Config;  /* == GameModeOptionsClass::Instance at 0xA8B250 */
} Session;

typedef struct {
    void *vtable;
    void **Items;
    int Capacity;
    char IsInitialized, IsAllocated;
    int Count;
    int CapacityIncrement;
} DynVec;

#pragma pack(push, 1)
typedef struct {
    wchar_t Name[20];
    BYTE Address[16];      /* sockaddr_in */
    char Serial[19];
    int Country, InitialCountry, Color, InitialColor, StartPoint, InitialStartPoint, Team, InitialTeam;
    DWORD SpectatorFlag;
    int HouseIndex;
    int Time;
    DWORD unknown_77;
    int Clan;
    DWORD unknown_7F;
    BYTE unknown_83, unknown_84;
} NodeName;
#pragma pack(pop)

_Static_assert(sizeof(NodeName) == 0x85, "NodeNameType size");
_Static_assert(offsetof(NodeName, StartPoint) == 0x5B, "NodeNameType.StartPoint");
_Static_assert(offsetof(NodeName, Team) == 0x63, "NodeNameType.Team");
_Static_assert(offsetof(Session, Config.Slots.Starts) == 0xA4, "AISlots.Starts at 0xA8B2DC");
_Static_assert(offsetof(Session, Config.Slots.Allies) == 0xC4, "AISlots.Allies at 0xA8B2FC");
_Static_assert(offsetof(Session, Config) == 0x18, "Session.Config");
_Static_assert(offsetof(GameModeOptions, Money) == 0x0C, "Money");
_Static_assert(offsetof(GameModeOptions, GameSpeed) == 0x18, "GameSpeed");
_Static_assert(offsetof(GameModeOptions, UnitCount) == 0x20, "UnitCount");
_Static_assert(offsetof(GameModeOptions, Slots) == 0x2C, "AISlots");
_Static_assert(offsetof(GameModeOptions, AlliesAllowed) == 0xCC, "AlliesAllowed");
_Static_assert(offsetof(DynVec, Count) == 0x10, "DynamicVectorClass.Count");

/* ---- logging ---- */
static FILE *logf;
static void logmsg(const char *fmt, ...)
{
    if (!logf)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

/* ---- code patching ---- */
static void patch(DWORD addr, const BYTE *bytes, size_t n)
{
    DWORD old;
    VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)addr, bytes, n);
    VirtualProtect((void *)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
}

static void patch_rel(DWORD addr, BYTE opcode, DWORD target)
{
    BYTE b[5] = { opcode };
    DWORD rel = target - (addr + 5);
    memcpy(b + 1, &rel, 4);
    patch(addr, b, 5);
}

/* ---- config (yspawn.ini in the game directory) ---- */
#define INI ".\\yspawn.ini"

static int ini_int(const char *sec, const char *key, int def)
{
    return GetPrivateProfileIntA(sec, key, def, INI);
}

static void ini_str(const char *sec, const char *key, const char *def, char *out, DWORD n)
{
    GetPrivateProfileStringA(sec, key, def, out, n, INI);
}

/* ---- the replacement for the main menu ---- */
static int started;

/* Start positions and teams go where the skirmish menu puts them; the game's own code then uses them:
 * ScenarioClass::AssignHouses (0x687F10) copies them into each house (HouseClass +0x16058 start,
 * +0x1605C team), MPGameModeClass vtable +0x80 (0x5D6BE0) places houses with a fixed start, +0x84 gives
 * the rest random free starts, and +0x88 AllyTeams (0x5D74A0) allies every two houses with the same team.
 * The game's values: start 0..7 or -2 = random (-1 would index the start list at -1), team 0..3 = A..D or
 * -1 = none. yspawn.ini uses -1 for random start. */
static int ini_start(const char *sec)
{
    int v = ini_int(sec, "Start", -1);
    return v >= 0 && v < 8 ? v : -2;
}

static int ini_team(const char *sec)
{
    int v = ini_int(sec, "Team", -1);
    return v >= 0 && v < 4 ? v : -1;
}

static char GFASTCALL spawn_start(char unused)
{
    (void)unused;
    if (started) {                  /* second call = match over: quit instead of showing the menu */
        logmsg("match over, exiting");
        return 0;
    }
    started = 1;

    char scenario[260], name[20];
    ini_str("Settings", "Scenario", "yspawn.map", scenario, sizeof scenario);
    ini_str("Settings", "Name", "Commander", name, sizeof name);
    int mode = ini_int("Settings", "GameMode", 1);
    int country = ini_int("Settings", "Country", 0);
    int color = ini_int("Settings", "Color", 0);
    int start = ini_start("Settings");
    int team = ini_team("Settings");
    logmsg("scenario=%s mode=%d country=%d color=%d start=%d team=%d", scenario, mode, country, color, start, team);

    *GAME_ISACTIVE = 1;
    INIT_COMMON_DIALOGS();
    if (!*PCX_INITIALIZED) {
        *PCX_INITIALIZED = 1;
        INIT_UI_COLORSHIFTS();
        LOAD_PCX_FILES();
    }
    logmsg("ui initialised");

    /* countries and sides are normally loaded by the menus */
    RULES_READ_COUNTRIES(RULES, INI_RULES);
    RULES_READ_SIDES(RULES, INI_RULES);
    DynVec *ht = HOUSETYPE_ARRAY;
    for (int i = 0; i < ht->Count; i++)
        HOUSETYPE_LOADFROMINI(ht->Items[i], INI_RULES);
    logmsg("countries loaded: %d", ht->Count);

    strncpy(GAME_SCENARIONAME, scenario, 0x1FF);
    READ_SCENARIO_DESCS(SESSION);
    logmsg("scenario descriptions read");

    Session *s = SESSION;
    s->MPGameMode = MPGAMEMODE_GET(mode);
    if (!s->MPGameMode)
        s->MPGameMode = MPGAMEMODE_GET(1);
    logmsg("game mode %p", s->MPGameMode);

    GameModeOptions *o = &s->Config;
    o->MPModeIndex = mode;
    o->Bases = ini_int("Settings", "Bases", 1);
    o->Money = ini_int("Settings", "Credits", 10000);
    o->BridgeDestruction = ini_int("Settings", "BridgeDestroy", 1);
    o->Crates = ini_int("Settings", "Crates", 0);
    o->ShortGame = ini_int("Settings", "ShortGame", 1);
    o->SWAllowed = ini_int("Settings", "Superweapons", 1);
    o->BuildOffAlly = ini_int("Settings", "BuildOffAlly", 1);
    o->GameSpeed = ini_int("Settings", "GameSpeed", 0);
    o->MultiEngineer = 0;
    o->UnitCount = ini_int("Settings", "UnitCount", 0);
    o->AlliesAllowed = 0;
    o->HarvesterTruce = 0;
    o->FogOfWar = 0;
    o->MCVRedeploy = ini_int("Settings", "MCVRedeploy", 1);
    MultiByteToWideChar(CP_ACP, 0, scenario, -1, o->MapDescription, 45);

    int seed = ini_int("Settings", "Seed", 0);
    *GAME_SEED = seed ? seed : (int)GetTickCount();
    *GAME_TECHLEVEL = ini_int("Settings", "TechLevel", 10);
    *GAME_PLAYERCOLOR = color;
    *OPTIONS_GAMESPEED = o->GameSpeed;

    /* AI opponents: sections [AI1]..[AI7] */
    int ais = 0;
    for (int i = 0; i < 8; i++) {
        o->Slots.Difficulties[i] = -1;
        o->Slots.Countries[i] = -1;
        o->Slots.Colors[i] = -1;
        o->Slots.Starts[i] = -1;
        o->Slots.Allies[i] = -1;
    }
    for (int i = 1; i < 8; i++) {
        char sec[8];
        snprintf(sec, sizeof sec, "AI%d", i);
        int c = ini_int(sec, "Country", -100);
        if (c == -100)
            continue;
        o->Slots.Countries[i] = c;
        o->Slots.Colors[i] = ini_int(sec, "Color", i);
        o->Slots.Difficulties[i] = ini_int(sec, "Difficulty", 2);   /* 0 hard, 1 medium, 2 easy */
        o->Slots.Starts[i] = ini_start(sec);
        o->Slots.Allies[i] = ini_team(sec);
        if (!ais)
            o->AIDifficulty = o->Slots.Difficulties[i];
        ais++;
        logmsg("AI slot %d: country %d color %d difficulty %d start %d team %d", i, c, o->Slots.Colors[i],
               o->Slots.Difficulties[i], o->Slots.Starts[i], o->Slots.Allies[i]);
    }
    o->AIPlayers = ais;

    /* the human player */
    NodeName *node = GAME_ALLOC(sizeof *node);
    memset(node, 0, sizeof *node);
    MultiByteToWideChar(CP_ACP, 0, name, -1, node->Name, 20);
    node->Country = node->InitialCountry = country;
    node->Color = node->InitialColor = color;
    node->StartPoint = node->InitialStartPoint = start;   /* read through 0x696F50 */
    node->Team = node->InitialTeam = team;                 /* read directly (+0x63) */
    node->Time = -1;

    DynVec *v = NODE_ARRAY;
    logmsg("node array: items %p cap %d count %d incr %d", v->Items, v->Capacity, v->Count, v->CapacityIncrement);
    if (v->Count >= v->Capacity) {   /* grow with the game's allocator; the old block is left alone */
        int cap = v->Capacity + (v->CapacityIncrement > 0 ? v->CapacityIncrement : 10);
        void **items = GAME_ALLOC(cap * sizeof *items);
        for (int i = 0; i < v->Count; i++)
            items[i] = v->Items[i];
        v->Items = items;
        v->Capacity = cap;
        v->IsAllocated = 1;
        v->IsInitialized = 1;
    }
    v->Items[v->Count++] = node;
    *GAME_PLAYERCOUNT = v->Count;

    s->GameMode = 5;   /* skirmish */
    INIT_RANDOM();
    logmsg("starting scenario");
    char ok = START_SCENARIO(scenario, 0, -1);
    logmsg("StartScenario returned %d", ok);
    return ok;
}

__declspec(dllexport) int yspawn_init(void) { return 0; }   /* the symbol the exe imports */

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    logf = fopen("yspawn.log", "w");
    logmsg("yspawn loaded");

    patch_rel(CALL_MAINMENU_1, 0xE8, (DWORD)spawn_start);
    patch_rel(CALL_MAINMENU_2, 0xE8, (DWORD)spawn_start);
    patch_rel(SKIP_INTRO, 0xE9, 0x52CB6E);
    patch_rel(SKIP_LOGO, 0xE9, 0x52C5F8);
    /* 0x501640 allies every computer player with every other one unless ScenarioClass+0x11E0 is set, and
     * AssignHouses only sets that when someone has a team. So with no teams the stock game has all the AIs
     * allied against you. Returning at once makes "no team" mean everyone for themselves. */
    patch(AI_GANG_UP, (const BYTE[]){ 0xC3 }, 1);
    logmsg("patched");
    return TRUE;
}
