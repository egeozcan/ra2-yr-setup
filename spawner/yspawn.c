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
#include <stdlib.h>
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
static int human_in_peace, peace_ready;

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

static void start_bases(void);
static void spawn_units(void);

static char GFASTCALL spawn_start(char unused)
{
    (void)unused;
    if (started) {                  /* second call = match over: quit instead of showing the menu */
        logmsg("match over, exiting");
        return 0;
    }
    started = 1;
    if (human_in_peace && !peace_ready) {
        logmsg("human in peace: required hooks unavailable; refusing to start");
        MessageBoxA(NULL, "Human in peace could not be enabled for this game executable. See yspawn.log.",
                    "Skirmish Setup", MB_OK | MB_ICONERROR);
        return 0;
    }

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
    if (ok) {
        start_bases();
        spawn_units();
    }
    return ok;
}

/* ---- Magnetron carry ----
 * Stock: the Magnetron's IsLocomotor warhead gives the victim a Jumpjet locomotor (ImbueLocomotor 0x710000,
 * magnetron+0x2AC LocomotorTarget = victim, victim+0x2B0 LocomotorSource = magnetron, victim+0x6AD
 * IsAttackedByLocomotor) and sends it towards the Magnetron. ReleaseLocomotor (0x70FEE0) drops it: in the air it
 * falls and takes damage. The stock drops happen when the Magnetron gets a destination (FootClass::SetDestination),
 * goes idle (UnitClass::EnterIdleMode), or the victim arrives (Jumpjet cruising state).
 * For a human player's Magnetron those drops are skipped: the victim hovers next to it and follows it around, and
 * the Stop command (S) drops it. Computer players keep the stock behaviour. Death, transports, chrono, a new
 * Magnetron target and the like still drop it through the untouched stock paths.
 * Attack-ground (force-fire on a cell) throws the victim instead of firing: it flies to that cell, still linked, and
 * the stock arrival drop releases it there. Released in the air before that, it would fall where it is: the Jumpjet
 * Process (0x54AF33) keeps a falling unit's destination on its own cell. */
#define RELEASE_LOCOMOTOR   0x70FEE0
#define CALL_RELEASE_SETDEST 0x4D9509  /* FootClass::SetDestination: release when given a destination */
#define CALL_RELEASE_IDLE   0x7389BF   /* UnitClass::EnterIdleMode */
#define JJ_ARRIVED          0x54C1B3   /* Jumpjet cruising state: victim arrived, release and descend */
#define JJ_ARRIVED_STOCK    0x54C1BD
#define JJ_STATE_DONE       0x54C4FD
#define STOP_EVENT          0x4C7512   /* EventClass::Execute, Stop, ESI = the techno */
#define WAVE_KEEP           0x762B9B   /* WaveClass::Update_Wave: end the beam unless owner->Target == wave->Target */
#define UNIT_AI_SLOT        0x7F5CCC   /* UnitClass vtable: AI */
#define UNIT_AI             0x7360C0
#define UNIT_FIRE_SLOT      0x7F603C   /* UnitClass vtable: Fire(target, weapon index) */
#define UNIT_FIRE           0x741340
#define JUMPJET_ILOCO_VT    0x7ECD68   /* JumpjetLocomotionClass ILocomotion vtable */
#define CURRENT_FRAME       (*(int *)0xA8ED84)

/* object offsets (YRpp, checked against this exe) */
#define O_OWNER       0x21C   /* TechnoClass: HouseClass* */
#define O_LOCOTARGET  0x2AC   /* TechnoClass: FootClass* the victim */
#define O_LOCOSOURCE  0x2B0   /* TechnoClass: FootClass* the Magnetron */
#define O_TARGET      0x2B4   /* TechnoClass: AbstractClass* Target */
#define O_LOCATION    0x09C   /* AbstractClass: CoordStruct */
#define O_LOCOMOTOR   0x674   /* FootClass: ILocomotion* */
#define O_ATTACKEDBYLOCO 0x6AD
#define H_ISHUMAN     0x1EC   /* HouseClass */
#define C_MAPCOORDS   0x024   /* CellClass: CellStruct, short X, Y */
#define W_RANGE       0x0B4   /* WeaponTypeClass: Range in leptons, -512 = unlimited */
#define ABS_CELL      11      /* AbstractType::Cell */
/* vtable slots */
#define VT_WHATAMI    0x02C
#define VT_SETTARGET  0x3C8
#define VT_GETWEAPON  0x3F8   /* WeaponStruct*, whose first field is the WeaponTypeClass* */
#define VT_MOVETO     0x044   /* ILocomotion::Move_To */
/* JumpjetLocomotionClass, from its ILocomotion pointer (= object + 4) */
#define J_DEST        0x3C    /* CoordStruct DestinationCoords */
#define J_ISMOVING    0x48
#define J_STATE       0x4C    /* 2 hovering, 3 cruising, 4 descending */
#define U_TYPE        0x6C4   /* UnitClass: UnitTypeClass* */
#define TT_JUMPJET    0xD94   /* TechnoTypeClass: JumpJet= (stored at 0x715200 by LoadFromINI) */
#define TT_BALLOONHOVER 0xD6A /* TechnoTypeClass: BalloonHover= (stored at 0x714DA9) */

typedef struct { int X, Y, Z; } Coord;
typedef void (GTHISCALL *release_fn)(BYTE *, char);
typedef void (__stdcall *moveto_fn)(void *, Coord);
typedef void *(GTHISCALL *fire_fn)(BYTE *, BYTE *, int);
#define FIELD(p, off, type) (*(type *)((BYTE *)(p) + (off)))
#define VFUNC(p, off) FIELD(FIELD(p, 0, BYTE *), off, void *)

static int carry_owner(BYTE *mag)
{
    BYTE *house = FIELD(mag, O_OWNER, BYTE *);
    return house && house[H_ISHUMAN];
}

/* the victim's locomotor, if it is the Jumpjet one the Magnetron gave it */
static BYTE *victim_jumpjet(BYTE *victim)
{
    BYTE *loco = FIELD(victim, O_LOCOMOTOR, BYTE *);
    return loco && FIELD(loco, 0, DWORD) == JUMPJET_ILOCO_VT ? loco : NULL;
}

/* Thrown victims, on their way to an attack-ground cell: the follow leaves them alone and they drop on arrival. A
 * record counts only while its link holds (victim->LocomotorSource == mag), and it is forgotten on arrival, on Stop,
 * and when either unit's link no longer matches it (carry_update). Only live objects are dereferenced. */
#define MAX_THROWS 16
static struct { BYTE *victim, *mag; } throws[MAX_THROWS];

static int throw_slot(BYTE *victim)
{
    for (int i = 0; i < MAX_THROWS; i++)
        if (throws[i].victim == victim)
            return i;
    return -1;
}

static int thrown(BYTE *victim)
{
    int i = throw_slot(victim);
    return victim && i >= 0 && FIELD(victim, O_LOCOSOURCE, BYTE *) == throws[i].mag;
}

static void throw_forget(int i)
{
    throws[i].victim = throws[i].mag = NULL;
}

static void throw_record(BYTE *victim, BYTE *mag)
{
    static int next;
    int i = throw_slot(victim);
    if (i < 0)
        i = throw_slot(NULL);
    if (i < 0)
        i = next++ % MAX_THROWS;   /* full: 16 throws in flight at once, reuse the oldest-ish */
    throws[i].victim = victim;
    throws[i].mag = mag;
}

static int is_cell(BYTE *obj)
{
    return obj && ((int (GTHISCALL *)(BYTE *))VFUNC(obj, VT_WHATAMI))(obj) == ABS_CELL;
}

static Coord cell_center(BYTE *cell)
{
    short *mc = &FIELD(cell, C_MAPCOORDS, short);
    return (Coord){ mc[0] * 256 + 128, mc[1] * 256 + 128, 0 };
}

/* within the primary weapon's Range, without its MinimumRange */
static int cell_in_range(BYTE *unit, BYTE *cell)
{
    BYTE **ws = ((BYTE **(GTHISCALL *)(BYTE *, int))VFUNC(unit, VT_GETWEAPON))(unit, 0);
    if (!ws || !*ws)
        return 0;
    int r = FIELD(*ws, W_RANGE, int);
    if (r == -512)
        return 1;
    Coord c = cell_center(cell), *m = &FIELD(unit, O_LOCATION, Coord);
    long long dx = c.X - m->X, dy = c.Y - m->Y;
    return dx * dx + dy * dy <= (long long)r * r;
}

/* send the held victim to the cell; the Magnetron's attack order ends */
static void carry_throw(BYTE *mag, BYTE *victim, BYTE *loco, BYTE *cell)
{
    Coord c = cell_center(cell);
    ((void (GTHISCALL *)(BYTE *, void *))VFUNC(mag, VT_SETTARGET))(mag, NULL);
    /* Move_To heads for the nearest free cell to it and sets IsMoving only when it finds one */
    char was_moving = FIELD(loco, J_ISMOVING, char);
    FIELD(loco, J_ISMOVING, char) = 0;
    ((moveto_fn)VFUNC(loco, VT_MOVETO))(loco, c);
    Coord *d = &FIELD(loco, J_DEST, Coord), *v = &FIELD(victim, O_LOCATION, Coord);
    logmsg("magnetron: throw to cell %d,%d, landing cell %d,%d", c.X >> 8, c.Y >> 8, d->X >> 8, d->Y >> 8);
    if (!FIELD(loco, J_ISMOVING, char)) {   /* none: keep carrying, the follow brings it back */
        FIELD(loco, J_ISMOVING, char) = was_moving;
        logmsg("magnetron: no landing cell, still carrying");
        return;
    }
    if (d->X == v->X && d->Y == v->Y) {     /* already over the landing cell: drop it now, like Stop */
        FIELD(loco, J_ISMOVING, char) = 0;
        ((release_fn)RELEASE_LOCOMOTOR)(mag, 1);
        return;
    }
    throw_record(victim, mag);
}

/* a human player's Magnetron with a consistent link, told to attack a cell: throw instead. Nonzero = thrown. */
static int carry_ground_attack(BYTE *mag, BYTE *target)
{
    BYTE *victim = FIELD(mag, O_LOCOTARGET, BYTE *), *loco;
    if (!victim || FIELD(victim, O_LOCOSOURCE, BYTE *) != mag || !carry_owner(mag) || !(loco = victim_jumpjet(victim))
        || !is_cell(target))
        return 0;
    carry_throw(mag, victim, loco, target);
    return 1;
}

/* replaces the calls to ReleaseLocomotor in SetDestination and EnterIdleMode */
static void GTHISCALL release_unless_carry(BYTE *self, char set_target)
{
    if (!carry_owner(self))
        ((release_fn)RELEASE_LOCOMOTOR)(self, set_target);
}

/* victim arrived where it was heading: nonzero = keep it hovering instead of dropping it. A thrown victim is over
 * its cell and drops. */
__attribute__((used)) int carry_keep_hovering(BYTE *victim)
{
    BYTE *mag = FIELD(victim, O_LOCOSOURCE, BYTE *);
    if (thrown(victim)) {
        throw_forget(throw_slot(victim));
        logmsg("magnetron: thrown vehicle over its cell, dropping");
        return 0;
    }
    return mag && carry_owner(mag);
}

__attribute__((used)) void carry_stop(BYTE *techno)
{
    BYTE *victim = FIELD(techno, O_LOCOTARGET, BYTE *);
    /* only a consistent link: the victim must point back at this unit */
    if (victim && FIELD(victim, O_LOCOSOURCE, BYTE *) == techno) {
        logmsg("magnetron: stop, dropping (unit vtable %08lX)", (unsigned long)FIELD(techno, 0, DWORD));
        int i = throw_slot(victim);
        if (i >= 0)
            throw_forget(i);
        ((release_fn)RELEASE_LOCOMOTOR)(techno, 1);
    }
}

/* at 0x54C1B3, replacing "mov dword [esi+0x80], 0"; ESI = JumpjetLocomotionClass, EAX = victim */
void arrive_stub(void);
/* at 0x4C7512, replacing "mov eax, [esi+0xAC]" */
void stop_stub(void);
/* at 0x762B9B, replacing "cmp [edi+0x2B4], ebx; jne 0x762C40"; EDI = the beam's owner, EBX = its target.
 * The Magnetron beam lasts only while the owner still targets its victim, and a move order clears the target.
 * Keep it while a human player's Magnetron carries that victim. */
void wave_stub(void);
__asm__(
    ".section .text\n"
    ".intel_syntax noprefix\n"
    "_arrive_stub:\n"
    "    mov dword ptr [esi+0x80], 0\n"
    "    push eax\n"
    "    push eax\n"
    "    call _carry_keep_hovering\n"
    "    add esp, 4\n"
    "    test eax, eax\n"
    "    pop eax\n"
    "    jnz 1f\n"
    "    push 0x54C1BD\n"           /* JJ_ARRIVED_STOCK: release and descend */
    "    ret\n"
    "1:  mov dword ptr [esi+0x50], 2\n"  /* hovering */
    "    mov byte ptr [esi+0x4C], 0\n"   /* not moving: the hovering state then just holds position */
    "    push 0x54C4FD\n"           /* JJ_STATE_DONE */
    "    ret\n"
    "_wave_stub:\n"
    "    cmp [edi+0x2B4], ebx\n"
    "    je 2f\n"
    "    cmp [edi+0x2AC], ebx\n"
    "    jne 3f\n"
    "    push eax\n"
    "    mov eax, [edi+0x21C]\n"   /* owner house */
    "    test eax, eax\n"
    "    jz 4f\n"
    "    cmp byte ptr [eax+0x1EC], 0\n"  /* IsHumanPlayer */
    "4:  pop eax\n"
    "    jnz 2f\n"
    "3:  push 0x762C40\n"          /* end the beam */
    "    ret\n"
    "2:  push 0x762BA7\n"          /* keep it */
    "    ret\n"
    "_stop_stub:\n"
    "    pushad\n"
    "    push esi\n"
    "    call _carry_stop\n"
    "    add esp, 4\n"
    "    popad\n"
    "    mov eax, [esi+0xAC]\n"
    "    ret\n"
    ".att_syntax prefix\n");

/* per frame, before UnitClass::AI */
static void carry_update(BYTE *unit)
{
    BYTE *victim = FIELD(unit, O_LOCOTARGET, BYTE *), *loco;

    /* Every drop from the carry hover lands here (Stop, the Magnetron dying, ...): ReleaseLocomotor unlinks the
     * victim, but the Jumpjet Process (0x54AEC0) skips a locomotor that is Hovering and not moving, so neither the
     * fall nor the landing would run. Descending with IsMoving set is what the stock arrival drop leaves behind. */
    /* The same for AI Magnetrons' victims (stock pull and drop) and for victims a Magnetron let go of in other
     * ways: hovering or cruising in place on the Jumpjet locomotor a Magnetron gave them, with no Magnetron, and
     * no longer flagged as attacked by one. They hung in the air for good. Units that fly by design (JumpJet= or
     * BalloonHover= on the type: Rocketeers, Siege Choppers, Kirovs) are left alone. */
    BYTE *utype = FIELD(unit, U_TYPE, BYTE *);
    if (!FIELD(unit, O_LOCOSOURCE, BYTE *) && utype && !utype[TT_JUMPJET] && !utype[TT_BALLOONHOVER]
        && (loco = victim_jumpjet(unit)) && (FIELD(loco, J_STATE, int) == 2 || FIELD(loco, J_STATE, int) == 3)
        && !FIELD(loco, J_ISMOVING, char)) {
        static int last_log;
        if (CURRENT_FRAME - last_log > 300) {
            last_log = CURRENT_FRAME;
            logmsg("magnetron: a %s left hanging in the air drops (state %d)", (char *)utype + 0x24, FIELD(loco, J_STATE, int));
        }
        FIELD(loco, J_STATE, int) = 4;
        FIELD(loco, J_ISMOVING, char) = 1;
    }

    for (int i = 0; i < MAX_THROWS; i++)   /* throw records whose link is gone */
        if ((throws[i].mag == unit && victim != throws[i].victim)
            || (throws[i].victim == unit && FIELD(unit, O_LOCOSOURCE, BYTE *) != throws[i].mag))
            throw_forget(i);

    if (!victim || !carry_owner(unit) || !(loco = victim_jumpjet(victim)))
        return;
    /* Attack-ground in range: throw now. The attack mission would fire only between MinimumRange and Range, and first
     * drives off to full range from a cell inside MinimumRange. Beyond Range it approaches, then this or unit_fire
     * throws. */
    BYTE *target = FIELD(unit, O_TARGET, BYTE *);
    if (is_cell(target) && cell_in_range(unit, target) && carry_ground_attack(unit, target))
        return;
    if (thrown(victim))
        return;
    if ((CURRENT_FRAME + ((DWORD)unit >> 4)) % 8)
        return;
    /* follow: when the Magnetron is over 1.5 cells from where the victim is heading, head for the Magnetron
     * again. Move_To picks the nearest free cell to it. */
    Coord *m = &FIELD(unit, O_LOCATION, Coord), *d = &FIELD(loco, J_DEST, Coord), *v = &FIELD(victim, O_LOCATION, Coord);
    int dx = m->X - d->X, dy = m->Y - d->Y;
    if (dx * dx + dy * dy <= 384 * 384)
        return;
    ((moveto_fn)FIELD(FIELD(loco, 0, BYTE *), 0x44, void *))(loco, *m);
    /* if that is the cell it already hovers over, the hovering state would make it land: stay put instead */
    if (d->X == v->X && d->Y == v->Y)
        FIELD(loco, J_ISMOVING, char) = 0;
}

static void combat_siege_update(BYTE *unit);
static int combat_siege_candidate(BYTE *unit);
static int combat_unit_exists(BYTE *unit);

static void GTHISCALL unit_ai(BYTE *self)
{
    carry_update(self);
    int siege_tick = (CURRENT_FRAME & 7) == 0 && combat_siege_candidate(self);
    ((void (GTHISCALL *)(BYTE *))UNIT_AI)(self);
    /* Unit::AI can remove a dying object. Check membership before reading it. */
    if (siege_tick && combat_unit_exists(self))
        combat_siege_update(self);
}

/* The stock range check adds bonuses (elevation, ...) to Range, so the attack mission can fire at a cell that
 * cell_in_range does not count as in range yet. Firing the Magnetron beam at it would drop the victim where it is
 * (the bullet releases the old LocomotorTarget, 0x4695EB): throw instead. The caller (0x736F6D) ignores the result. */
static void *GTHISCALL unit_fire(BYTE *self, BYTE *target, int weapon)
{
    if (carry_ground_attack(self, target))
        return NULL;
    return ((fire_fn)UNIT_FIRE)(self, target, weapon);
}

/* patch only when the bytes are the expected stock ones */
static int patch_checked(const char *what, DWORD addr, const BYTE *expect, size_t n)
{
    if (memcmp((void *)addr, expect, n) == 0)
        return 1;
    logmsg("%s: unexpected bytes at %08lX, not patched", what, (unsigned long)addr);
    return 0;
}

static void call_bytes(BYTE out[5], DWORD addr, DWORD target)
{
    DWORD rel = target - (addr + 5);
    out[0] = 0xE8;
    memcpy(out + 1, &rel, 4);
}

static void patch_magnetron(void)
{
    BYTE b[5];
    int ok = 0;
    call_bytes(b, CALL_RELEASE_SETDEST, RELEASE_LOCOMOTOR);
    if (patch_checked("release on destination", CALL_RELEASE_SETDEST, b, 5)) {
        patch_rel(CALL_RELEASE_SETDEST, 0xE8, (DWORD)release_unless_carry);
        ok++;
    }
    call_bytes(b, CALL_RELEASE_IDLE, RELEASE_LOCOMOTOR);
    if (patch_checked("release on idle", CALL_RELEASE_IDLE, b, 5)) {
        patch_rel(CALL_RELEASE_IDLE, 0xE8, (DWORD)release_unless_carry);
        ok++;
    }
    if (patch_checked("jumpjet arrival", JJ_ARRIVED, (const BYTE[]){ 0xC7, 0x86, 0x80, 0, 0, 0, 0, 0, 0, 0 }, 10)) {
        patch(JJ_ARRIVED + 5, (const BYTE[]){ 0x90, 0x90, 0x90, 0x90, 0x90 }, 5);
        patch_rel(JJ_ARRIVED, 0xE9, (DWORD)arrive_stub);
        ok++;
    }
    if (patch_checked("stop event", STOP_EVENT, (const BYTE[]){ 0x8B, 0x86, 0xAC, 0, 0, 0 }, 6)) {
        patch(STOP_EVENT + 5, (const BYTE[]){ 0x90 }, 1);
        patch_rel(STOP_EVENT, 0xE8, (DWORD)stop_stub);
        ok++;
    }
    if (patch_checked("magnetron beam", WAVE_KEEP,
                      (const BYTE[]){ 0x39, 0x9F, 0xB4, 0x02, 0, 0, 0x0F, 0x85, 0x99, 0, 0, 0 }, 12)) {
        patch(WAVE_KEEP + 5, (const BYTE[]){ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, 7);
        patch_rel(WAVE_KEEP, 0xE9, (DWORD)wave_stub);
        ok++;
    }
    DWORD ai = UNIT_AI, fn = (DWORD)unit_ai;
    if (patch_checked("unit AI vtable slot", UNIT_AI_SLOT, (const BYTE *)&ai, 4)) {
        patch(UNIT_AI_SLOT, (const BYTE *)&fn, 4);
        ok++;
    }
    DWORD fire = UNIT_FIRE;
    fn = (DWORD)unit_fire;
    if (patch_checked("unit Fire vtable slot", UNIT_FIRE_SLOT, (const BYTE *)&fire, 4)) {
        patch(UNIT_FIRE_SLOT, (const BYTE *)&fn, 4);
        ok++;
    }
    logmsg("magnetron carry: %d of 7 patches applied", ok);
}

/* ---- test units ----
 * Optional [Units] section in yspawn.ini, read once the scenario is loaded: n=TYPE,COUNTRY,X,Y,FACING,MISSION puts a
 * vehicle or a building (a UnitTypeClass or BuildingTypeClass ID such as ATTNK or GAWEAP) on map cell X,Y for the
 * house playing COUNTRY (a HouseTypeClass ID such as Americans). FACING is 0-255 (0 north, 64 east); MISSION is Sleep
 * (never fires), Guard, Area_Guard, Hunt or a number, and is ignored for buildings. A building goes on the nearest
 * cell to X,Y (its top-left corner) where the game's own placement check lets it stand. For screenshots and tests: vehicles in the map's own [Units] did not show up for the player. Vtable slots follow the YRpp
 * declaration order and were checked against this exe (CreateObject 0x747560 calls UnitClass::UnitClass 0x7353C0). */
#define UNITTYPE_ARRAY    ((DynVec *)0xA83CE0)
#define BUILDINGTYPE_ARRAY ((DynVec *)0xA83C68)
#define BTYPE_CAN_PLACE   0x464AC0   /* BuildingTypeClass::CanPlaceHere(CellStruct*, HouseClass*) */
#define HOUSE_ARRAY       ((DynVec *)0xA80228)
#define MAP_INSTANCE      ((void *)0x87F7E8)
#define MAP_FLOOR_HEIGHT  0x578080   /* MapClass::GetCellFloorHeight(const CoordStruct&) */
#define H_TYPE            0x034      /* HouseClass: HouseTypeClass* */
#define T_ID              0x024      /* AbstractTypeClass: char ID[0x18] */
#define VT_CREATEOBJECT   0x08C      /* ObjectTypeClass::CreateObject(HouseClass*) */
#define VT_UNLIMBO        0x0D8      /* ObjectClass::Unlimbo(const CoordStruct&, DirType) */
#define VT_QUEUEMISSION   0x1E8      /* MissionClass::QueueMission(Mission, bool) */

static BYTE *find_type(DynVec *v, const char *id)
{
    for (int i = 0; i < v->Count; i++)
        if (!_stricmp((char *)v->Items[i] + T_ID, id))
            return v->Items[i];
    return NULL;
}

static BYTE *find_house(const char *country)
{
    DynVec *v = HOUSE_ARRAY;
    for (int i = 0; i < v->Count; i++) {
        BYTE *type = FIELD(v->Items[i], H_TYPE, BYTE *);
        if (type && !_stricmp((char *)type + T_ID, country))
            return v->Items[i];
    }
    return NULL;
}

typedef struct { short X, Y; } CellXY;
static int dir_height(CellXY c);
static int dir_blocks_passage(BYTE *type, CellXY tl);

/* the nearest top-left cell to *x,*y, in growing squares, where the building can be placed */
static int building_spot(BYTE *type, BYTE *house, int *x, int *y)
{
    for (int r = 0; r <= 12; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;
                CellXY c = { *x + dx, *y + dy };
                if (((char (GTHISCALL *)(BYTE *, CellXY *, BYTE *))BTYPE_CAN_PLACE)(type, &c, house)) {
                    *x = c.X;
                    *y = c.Y;
                    return 1;
                }
            }
    return 0;
}

/* a new vehicle or building of TYPE for HOUSE on cell x,y (a building's top-left cell); NULL if it could not be put there */
static BYTE *put_object(BYTE *type, BYTE *house, int x, int y, int facing)
{
    BYTE *obj = ((BYTE *(GTHISCALL *)(BYTE *, BYTE *))VFUNC(type, VT_CREATEOBJECT))(type, house);
    Coord c = { x * 256 + 128, y * 256 + 128, 0 };
    c.Z = ((int (GTHISCALL *)(void *, Coord *))MAP_FLOOR_HEIGHT)(MAP_INSTANCE, &c);
    return obj && ((char (GTHISCALL *)(BYTE *, Coord *, int))VFUNC(obj, VT_UNLIMBO))(obj, &c, facing & 0xFF) ? obj : NULL;
}

static int mission_number(const char *name)
{
    static const struct { const char *name; int value; } names[] = {
        { "Sleep", 0 }, { "Guard", 5 }, { "Area_Guard", 11 }, { "Hunt", 15 }, { "Harmless", 23 },
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!_stricmp(name, names[i].name))
            return names[i].value;
    return atoi(name);
}

static void spawn_units(void)
{
    static char buf[8192];
    if (!GetPrivateProfileSectionA("Units", buf, sizeof buf, INI))
        return;
    for (char *line = buf; *line; line += strlen(line) + 1) {
        char *value = strchr(line, '=');
        char type_id[32], country[32], mission[32];
        int x, y, facing;
        if (!value || sscanf(value + 1, "%31[^,],%31[^,],%d,%d,%d,%31s", type_id, country, &x, &y, &facing, mission) != 6) {
            logmsg("units: bad line '%s'", line);
            continue;
        }
        BYTE *type = find_type(UNITTYPE_ARRAY, type_id), *house = find_house(country);
        if (!type)
            type = find_type((DynVec *)0xA8B218, type_id);   /* AircraftTypeClass::Array (Kirovs etc.) */
        int building = !type && (type = find_type(BUILDINGTYPE_ARRAY, type_id));
        if (!type || !house) {
            logmsg("units: %s: no %s", line, type ? "house" : "vehicle or building type");
            continue;
        }
        if (building && !building_spot(type, house, &x, &y)) {
            logmsg("units: %s: no room for the building near %d,%d", type_id, x, y);
            continue;
        }
        BYTE *obj = put_object(type, house, x, y, facing);
        if (obj && !building)
            ((char (GTHISCALL *)(BYTE *, int, char))VFUNC(obj, VT_QUEUEMISSION))(obj, mission_number(mission), 0);
        logmsg("units: %s %s at %d,%d facing %d %s: %s", type_id, country, x, y, facing, mission,
               obj ? "placed" : "could not be placed");
    }
}

/* ---- starting bases ----
 * Optional [StartBase] section in yspawn.ini (skirmish.py writes it), read once the scenario is loaded, before [Units]:
 * COUNTRY=ID,ID,... gives every house of that country (a HouseTypeClass ID such as Americans) those buildings instead
 * of its MCV, and Remove=ID,ID,... names the vehicles taken away (the [General] BaseUnit MCVs). The first building,
 * the Construction Yard, goes where the house starts: its BaseSpawnCell (HouseClass +0x5490; FindBuildLocation 0x5060B0
 * falls back to it when BaseCenter +0x5494 is empty), which is where the game put its MCV. The others go in order on
 * the nearest spots around it where the game's own placement check lets them stand, each with a free cell on every
 * side, so units can leave the factories and harvesters reach the refinery.
 * A computer player also gets what UnitClass::TryToDeploy does when its MCV becomes a Construction Yard in a skirmish
 * (0x739855-0x739926); without it the AI never starts producing. Its base plan (HouseClass Base.Nodes) then has a node
 * for each building it means to have, and the ones it has built carry their top-left cell and Placed; the starting
 * buildings are marked on it the same way, so it does not build them again, and flagged like the buildings its
 * factory makes (0x4C9DC5). */
#define UNIT_ARRAY        ((DynVec *)0x8B4108)
#define U_TYPE            0x6C4      /* UnitClass: UnitTypeClass* */
#define B_TYPE            0x520      /* BuildingClass: BuildingTypeClass* */
#define B_CONSTRUCTIONYARD 0x16B9    /* BuildingTypeClass: bool ConstructionYard (the deploy checks it) */
#define H_BASESPAWNCELL   0x5490     /* HouseClass: CellStruct */
#define BTYPE_WIDTH       0x45EC90   /* BuildingTypeClass::GetFoundationWidth() */
#define BTYPE_HEIGHT      0x45ECA0   /* BuildingTypeClass::GetFoundationHeight(bool with the bib) */
#define VT_LIMBO          0x0D4      /* ObjectClass::Limbo: off the map */
#define VT_UNINIT         0x0F8      /* ObjectClass::UnInit: limbo now, delete (and untrack) at the end of the frame */
#define HOUSE_IS_HUMAN    0x50B730   /* HouseClass::IsControlledByHuman() */
#define HOUSE_PLAN_BASE   0x505180   /* HouseClass: makes the base plan (Base.Nodes) if it has none */
#define HOUSE_BASE_READY  0x50C920   /* HouseClass: the last step of the deploy's AI set-up */
#define H_PRODUCTION      0x1EE      /* HouseClass: bool, AI production has begun */
#define H_AITRIGGERS      0x1F2      /* HouseClass: bool AITriggersActive */
#define H_AUTOBASE        0x1F3      /* HouseClass: bool AutoBaseBuilding */
#define H_NODES           0x5708     /* HouseClass: Base.Nodes items, BaseNodeClass[16 bytes: type index, cell, Placed] */
#define H_NODECOUNT       0x5714
#define H_BASE_CENTER     0x5750     /* HouseClass: Base.Center */
#define B_AI_BUILT        0x6CA      /* BuildingClass: set on what a computer player's factory makes */

typedef struct { int x, y, w, h; } Rect;
static Rect base_rects[128];
static int base_count;

static int in_list(const char *csv, const char *id)
{
    size_t n = strlen(id);
    for (const char *p = csv; *p; p += strcspn(p, ",")) {
        p += strspn(p, ", ");
        if (!_strnicmp(p, id, n) && (p[n] == ',' || p[n] == ' ' || !p[n]))
            return 1;
    }
    return 0;
}

/* no starting-base building within a cell of the rectangle */
static int base_clear(int x, int y, int w, int h)
{
    for (int i = 0; i < base_count; i++) {
        Rect *r = &base_rects[i];
        if (x - 1 < r->x + r->w && r->x < x + w + 1 && y - 1 < r->y + r->h && r->y < y + h + 1)
            return 0;
    }
    return 1;
}

/* the top-left cell for a building centred as near as it can be to cx,cy: the closest in the first square ring around
 * it that has a spot the placement check accepts and base_clear leaves free. The first pass also keeps to the start
 * cell's level and out of passages (dir_blocks_passage: ramps, gaps between cliffs): a War Factory put at the foot of
 * the ramp below its base jammed new units going up against the army coming down. */
static int base_spot(BYTE *type, BYTE *house, int cx, int cy, int *x, int *y)
{
    int w = ((int (GTHISCALL *)(BYTE *))BTYPE_WIDTH)(type);
    int h = ((int (GTHISCALL *)(BYTE *, char))BTYPE_HEIGHT)(type, 1);
    int level = dir_height((CellXY){ (short)cx, (short)cy });
    for (int strict = 1; strict >= 0; strict--)
    for (int r = 0; r <= (strict ? 14 : 24); r++) {
        int best = -1;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;
                CellXY c = { cx - w / 2 + dx, cy - h / 2 + dy };
                int d = dx * dx + dy * dy;
                if ((best < 0 || d < best) && base_clear(c.X, c.Y, w, h)
                    && ((char (GTHISCALL *)(BYTE *, CellXY *, BYTE *))BTYPE_CAN_PLACE)(type, &c, house)
                    && (!strict || (abs(dir_height(c) - level) <= 52 && !dir_blocks_passage(type, c)))) {
                    best = d;
                    *x = c.X;
                    *y = c.Y;
                }
            }
        if (best >= 0) {
            if (base_count < (int)(sizeof base_rects / sizeof *base_rects))
                base_rects[base_count++] = (Rect){ *x, *y, w, h };
            return 1;
        }
    }
    return 0;
}

/* the index in BuildingTypeClass::Array, as base nodes store it */
static int building_type_index(BYTE *type)
{
    DynVec *v = BUILDINGTYPE_ARRAY;
    for (int i = 0; i < v->Count; i++)
        if (v->Items[i] == type)
            return i;
    return -1;
}

static CellXY object_cell(BYTE *obj)
{
    Coord *c = &FIELD(obj, O_LOCATION, Coord);
    return (CellXY){ c->X >> 8, c->Y >> 8 };
}

/* a computer player's first building is its Construction Yard: start its AI as the MCV deploy would, then put the
 * other buildings on its base plan */
static void start_ai_base(BYTE *house, BYTE **placed, int n)
{
    CellXY cell = object_cell(placed[0]);
    FIELD(house, H_BASESPAWNCELL, CellXY) = cell;     /* all that 0x50E000 does */
    ((void (GTHISCALL *)(BYTE *))HOUSE_PLAN_BASE)(house);
    BYTE *nodes = FIELD(house, H_NODES, BYTE *);
    int count = FIELD(house, H_NODECOUNT, int);
    if (count > 0)
        FIELD(nodes, 4, CellXY) = cell;               /* node 0 is the Construction Yard */
    FIELD(house, H_BASE_CENTER, CellXY) = cell;
    FIELD(house, H_PRODUCTION, char) = 1;
    FIELD(house, H_AITRIGGERS, char) = 1;
    FIELD(house, H_AUTOBASE, char) = 1;
    ((void (GTHISCALL *)(BYTE *))HOUSE_BASE_READY)(house);
    int marked = 0;
    for (int k = 0; k < n; k++) {
        FIELD(placed[k], B_AI_BUILT, char) = 1;
        if (!k)
            continue;
        int index = building_type_index(FIELD(placed[k], B_TYPE, BYTE *));
        for (int j = 1; j < count; j++) {
            BYTE *node = nodes + 16 * j;
            if (FIELD(node, 0, int) == index && !FIELD(node, 8, char) && !FIELD(node, 4, DWORD)) {
                FIELD(node, 4, CellXY) = object_cell(placed[k]);
                FIELD(node, 8, char) = 1;
                marked++;
                break;
            }
        }
    }
    logmsg("start base: computer player started at %d,%d; %d base plan nodes, %d of %d buildings marked on it",
           cell.X, cell.Y, count, marked, n - 1);
}

static void start_bases(void)
{
    char remove[256], list[1024];
    if (!GetPrivateProfileSectionA("StartBase", list, sizeof list, INI))
        return;
    ini_str("StartBase", "Remove", "", remove, sizeof remove);
    DynVec *hv = HOUSE_ARRAY;
    for (int i = 0; i < hv->Count; i++) {
        BYTE *house = hv->Items[i], *htype = FIELD(house, H_TYPE, BYTE *);
        const char *country = htype ? (char *)htype + T_ID : "";
        ini_str("StartBase", country, "", list, sizeof list);
        if (!*country || !*list)
            continue;
        CellXY start = FIELD(house, H_BASESPAWNCELL, CellXY);
        logmsg("start base: house %d %s, start cell %d,%d", i, country, start.X, start.Y);
        if (start.X <= 0 || start.Y <= 0)
            continue;
        /* off the map while the base goes down, so the Construction Yard can take their cells */
        BYTE *mcvs[8];
        Coord at[8];
        int m = 0;
        DynVec *uv = UNIT_ARRAY;
        for (int k = 0; k < uv->Count && m < 8; k++) {
            BYTE *unit = uv->Items[k], *utype = FIELD(unit, U_TYPE, BYTE *);
            if (FIELD(unit, O_OWNER, BYTE *) == house && utype && in_list(remove, (char *)utype + T_ID)) {
                at[m] = FIELD(unit, O_LOCATION, Coord);
                ((char (GTHISCALL *)(BYTE *))VFUNC(unit, VT_LIMBO))(unit);
                mcvs[m++] = unit;
            }
        }
        BYTE *placed[32];
        int n = 0;
        for (char *id = strtok(list, ", "); id; id = strtok(NULL, ", ")) {
            BYTE *type = find_type(BUILDINGTYPE_ARRAY, id), *obj = NULL;
            int x, y;
            if (!type)
                logmsg("start base: %s: no such building", id);
            else if (!base_spot(type, house, start.X, start.Y, &x, &y))
                logmsg("start base: %s: no room near %d,%d", id, start.X, start.Y);
            else
                logmsg("start base: %s at %d,%d (height %+d): %s", id, x, y,
                       dir_height((CellXY){ (short)x, (short)y }) - dir_height(start),
                       (obj = put_object(type, house, x, y, 0)) ? "placed" : "could not be placed");
            if (obj && n < (int)(sizeof placed / sizeof *placed) && (n || FIELD(obj, B_TYPE, BYTE *)[B_CONSTRUCTIONYARD]))
                placed[n++] = obj;
            if (!n)
                break;   /* no Construction Yard: no base */
        }
        for (int k = 0; k < m; k++)   /* UnInit deletes it at the end of the frame; Unlimbo puts it back */
            if (n)
                ((void (GTHISCALL *)(BYTE *))VFUNC(mcvs[k], VT_UNINIT))(mcvs[k]);
            else
                ((char (GTHISCALL *)(BYTE *, Coord *, int))VFUNC(mcvs[k], VT_UNLIMBO))(mcvs[k], &at[k], 0);
        logmsg("start base: %d vehicles %s", m, n ? "removed" : "put back, as the Construction Yard found no room");
        if (n && !((char (GTHISCALL *)(BYTE *))HOUSE_IS_HUMAN)(house))
            start_ai_base(house, placed, n);
    }
}

static void combat_queue_expansion(BYTE *house);
static void team_telemetry_sample(BYTE *house);
static void bench_sample(void);
static void observer_reveal(void);
static void dir_observer_report(void);
static int director_enabled(BYTE *house);
static int dir_factory_cash_pct(BYTE *house);
static int dir_fallback_spot(BYTE *house, BYTE *type, CellXY *out);
static void dir_update(BYTE *house);
#include "oil-defenses.h"
#include "bench.h"
#include "combat-ai.h"
#include "team-telemetry.h"
#include "director.h"
#include "human-peace.h"

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
    patch_magnetron();
    patch_oil_defenses();
    patch_director();
    team_telemetry_init();
    bench_init();
    human_in_peace = ini_int("Settings", "HumanInPeace", 0) != 0;
    if (human_in_peace)
        peace_ready = patch_human_peace();
    logmsg("patched");
    return TRUE;
}
