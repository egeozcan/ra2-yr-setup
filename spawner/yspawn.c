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

/* ---- Magnetron carry ----
 * Stock: the Magnetron's IsLocomotor warhead gives the victim a Jumpjet locomotor (ImbueLocomotor 0x710000,
 * magnetron+0x2AC LocomotorTarget = victim, victim+0x2B0 LocomotorSource = magnetron, victim+0x6AD
 * IsAttackedByLocomotor) and sends it towards the Magnetron. ReleaseLocomotor (0x70FEE0) drops it: in the air it
 * falls and takes damage. The stock drops happen when the Magnetron gets a destination (FootClass::SetDestination),
 * goes idle (UnitClass::EnterIdleMode), or the victim arrives (Jumpjet cruising state).
 * For a human player's Magnetron those drops are skipped: the victim hovers next to it and follows it around, and
 * the Stop command (S) drops it. Computer players keep the stock behaviour. Death, transports, chrono, a new
 * Magnetron target and the like still drop it through the untouched stock paths. */
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
#define JUMPJET_ILOCO_VT    0x7ECD68   /* JumpjetLocomotionClass ILocomotion vtable */
#define CURRENT_FRAME       (*(int *)0xA8ED84)

/* object offsets (YRpp, checked against this exe) */
#define O_OWNER       0x21C   /* TechnoClass: HouseClass* */
#define O_LOCOTARGET  0x2AC   /* TechnoClass: FootClass* the victim */
#define O_LOCOSOURCE  0x2B0   /* TechnoClass: FootClass* the Magnetron */
#define O_LOCATION    0x09C   /* AbstractClass: CoordStruct */
#define O_LOCOMOTOR   0x674   /* FootClass: ILocomotion* */
#define O_ATTACKEDBYLOCO 0x6AD
#define H_ISHUMAN     0x1EC   /* HouseClass */
/* JumpjetLocomotionClass, from its ILocomotion pointer (= object + 4) */
#define J_DEST        0x3C    /* CoordStruct DestinationCoords */
#define J_ISMOVING    0x48
#define J_STATE       0x4C    /* 2 hovering, 3 cruising, 4 descending */

typedef struct { int X, Y, Z; } Coord;
typedef void (GTHISCALL *release_fn)(BYTE *, char);
typedef void (__stdcall *moveto_fn)(void *, Coord);
#define FIELD(p, off, type) (*(type *)((BYTE *)(p) + (off)))

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

/* replaces the calls to ReleaseLocomotor in SetDestination and EnterIdleMode */
static void GTHISCALL release_unless_carry(BYTE *self, char set_target)
{
    if (!carry_owner(self))
        ((release_fn)RELEASE_LOCOMOTOR)(self, set_target);
}

/* victim arrived next to the Magnetron: nonzero = keep it hovering instead of dropping it */
__attribute__((used)) int carry_keep_hovering(BYTE *victim)
{
    BYTE *mag = FIELD(victim, O_LOCOSOURCE, BYTE *);
    return mag && carry_owner(mag);
}

__attribute__((used)) void carry_stop(BYTE *techno)
{
    BYTE *victim = FIELD(techno, O_LOCOTARGET, BYTE *);
    /* only a consistent link: the victim must point back at this unit */
    if (victim && FIELD(victim, O_LOCOSOURCE, BYTE *) == techno) {
        logmsg("magnetron: stop, dropping (unit vtable %08lX)", (unsigned long)FIELD(techno, 0, DWORD));
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
    if (FIELD(unit, O_ATTACKEDBYLOCO, char) && !FIELD(unit, O_LOCOSOURCE, BYTE *) && (loco = victim_jumpjet(unit))
        && FIELD(loco, J_STATE, int) == 2 && !FIELD(loco, J_ISMOVING, char)) {
        FIELD(loco, J_STATE, int) = 4;
        FIELD(loco, J_ISMOVING, char) = 1;
    }

    if (!victim || !carry_owner(unit) || !(loco = victim_jumpjet(victim)))
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

static void GTHISCALL unit_ai(BYTE *self)
{
    carry_update(self);
    ((void (GTHISCALL *)(BYTE *))UNIT_AI)(self);
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
    logmsg("magnetron carry: %d of 6 patches applied", ok);
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
    patch_magnetron();
    logmsg("patched");
    return TRUE;
}
