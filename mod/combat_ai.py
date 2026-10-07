"""Brutal skirmish teams using only vanilla 1.001 INI features.

Split shared triggers by difficulty instead of changing a shared stock task
force. Easy/Normal keep their original teams. Siege scripts issue attacks
directly: action 53 (gather at enemy) must not drag artillery into the base.
"""
import struct

PREFIX = '0F1BF'
ASSAULT_SCRIPT = '0F1BF001-G'
SIEGE_SCRIPT = '0F1BF002-G'
NAVAL_SCRIPT = '0F1BF003-G'
AIR_SCRIPT = '0F1BF004-G'
NAVAL_HUNT_SCRIPT = '0F1BF005-G'
COUNTER_ARMOR_SCRIPT = '0F1BF006-G'
COUNTER_AIR_SCRIPT = '0F1BF007-G'
COUNTER_INFANTRY_SCRIPT = '0F1BF008-G'
SCRIPTS = {
    # Vanilla action 0 keeps pursuing its quarry until none remain. Put the
    # intended primary target first; later actions are fallbacks, not a loop.
    ASSAULT_SCRIPT: ('Brutal Assembled Assault', ('54,0', '0,5', '49,0', '0,7', '0,2', '0,1')),
    SIEGE_SCRIPT: ('Brutal Stand-off Bombardment', ('0,7', '49,0', '0,6', '0,2', '0,1')),
    NAVAL_SCRIPT: ('Brutal Shore Bombardment', ('0,2', '49,0', '0,1')),
    AIR_SCRIPT: ('Brutal Building Air Strike', ('0,2', '49,0', '0,1')),
    NAVAL_HUNT_SCRIPT: ('Brutal Naval Hunters', ('0,6', '49,0', '0,1')),
    COUNTER_ARMOR_SCRIPT: ('Brutal Armor Counter', ('54,0', '0,5', '49,0', '0,2')),
    COUNTER_AIR_SCRIPT: ('Brutal Air Counter Guard', ('54,0', '11,11')),
    COUNTER_INFANTRY_SCRIPT: ('Brutal Infantry Counter', ('54,0', '0,4', '49,0', '0,2')),
}

# Each enemy-own condition measures one type, so related types share a Max=1
# counter team. This prevents several sightings from reserving duplicate teams.
COUNTER_THREATS = {
    'armor': (('MTNK', 5), ('HTNK', 5), ('LTNK', 5), ('APOC', 2),
              ('BFRT', 2), ('MIND', 2), ('ROBO', 4), ('TELE', 4)),
    'air': (('ORCA', 3), ('BEAG', 3), ('ZEP', 1), ('DISK', 2), ('JUMPJET', 6)),
    'infantry': (('E1', 8), ('E2', 8), ('INIT', 8), ('GGI', 6),
                 ('SHK', 5), ('BRUTE', 5), ('FLAKT', 8)),
}
COUNTER_TEAMS = {
    1: ('0CABE7DC-G', {
        'armor': (('6,MTNK', '4,GGI'), COUNTER_ARMOR_SCRIPT),
        'air': (('6,FV',), COUNTER_AIR_SCRIPT),
        'infantry': (('4,MTNK', '4,FV'), COUNTER_INFANTRY_SCRIPT),
    }),
    2: ('0CA1D38C-G', {
        'armor': (('6,HTNK', '4,SHK'), COUNTER_ARMOR_SCRIPT),
        'air': (('6,HTK',), COUNTER_AIR_SCRIPT),
        'infantry': (('4,HTNK', '4,HTK'), COUNTER_INFANTRY_SCRIPT),
    }),
    3: ('05FFBD7C-G', {
        'armor': (('6,LTNK', '4,BRUTE'), COUNTER_ARMOR_SCRIPT),
        'air': (('6,YTNK',), COUNTER_AIR_SCRIPT),
        'infantry': (('4,LTNK', '4,YTNK'), COUNTER_INFANTRY_SCRIPT),
    }),
}

# stock template: (composition, script). No missiles/spawned aircraft in forces.
REPLACEMENTS = {
    '0CABE7DC-G': (('10,MTNK', '3,FV', '2,SREF'), ASSAULT_SCRIPT),
    '09B4FB7C-G': (('10,MTNK', '3,FV'), ASSAULT_SCRIPT),
    '0CA0486C-G': (('8,MTNK', '3,FV', '6,JUMPJET'), ASSAULT_SCRIPT),
    '0A43AEFC-G': (('10,MTNK', '4,FV', '4,SREF'), ASSAULT_SCRIPT),
    '0A6E210C-G': (('4,SREF', '6,MTNK', '3,FV'), SIEGE_SCRIPT),
    '0CA48EFC-G': (('8,MTNK', '3,FV'), ASSAULT_SCRIPT),
    '0CA1D38C-G': (('10,HTNK', '3,HTK'), ASSAULT_SCRIPT),
    '06BC7B5C-G': (('8,HTNK', '3,HTK'), ASSAULT_SCRIPT),
    '0CA1683C-G': (('4,V3', '6,HTNK', '3,HTK'), SIEGE_SCRIPT),
    '05FFBD7C-G': (('8,LTNK', '3,YTNK'), ASSAULT_SCRIPT),
    '060F8A7C-G': (('8,LTNK', '3,YTNK', '2,MIND'), ASSAULT_SCRIPT),
    '060F883C-G': (('8,LTNK', '3,YTNK', '2,TELE', '2,MIND'), ASSAULT_SCRIPT),
    '08B9343C-G': (('10,LTNK', '4,YTNK', '2,TELE', '2,MIND'), ASSAULT_SCRIPT),
    '0CA1E67C-G': (('4,DEST', '2,AEGIS', '2,DLPH'), NAVAL_HUNT_SCRIPT),
    '0CA18BAC-G': (('5,SUB', '3,HYD'), NAVAL_HUNT_SCRIPT),
    '06C5992C-G': (('3,BSUB',), NAVAL_HUNT_SCRIPT),
    '0EC2038C-G': (('2,DRED', '3,HYD', '2,SUB'), NAVAL_SCRIPT),
    '05FC5C6C-G': (('4,BSUB',), NAVAL_SCRIPT),
    '08B909EC-G': (('4,BSUB',), NAVAL_SCRIPT),
}
# Stock naval hunters can trigger from enemy state before their owner has a yard.
# Keep the Easy/Normal triggers intact and gate only the cloned Brutal variants.
NAVAL_YARDS = {
    '0CA1E67C-G': 'GAYARD',  # Allied hunters
    '0CA18BAC-G': 'NAYARD',  # Soviet hunters
    '06C5992C-G': 'YAYARD',  # Yuri hunters
    '0EC2038C-G': 'NAYARD',  # Soviet bombardment
}


# AITriggerTypes condition field (AITriggerTypeClass::ConditionMet 0x41E720, switch at 0x41E8F0):
# 0 calls 0x41EAF0 on the house's current enemy, 1 calls 0x41EE90 on the AI house itself.
# YRpp's enum (AIOwns = 0, EnemyOwns = 1) has these backwards; Westwood's own triggers agree
# with the engine ("Soviet Miners" 1,HARV; "Allied Spy vs Soviet Power" 0,NAPOWR).
COND_ENEMY_OWNS, COND_AI_OWNS = 0, 1


def comparator(count, operator=3):
    """Stock condition payload: signed count, comparator (3 => >=)."""
    return (struct.pack('<ii', count, operator) + bytes(24)).hex()


def trigger(identifier, name, team, side, building, count=1, weight=120,
            owner='<all>', condition=COND_AI_OWNS, operator=3):
    return (f'{identifier}={name},{team},{owner},1,{condition},{building},'
            f'{comparator(count, operator)},{weight:.6f},10.000000,{weight:.6f},'
            f'1,0,{side},0,<none>,0,0,1')


def patch(lines, section_lines, clone_section, append_to_list, rules):
    """Register stronger variants and reroute only the Brutal trigger fields."""
    if any(PREFIX in line.split(';')[0] for line in lines):
        raise SystemExit('Combat AI IDs already present')
    def add_script(identifier):
        name, actions = SCRIPTS[identifier]
        append_to_list(lines, 'ScriptTypes', identifier)
        lines.extend([f'[{identifier}]', f'Name={name}'])
        lines.extend(f'{i}={action}' for i, action in enumerate(actions))
        lines.append('')

    counter_scripts = (COUNTER_ARMOR_SCRIPT, COUNTER_AIR_SCRIPT, COUNTER_INFANTRY_SCRIPT)
    for identifier in SCRIPTS:
        if identifier not in counter_scripts:
            add_script(identifier)

    serial = 0
    new_triggers = []

    def add_team(template, name, members, script, limit='1', capture=False,
                 counter=False, defense=False):
        nonlocal serial
        force_id = f'{PREFIX}{0x100 + serial:03X}-G'
        team_id = f'{PREFIX}{0x200 + serial:03X}-G'
        serial += 1
        append_to_list(lines, 'TaskForces', force_id)
        append_to_list(lines, 'TeamTypes', team_id)
        body = clone_section(lines, template, team_id, {
            'Name': name, 'TaskForce': force_id, 'Script': script, 'House': '<none>',
            'Max': limit, 'Priority': '60' if capture else '40' if counter else '20',
            'Autocreate': 'yes',
            'Recruiter': 'no', 'AreTeamMembersRecruitable': 'no', 'LooseRecruit': 'no',
            'Full': 'no', 'Reinforce': 'no', 'Suicide': 'no',
            'IsBaseDefense': 'yes' if defense else 'no',
            'Aggressive': 'no' if capture else 'yes',
            'AvoidThreats': 'yes' if capture else 'no',
        })
        lines.extend([f'[{force_id}]', f'Name={name}', 'Group=-1'])
        lines.extend(f'{i}={member}' for i, member in enumerate(members))
        lines.extend([''] + body + [''])
        return team_id

    replacements = {
        old: add_team(old, 'Brutal ' + old, members, script)
        for old, (members, script) in REPLACEMENTS.items()
    }
    # Disable stock Brutal air strikes; replacements below adapt to dock count.
    air_templates = {'0CAC9EFC-G', '0CACFEFC-G', '0CB1CDEC-G', '0CB246CC-G'}
    s, e = section_lines(lines, 'AITriggerTypes')
    for i in range(s + 1, e):
        raw = lines[i].split(';')[0].strip()
        if '=' not in raw:
            continue
        identifier, value = raw.split('=', 1)
        fields = value.split(',')
        if len(fields) != 18 or fields[17] != '1':
            continue
        if fields[1] in air_templates:
            fields[17] = '0'
            lines[i] = identifier + '=' + ','.join(fields)
        elif fields[1] in replacements or fields[14] in replacements:
            hard = fields.copy()
            hard[0] = 'Brutal ' + hard[0]
            hard[1] = replacements.get(hard[1], hard[1])
            hard[14] = replacements.get(hard[14], hard[14])
            if fields[1] in NAVAL_YARDS:
                # the stock hunters' comparator is ">= 0", which no yard count can fail
                hard[4:7] = [str(COND_AI_OWNS), NAVAL_YARDS[fields[1]], comparator(1)]
            if hard[14] in air_templates:
                hard[14] = '<none>'
            hard[15:] = ['0', '0', '1']
            fields[17] = '0'
            lines[i] = identifier + '=' + ','.join(fields)
            new_triggers.append(f'{PREFIX}{0x300 + len(new_triggers):03X}-G=' + ','.join(hard))
        elif fields[14] in air_templates:
            hard = fields.copy()
            hard[14] = '<none>'
            hard[15:] = ['0', '0', '1']
            fields[17] = '0'
            lines[i] = identifier + '=' + ','.join(fields)
            new_triggers.append(f'{PREFIX}{0x300 + len(new_triggers):03X}-G=' + ','.join(hard))

    def add_trigger(name, team, side, building, **kwargs):
        identifier = f'{PREFIX}{0x300 + len(new_triggers):03X}-G'
        new_triggers.append(trigger(identifier, name, team, side, building, **kwargs))

    # Request bombardment only after the owner has a yard: gated on a tech
    # building, the team reserved ships before the AI had any way to build them.
    for side, template, members, yard in (
        (1, '0CAC330C-G', ('2,CARRIER', '2,DEST', '2,AEGIS'), 'GAYARD'),
        (2, '0EC2038C-G', ('2,DRED', '3,HYD', '2,SUB'), 'NAYARD'),
        (3, '06C5992C-G', ('3,BSUB',), 'YAYARD'),
    ):
        team = add_team(template, f'Brutal Navy Bombard {side}', members, NAVAL_SCRIPT)
        add_trigger(f'Brutal Navy Bombard {side}', team, side, yard,
                    condition=COND_AI_OWNS, weight=220)

    # Two airbases mean eight docks. Four-plane teams remain available while
    # expanding, but cannot keep starting and reserving pads after the second.
    for country in ('British', 'French', 'Germans', 'Americans', 'Alliance'):
        aircraft = 'BEAG' if country == 'Alliance' else 'ORCA'
        airport = 'AMRADR' if country == 'Americans' else 'GAAIRC'
        template = '0CB1CDEC-G' if aircraft == 'BEAG' else '0CACFEFC-G'
        for count, operator, planes in ((1, 2, 4), (2, 3, 8)):
            team = add_team(template, f'Brutal {country} {planes}-plane Strike',
                            (f'{planes},{aircraft}',), AIR_SCRIPT)
            add_trigger(f'Brutal {country} {planes}-plane Strike', team, 1, airport,
                        owner=country, condition=COND_AI_OWNS, count=count,
                        operator=operator, weight=140)

    # Other neutral tech buildings get their own unarmed engineer teams.
    # Oil capture/defense continues to use the existing oil_ai IDs and limits.
    buildings = list(rules['BuildingTypes'].values())
    for side, template, engineer in (
        (1, '0CFFF59C-G', 'ENGINEER'), (2, '0D04C46C-G', 'SENGINEER'),
        (3, '08B97B3C-G', 'YENGINEER'),
    ):
        for offset, building in enumerate(('CAAIRP', 'CATHOSP', 'CAOUTP', 'CAMACH', 'CAPOWR')):
            script = f'{PREFIX}{0x010 + offset:03X}-G'
            if side == 1:
                append_to_list(lines, 'ScriptTypes', script)
                lines.extend([f'[{script}]', f'Name=Capture nearest {building}',
                              f'0=46,{131072 + buildings.index(building)}', ''])
            team = add_team(template, f'Brutal {side} Capture {building}',
                            (f'1,{engineer}',), script, capture=True)
            add_trigger(f'Brutal {side} Capture {building}', team, side, building,
                        condition=7, weight=120)

    # Counters activate only when the current enemy fields a sizeable force.
    # Reuse the same team for each threat in a role; Max=1 bounds production.
    for script in counter_scripts:
        add_script(script)
    for side, (template, roles) in COUNTER_TEAMS.items():
        for role, (members, script) in roles.items():
            team = add_team(template, f'Brutal {side} Counter {role}', members,
                            script, counter=True, defense=role == 'air')
            for enemy, minimum in COUNTER_THREATS[role]:
                add_trigger(f'Brutal {side} Counter {role} vs {enemy}', team, side, enemy,
                            condition=COND_ENEMY_OWNS, count=minimum, weight=180)

    s, e = section_lines(lines, 'AITriggerTypes')
    last = max(i for i in range(s + 1, e) if '=' in lines[i].split(';')[0])
    lines[last + 1:last + 1] = new_triggers


def stock_trigger_value(value):
    """Expected preserved stock value, except rerouted Brutal enable flags."""
    fields = value.split(',')
    air = {'0CAC9EFC-G', '0CACFEFC-G', '0CB1CDEC-G', '0CB246CC-G'}
    if len(fields) == 18 and fields[17] == '1' and (
        fields[1] in REPLACEMENTS or fields[14] in REPLACEMENTS
        or fields[1] in air or fields[14] in air
    ):
        fields[17] = '0'
    return ','.join(fields)
