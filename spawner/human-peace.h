/* Optional skirmish protection, without changing teams or alliance bitfields.
 * YRpp ABI, checked against gamemd.exe 1.001. Install every guard or refuse the
 * match: partial protection must never silently masquerade as this mode.
 */
#define PEACE_ENEMY 0x5600
#define PEACE_ANGER 0x5604
#define PEACE_TECHNOS ((DynVec *)0xA8EC78)
#define PEACE_SW_MARGIN 32 /* cells; covers stock nuke spread and storm scatter */

static int peace_human(BYTE *house)
{
    return house && (house[H_ISHUMAN] || house[0x1ED]);
}

static int peace_protected(BYTE *obj)
{
    /* Targets can also be cells, terrain, bullets, etc. Never read a Techno
     * owner from those smaller objects. */
    return obj && (FIELD(obj, 0x14, DWORD) & 1)
        && peace_human(FIELD(obj, O_OWNER, BYTE *));
}

static int peace_pair(BYTE *house, BYTE *other)
{
    return house && !peace_human(house) && peace_human(other);
}

static int peace_target(BYTE *self, BYTE *target)
{
    return self && !peace_human(FIELD(self, O_OWNER, BYTE *)) && peace_protected(target);
}

typedef void (GTHISCALL *peace_set_fn)(BYTE *, BYTE *);
typedef int (GTHISCALL *peace_damage_fn)(BYTE *, int *, int, BYTE *, BYTE *, char, char, BYTE *);
typedef char (GTHISCALL *peace_owner_fn)(BYTE *, BYTE *, char);
typedef int (GTHISCALL *peace_fire_error_fn)(BYTE *, BYTE *, int, char);
typedef void (GTHISCALL *peace_destination_fn)(BYTE *, BYTE *, char);
static void *peace_original[4][6];

/* Intercept at each class's vtable, before subclass damage/death effects,
 * including crushing, radiation, collateral explosions and unattributed
 * lightning. Repairs (negative damage) still run normally. */
#define PEACE_CLASS(n) \
static int GTHISCALL peace_damage_##n(BYTE *self, int *damage, int distance, BYTE *wh, BYTE *attacker, \
                                     char ignore, char escape, BYTE *house) \
{ \
    if (peace_protected(self) && damage && *damage >= 0) return 0; \
    return ((peace_damage_fn)peace_original[n][0])(self, damage, distance, wh, attacker, ignore, escape, house); \
} \
static void GTHISCALL peace_set_##n(BYTE *self, BYTE *target) \
{ \
    ((peace_set_fn)peace_original[n][1])(self, peace_target(self, target) ? NULL : target); \
} \
static void *GTHISCALL peace_fire_##n(BYTE *self, BYTE *target, int weapon) \
{ \
    if (peace_target(self, target)) return NULL; \
    return ((fire_fn)peace_original[n][2])(self, target, weapon); \
} \
static char GTHISCALL peace_owner_##n(BYTE *self, BYTE *house, char announce) \
{ \
    if (peace_protected(self) && house && !peace_human(house)) return 0; \
    return ((peace_owner_fn)peace_original[n][3])(self, house, announce); \
} \
static int GTHISCALL peace_error_##n(BYTE *self, BYTE *target, int weapon, char ignore) \
{ \
    if (peace_target(self, target)) return 5; /* FireError::ILLEGAL */ \
    return ((peace_fire_error_fn)peace_original[n][4])(self, target, weapon, ignore); \
} \
static void GTHISCALL peace_destination_##n(BYTE *self, BYTE *target, char unknown) \
{ \
    ((peace_destination_fn)peace_original[n][5])(self, peace_target(self, target) ? NULL : target, unknown); \
}
PEACE_CLASS(0) /* aircraft */
PEACE_CLASS(1) /* buildings */
PEACE_CLASS(2) /* infantry */
PEACE_CLASS(3) /* vehicles, including ships */
#undef PEACE_CLASS

static peace_set_fn peace_set_original;
static void GTHISCALL peace_set_common(BYTE *self, BYTE *target)
{
    peace_set_original(self, peace_target(self, target) ? NULL : target);
}

static peace_damage_fn peace_damage_original;
static int GTHISCALL peace_damage_common(BYTE *self, int *damage, int distance, BYTE *wh, BYTE *attacker,
                                         char ignore, char escape, BYTE *house)
{
    if (peace_protected(self) && damage && *damage >= 0)
        return 0;
    return peace_damage_original(self, damage, distance, wh, attacker, ignore, escape, house);
}

typedef char (GTHISCALL *peace_eval_fn)(BYTE *, int, int, int, BYTE *, int *, DWORD, Coord *);
static peace_eval_fn peace_eval_original;
static char GTHISCALL peace_evaluate(BYTE *self, int flags, int types, int distance, BYTE *target,
                                    int *threat, DWORD unknown, Coord *source)
{
    if (peace_target(self, target)) {
        if (threat) *threat = 0;
        return 0;
    }
    return peace_eval_original(self, flags, types, distance, target, threat, unknown, source);
}

typedef void (GTHISCALL *peace_anger_fn)(BYTE *, int, BYTE *);
static peace_anger_fn peace_anger_original;
static void GTHISCALL peace_anger(BYTE *house, int amount, BYTE *other)
{
    if (!peace_human(house)) {
        DynVec *nodes = (DynVec *)(house + PEACE_ANGER);
        for (int i = 0; i < nodes->Count; i++) {
            BYTE *node = (BYTE *)nodes->Items + i * 8;
            if (peace_human(FIELD(node, 0, BYTE *)))
                FIELD(node, 4, int) = 0;
        }
        if (peace_pair(house, other)) amount = 0;
    }
    peace_anger_original(house, amount, other);
}

/* HouseClass::UpdateAI seeds anger with the nearest house without checking
 * alliances. Skip human candidates there as well as in later anger updates. */
static int __attribute__((cdecl, used)) peace_skip_house(BYTE *house, BYTE *candidate)
{
    return peace_pair(house, candidate);
}
static void __attribute__((naked)) peace_house_candidate(void)
{
    __asm__ volatile (
        "pushal\n\tpushl %esi\n\tpushl %ebx\n\tcall _peace_skip_house\n\taddl $8, %esp\n\t"
        "testl %eax, %eax\n\tpopal\n\tjnz 1f\n\t"
        "movl 0x34(%esi), %edx\n\tmovb 0x1a6(%edx), %al\n\t"
        "pushl $0x4fd61f\n\tret\n\t1: pushl $0x4fd6fe\n\tret\n\t");
}

/* Stock SW selectors index HouseClass::Array with EnemyHouseIndex directly.
 * With only a human left there is no legal enemy; do not let them index -1.
 * With other AIs present, choose a real hostile AI if the old enemy is human. */
typedef void (GTHISCALL *peace_ai_sw_fn)(BYTE *);
static peace_ai_sw_fn peace_ai_sw_original;
static void GTHISCALL peace_ai_supers(BYTE *house)
{
    if (!peace_human(house)) {
        DynVec *v = HOUSE_ARRAY;
        int idx = FIELD(house, PEACE_ENEMY, int);
        if (idx < 0 || idx >= v->Count || peace_human(v->Items[idx]) || ((BYTE *)v->Items[idx])[0x1F5]) {
            idx = -1;
            for (int i = 0; i < v->Count; i++) {
                BYTE *other = v->Items[i];
                if (other != house && !peace_human(other) && !other[0x1F5]
                    && !FIELD(FIELD(other, H_TYPE, BYTE *), 0x1A6, char)
                    && !((char (GTHISCALL *)(BYTE *, BYTE *))0x4F9A50)(house, other)) {
                    idx = i;
                    break;
                }
            }
            FIELD(house, PEACE_ENEMY, int) = idx;
        }
        if (idx < 0) return;
    }
    peace_ai_sw_original(house);
}

/* Guard the final launch, including mutation, domination and support powers.
 * A generous buffer prevents a strike on another AI beside a human base.
 * Per-object damage and ownership guards remain effective if a human unit
 * moves into a storm or delayed dominator after it was launched. */
typedef void (GTHISCALL *peace_sw_fn)(BYTE *, CellXY *, char);
static peace_sw_fn peace_sw_original;
static void GTHISCALL peace_super_launch(BYTE *super, CellXY *target, char is_player)
{
    BYTE *house = FIELD(super, 0x2C, BYTE *);
    if (house && !peace_human(house) && target) {
        DynVec *v = PEACE_TECHNOS;
        for (int i = 0; i < v->Count; i++) {
            BYTE *obj = v->Items[i];
            if (peace_protected(obj) && !obj[0x81] && FIELD(obj, 0x6C, int) > 0) {
                Coord at = FIELD(obj, O_LOCATION, Coord);
                if (abs(at.X / 256 - target->X) <= PEACE_SW_MARGIN
                    && abs(at.Y / 256 - target->Y) <= PEACE_SW_MARGIN)
                    return;
            }
        }
    }
    peace_sw_original(super, target, is_player);
}

typedef char (GTHISCALL *peace_capture_fn)(BYTE *, BYTE *);
static peace_capture_fn peace_capture_original, peace_can_capture_original;
static char GTHISCALL peace_capture(BYTE *manager, BYTE *target)
{
    if (peace_target(FIELD(manager, 0x48, BYTE *), target)) return 0;
    return peace_capture_original(manager, target);
}
static char GTHISCALL peace_can_capture(BYTE *manager, BYTE *target)
{
    if (peace_target(FIELD(manager, 0x48, BYTE *), target)) return 0;
    return peace_can_capture_original(manager, target);
}

static int __attribute__((cdecl, used)) peace_skip_dominator(BYTE *target)
{
    BYTE *house = *(BYTE **)0xA9FAC8; /* PsyDom::Owner */
    return house && !peace_human(house) && peace_protected(target);
}
static void __attribute__((naked)) peace_dominator_candidate(void)
{
    /* Skip before freeing old mind control or setting permanent-control flags,
     * even if a human unit enters the delayed effect after the launch. */
    __asm__ volatile (
        "pushal\n\tpushl %esi\n\tcall _peace_skip_dominator\n\taddl $4, %esp\n\t"
        "testl %eax, %eax\n\tpopal\n\tjnz 1f\n\t"
        "movl 0x2c0(%esi), %eax\n\tpushl $0x53b27c\n\tret\n\t"
        "1: pushl $0x53b364\n\tret\n\t");
}

static int patch_human_peace(void)
{
    static const struct {
        DWORD address;
        BYTE bytes[9];
        int size;
        void *replacement;
        void *original;
    } hooks[] = {
        {0x6FCDB0, {0x83,0xEC,0x0C,0x53,0x56}, 5, peace_set_common, &peace_set_original},
        {0x701900, {0x81,0xEC,0xB4,0,0,0}, 6, peace_damage_common, &peace_damage_original},
        {0x6F7CA0, {0x83,0xEC,0x2C,0x53,0x55}, 5, peace_evaluate, &peace_eval_original},
        {0x504790, {0x8B,0xC1,0x53,0x33,0xDB}, 5, peace_anger, &peace_anger_original},
        {0x5098F0, {0xA1,0x38,0xB2,0xA8,0}, 5, peace_ai_supers, &peace_ai_sw_original},
        {0x6CC390, {0x81,0xEC,0xD4,0x01,0,0}, 6, peace_super_launch, &peace_sw_original},
        {0x471C90, {0x53,0x56,0x8B,0x74,0x24,0x0C}, 6, peace_can_capture, &peace_can_capture_original},
        {0x471D40, {0x8B,0x44,0x24,0x04,0x83,0xEC,0x18}, 7, peace_capture, &peace_capture_original},
        {0x4FD616, {0x8B,0x56,0x34,0x8A,0x82,0xA6,0x01,0,0}, 9, peace_house_candidate, NULL},
        {0x53B276, {0x8B,0x86,0xC0,0x02,0,0}, 6, peace_dominator_candidate, NULL},
    };
    static const DWORD vtables[] = {0x7E22A4, 0x7E3EBC, 0x7EB058, 0x7F5C70};
    static const int slots[] = {0x16C, 0x3C8, 0x3CC, 0x3D4, 0x3C0, 0x480};
    DWORD expected[4][6] = {
        {0x4165C0, 0x6FCDB0, 0x415EE0, 0x4DBED0, 0x41A9E0, 0x41AA80},
        {0x442230, 0x443B90, 0x6FDD50, 0x448260, 0x447F10, 0x455D50},
        {0x517FA0, 0x51B1F0, 0x51DF60, 0x4DBED0, 0x51C8B0, 0x51AA40},
        {0x737C90, 0x6FCDB0, 0x741340, 0x7463A0, 0x740FD0, 0x741970},
    };
    void *wrappers[4][6] = {
        {peace_damage_0, peace_set_0, peace_fire_0, peace_owner_0, peace_error_0, peace_destination_0},
        {peace_damage_1, peace_set_1, peace_fire_1, peace_owner_1, peace_error_1, peace_destination_1},
        {peace_damage_2, peace_set_2, peace_fire_2, peace_owner_2, peace_error_2, peace_destination_2},
        {peace_damage_3, peace_set_3, peace_fire_3, peace_owner_3, peace_error_3, peace_destination_3},
    };
    /* The previously installed Magnetron wrapper must remain in the chain. */
    if (FIELD(UNIT_FIRE_SLOT, 0, DWORD) == (DWORD)unit_fire)
        expected[3][2] = (DWORD)unit_fire;
    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++)
        if (!patch_checked("human in peace", hooks[i].address, hooks[i].bytes, hooks[i].size))
            return 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 6; j++)
            if (!patch_checked("human in peace vtable", vtables[i] + slots[j], (BYTE *)&expected[i][j], 4))
                return 0;
    BYTE *tramp = VirtualAlloc(NULL, sizeof hooks / sizeof *hooks * 16, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        logmsg("human in peace: could not allocate trampolines");
        return 0;
    }
    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++) {
        memcpy(tramp + i * 16, hooks[i].bytes, hooks[i].size);
        patch_rel((DWORD)tramp + i * 16 + hooks[i].size, 0xE9, hooks[i].address + hooks[i].size);
        if (hooks[i].original)
            *(void **)hooks[i].original = tramp + i * 16;
    }
    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++)
        patch_rel(hooks[i].address, 0xE9, (DWORD)hooks[i].replacement);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 6; j++) {
            peace_original[i][j] = (void *)expected[i][j];
            patch(vtables[i] + slots[j], (BYTE *)&wrappers[i][j], 4);
        }
    logmsg("human in peace: all 34 guards applied; teams unchanged");
    return 1;
}
