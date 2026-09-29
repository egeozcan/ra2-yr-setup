/* Optional, read-only Brutal team assembly snapshots for live skirmish checks.
 * Offsets match YRpp TeamClass/TeamTypeClass/TaskForceClass and gamemd.exe 1.001
 * (notably TeamClass::AddMember at 0x6EA54D and missing members at 0x6EF4D0).
 * Sample from the existing HouseClass building-AI wrapper; never alter teams. */
#define TEAM_ARRAY ((DynVec *)0x8B40E8)
#define TEAM_SAMPLE_INTERVAL 150
#define TEAM_TYPE 0x24
#define TEAM_OWNER 0x2C
#define TEAM_CREATED 0x50
#define TEAM_FULL 0x79
#define TEAM_UNDER 0x7A
#define TEAM_FORCED 0x77
#define TEAM_COUNTS 0x88
#define TEAM_TASKFORCE 0xE4
#define FORCE_COUNT 0x9C
#define FORCE_ENTRIES 0xA4
#define TEAM_SCRIPT 0x28
#define SCRIPT_LINE 0x2C

static FILE *team_telemetry_file;
static int team_telemetry_next[32];

static void team_telemetry_init(void)
{
    if (!ini_int("Settings", "TeamTelemetry", 0))
        return;
    team_telemetry_file = fopen("yspawn-teams.csv", "w");
    if (!team_telemetry_file) {
        logmsg("team telemetry: could not open yspawn-teams.csv");
        return;
    }
    fputs("frame,house,kind,teams,large_teams,team_id,created_frame,age,wanted,present,missing,full,under,forced,script_line,cash,composition\n",
          team_telemetry_file);
    fflush(team_telemetry_file);
    logmsg("team telemetry: enabled (150-frame snapshots to yspawn-teams.csv)");
}

static void team_telemetry_sample(BYTE *house)
{
    if (!team_telemetry_file || !house || SESSION->GameMode != 5
        || house[H_ISHUMAN] || house[0x1ED] || house[OIL_H_DEFEATED]
        || FIELD(house, OIL_H_DIFFICULTY, int) != 0)
        return;
    int idx = FIELD(house, 0x30, int), frame = CURRENT_FRAME;
    if (idx < 0 || idx >= 32 || frame < team_telemetry_next[idx])
        return;
    team_telemetry_next[idx] = frame + TEAM_SAMPLE_INTERVAL;

    DynVec *v = TEAM_ARRAY;
    if (!v->Items || v->Count < 0 || v->Count > 512)
        return;
    int teams = 0, large = 0, cash = FIELD(house, OIL_H_CASH, int);
    for (int i = 0; i < v->Count; i++) {
        BYTE *team = v->Items[i];
        if (!team || FIELD(team, TEAM_OWNER, BYTE *) != house)
            continue;
        teams++;
        BYTE *type = FIELD(team, TEAM_TYPE, BYTE *);
        BYTE *force = type ? FIELD(type, TEAM_TASKFORCE, BYTE *) : NULL;
        if (!force)
            continue;
        int entries = FIELD(force, FORCE_COUNT, int), wanted = 0, present = 0;
        if (entries < 0 || entries > 6)
            continue;
        char composition[256] = "";
        size_t used = 0;
        for (int j = 0; j < entries; j++) {
            BYTE *entry = force + FORCE_ENTRIES + j * 8;
            BYTE *member_type = FIELD(entry, 4, BYTE *);
            int need = FIELD(entry, 0, int), have = FIELD(team, TEAM_COUNTS + j * 4, int);
            if (!member_type || need <= 0)
                continue;
            wanted += need;
            present += have;
            used += snprintf(composition + used, sizeof composition - used, "%s%.24s:%d/%d",
                             used ? ";" : "", (char *)member_type + T_ID, have, need);
            if (used >= sizeof composition) {
                composition[sizeof composition - 1] = 0;
                break;
            }
        }
        if (wanted < 8)
            continue;
        large++;
        BYTE *script = FIELD(team, TEAM_SCRIPT, BYTE *);
        fprintf(team_telemetry_file, "%d,%d,team,,,%.*s,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s\n",
                frame, idx, 24, (char *)type + T_ID, FIELD(team, TEAM_CREATED, unsigned),
                frame - FIELD(team, TEAM_CREATED, int), wanted, present, wanted - present,
                team[TEAM_FULL] != 0, team[TEAM_UNDER] != 0, team[TEAM_FORCED] != 0,
                script ? FIELD(script, SCRIPT_LINE, int) : -1, cash, composition);
    }
    fprintf(team_telemetry_file, "%d,%d,summary,%d,%d,,,,,,,,,,,%d,\n", frame, idx, teams, large, cash);
    fflush(team_telemetry_file);
}
