"""Vanilla Yuri's Revenge AI definitions for Brutal oil capture and defense.

Keep engineers in separate teams: script action 46 makes armed teammates attack
the derrick instead of protecting it. Defenders use action 58 and area guard.
"""

# BuildingTypes entries start at 1, but script arguments use the zero-based
# array position. Stock CAOILD is entry 72, array index 71. The installer only
# appends new building types, so this index remains stable.
OIL_INDEX = 71
CAPTURE_SCRIPT = '0F1BC001-G'
NEAR_GUARD_SCRIPT = '0F1BC002-G'
PATROL_SCRIPT = '0F1BC003-G'
NEAREST_OIL = 131072 + OIL_INDEX
FARTHEST_OIL = 196608 + OIL_INDEX
SCRIPTS = {
    CAPTURE_SCRIPT: ('Brutal Oil Capture', [f'46,{NEAREST_OIL}']),
    NEAR_GUARD_SCRIPT: ('Brutal Oil Infantry Guard', [
        f'58,{NEAREST_OIL}', '5,30', '6,1',
    ]),
    PATROL_SCRIPT: ('Brutal Oil Vehicle Patrol', [
        f'58,{NEAREST_OIL}', '5,30', f'58,{FARTHEST_OIL}', '5,30', '6,1',
    ]),
}

# Side index, name, stock engineer-team template, engineer, infantry, vehicles.
# All defenders are available from a barracks/war factory without a battle lab.
FACTIONS = (
    (1, 'Allied', '0CFFF59C-G', 'ENGINEER', ('3,E1', '2,GGI'), ('2,MTNK', '2,FV')),
    (2, 'Soviet', '0D04C46C-G', 'SENGINEER', ('4,E2', '2,SHK'), ('2,HTNK', '2,HTK')),
    (3, 'Yuri', '08B97B3C-G', 'YENGINEER', ('4,INIT', '2,BRUTE'), ('2,LTNK', '2,YTNK')),
)


def ids(side, role):
    """Stable task force and team IDs: role 0 capture, 1 infantry, 2 vehicles."""
    return f'0F1BC{side}0{role}-G', f'0F1BC{side}1{role}-G'


def trigger(side, slot, name, team, condition, weights):
    """Trigger when neutral (7), enemy (0), or owner (1) owns at least one oil.

    The final fields are easy/normal/hard; only hard (Brutal in the game UI)
    is enabled. Oil guards are ordinary teams so they don't consume the stock
    base-defense quota. Neutral and enemy capture share one team's Max limit.
    """
    comparator = '0100000003000000000000000000000000000000000000000000000000000000'
    weight_fields = ','.join(f'{value:.6f}' for value in weights)
    return (f'0F1BC{side}2{slot}-G={name},{team},<all>,1,{condition},CAOILD,'
            f'{comparator},{weight_fields},1,0,{side},0,<none>,0,0,1')
