"""Check complete vanilla AI output, difficulty isolation and real prerequisites."""
import struct
import unittest

import combat_ai
from test_bulldozer import builder, ini


class CombatAITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stock = ini(builder.stock('ra2md.mix', 'localmd.mix/aimd.ini'))
        cls.data = builder.patch_ai(builder.stock('ra2md.mix', 'localmd.mix/aimd.ini').decode('latin-1')).encode('latin-1')
        cls.ai = ini(cls.data)
        cls.rules = ini(builder.patch(builder.stock('expandmd01.mix', 'rulesmd.ini').decode('latin-1')).encode('latin-1'))
        cls.triggers = {k: v.split(',') for k, v in cls.ai['AITriggerTypes'].items()}
        cls.new = {k: v for k, v in cls.triggers.items() if k.startswith(combat_ai.PREFIX)}

    def test_only_brutal_stock_triggers_change_and_all_references_resolve(self):
        for name, section in self.stock.items():
            for key, value in section.items():
                expected = combat_ai.stock_trigger_value(value) if name == 'AITriggerTypes' else value
                self.assertEqual(self.ai[name][key], expected, (name, key))
        self.assertTrue(self.new)
        for fields in self.new.values():
            self.assertEqual(len(fields), 18)
            self.assertEqual(fields[15:], ['0', '0', '1'])
        for fields in self.triggers.values():
            for team_id in (fields[1], fields[14]):
                if team_id == '<none>':
                    continue
                team = self.ai[team_id]
                self.assertIn(team['TaskForce'], self.ai['TaskForces'].values())
                self.assertIn(team['Script'], self.ai['ScriptTypes'].values())
        for registry in ('TaskForces', 'TeamTypes', 'ScriptTypes'):
            values = list(self.ai[registry].values())
            self.assertEqual(len(values), len(set(values)))
        self.assertNotIn(b'\n', self.data.replace(b'\r\n', b''))

    def members(self, team):
        force = self.ai[self.ai[team]['TaskForce']]
        return {unit: int(count) for k, v in force.items() if k.isdigit()
                for count, unit in [v.split(',')]}

    def test_brutal_land_groups_are_large_mixed_and_buildable(self):
        for old, (members, _) in combat_ai.REPLACEMENTS.items():
            # Names on triggers can differ from team names; match via force.
            candidates = [t for t in self.ai['TeamTypes'].values()
                          if self.ai[t]['Name'] == 'Brutal ' + old]
            self.assertEqual(len(candidates), 1)
            units = self.members(candidates[0])
            self.assertEqual(units, {u: int(n) for n, u in (x.split(',') for x in members)})
            if any(u in units for u in ('MTNK', 'HTNK', 'LTNK')):
                self.assertGreaterEqual(sum(units.values()), 11)
                self.assertGreaterEqual(len(units), 2)
            for unit in units:
                self.assertGreaterEqual(int(self.rules[unit]['TechLevel']), 0, unit)
                self.assertIn(unit, set(self.rules['VehicleTypes'].values())
                              | set(self.rules['InfantryTypes'].values()))
            self.assertNotIn('V3ROCKET', units)

    def test_siege_scripts_do_not_move_to_enemy_base(self):
        for script in (combat_ai.SIEGE_SCRIPT, combat_ai.NAVAL_SCRIPT):
            actions = [v for k, v in self.ai[script].items() if k.isdigit()]
            self.assertTrue(any(v.startswith('0,') for v in actions))
            self.assertFalse(any(v.split(',')[0] in ('53', '58', '3') for v in actions))
        v3 = next(t for t in self.ai['TeamTypes'].values()
                  if self.ai[t]['Name'] == 'Brutal 0CA1683C-G')
        self.assertEqual(self.members(v3)['V3'], 4)

    def test_armor_and_siege_prioritize_different_targets(self):
        expected = {
            combat_ai.ASSAULT_SCRIPT: ['54,0', '0,5', '49,0', '0,7', '0,2', '0,1'],
            combat_ai.SIEGE_SCRIPT: ['0,7', '49,0', '0,6', '0,2', '0,1'],
        }
        for script_id, actions in expected.items():
            actual = [value for key, value in self.ai[script_id].items() if key.isdigit()]
            self.assertEqual(actual, actions)
        for old, (_, script_id) in combat_ai.REPLACEMENTS.items():
            if script_id not in expected:
                continue
            team = next(t for t in self.ai['TeamTypes'].values()
                        if self.ai[t]['Name'] == 'Brutal ' + old)
            self.assertEqual(self.ai[team]['Script'], script_id)

    def test_condition_constants_match_westwood_triggers(self):
        # Stock triggers whose meaning is unambiguous pin which value is which.
        stock = {k: v.split(',') for k, v in self.stock['AITriggerTypes'].items()}
        for identifier, name, condition, building in (
            ('0C8C2C3C-G', 'Soviet Miners', combat_ai.COND_AI_OWNS, 'HARV'),        # own HARV <= 3
            ('043F874C-G', 'Allied MCV - H', combat_ai.COND_AI_OWNS, 'GACNST'),     # own yards == 0
            ('0C9561BC-G', 'Allied Spy vs Soviet Power', combat_ai.COND_ENEMY_OWNS, 'NAPOWR'),
            ('0D53430C-G', 'Soviet Engineer vs Ally med', combat_ai.COND_ENEMY_OWNS, 'GACNST'),
        ):
            self.assertEqual(stock[identifier][0], name)
            self.assertEqual(stock[identifier][4:6], [str(condition), building], name)

    def test_navy_bombards_require_own_shipyard(self):
        fleets = [f for f in self.new.values() if f[0].startswith('Brutal Navy Bombard')]
        self.assertEqual(len(fleets), 3)
        for fields in fleets:
            self.assertEqual(fields[4], str(combat_ai.COND_AI_OWNS))
            self.assertIn(fields[5], ('GAYARD', 'NAYARD', 'YAYARD'))
            count, operator = struct.unpack('<ii', bytes.fromhex(fields[6])[:8])
            self.assertEqual((count, operator), (1, 3))  # at least one yard
            units = self.members(fields[1])
            self.assertGreaterEqual(sum(units.values()), 3)
            for unit in units:
                self.assertEqual(self.rules[unit]['Naval'], 'yes')
            self.assertTrue(set(units) & {'CARRIER', 'DRED', 'BSUB'})

        for old, yard in combat_ai.NAVAL_YARDS.items():
            new_team = next(t for t in self.ai['TeamTypes'].values()
                            if self.ai[t]['Name'] == 'Brutal ' + old)
            cloned = [f for f in self.new.values() if f[1] == new_team]
            self.assertTrue(cloned, old)
            for fields in cloned:
                self.assertEqual(fields[4:6], [str(combat_ai.COND_AI_OWNS), yard])
                count, operator = struct.unpack('<ii', bytes.fromhex(fields[6])[:8])
                self.assertEqual((count, operator), (1, 3))  # stock ">= 0" never gates

    def test_air_strikes_fit_docks_and_korea_uses_black_eagles(self):
        strikes = [f for f in self.new.values() if '-plane Strike' in f[0]]
        self.assertEqual(len(strikes), 10)
        for fields in strikes:
            self.assertEqual(fields[4], str(combat_ai.COND_AI_OWNS))  # own docks, not the enemy's
            country = fields[2]
            self.assertEqual(fields[5], 'AMRADR' if country == 'Americans' else 'GAAIRC')
            units = self.members(fields[1])
            unit = 'BEAG' if country == 'Alliance' else 'ORCA'
            self.assertEqual(set(units), {unit})
            count, op = struct.unpack('<ii', bytes.fromhex(fields[6])[:8])
            self.assertEqual((count, op), (1, 2) if units[unit] == 4 else (2, 3))
            self.assertLessEqual(units[unit], count * int(self.rules[fields[5]]['NumberOfDocks']))

    def test_counters_follow_enemy_force_and_share_one_team_per_role(self):
        threats = combat_ai.COUNTER_THREATS
        all_units = (set(self.rules['VehicleTypes'].values())
                     | set(self.rules['InfantryTypes'].values())
                     | set(self.rules['AircraftTypes'].values()))
        expected_count = sum(len(threats[role]) for _, roles in combat_ai.COUNTER_TEAMS.values()
                             for role in roles)
        counters = [f for f in self.new.values() if ' Counter ' in f[0]]
        self.assertEqual(len(counters), expected_count)
        for side, (_, roles) in combat_ai.COUNTER_TEAMS.items():
            for role, (members, script_id) in roles.items():
                matching = [f for f in counters
                            if f[0].startswith(f'Brutal {side} Counter {role} vs ')]
                self.assertEqual(len(matching), len(threats[role]))
                self.assertEqual({f[5] for f in matching}, {u for u, _ in threats[role]})
                self.assertEqual(len({f[1] for f in matching}), 1)
                team = self.ai[matching[0][1]]
                self.assertEqual(team['Script'], script_id)
                self.assertEqual(team['Max'], '1')
                self.assertEqual(team['Priority'], '40')
                self.assertEqual(team['IsBaseDefense'], 'yes' if role == 'air' else 'no')
                self.assertEqual(self.members(matching[0][1]),
                                 {u: int(n) for n, u in (x.split(',') for x in members)})
                for unit in self.members(matching[0][1]):
                    self.assertIn(unit, all_units)
                    self.assertGreaterEqual(int(self.rules[unit]['TechLevel']), 0)
                for fields in matching:
                    self.assertEqual(fields[4], str(combat_ai.COND_ENEMY_OWNS))
                    self.assertEqual(fields[12], str(side))
                    count, operator = struct.unpack('<ii', bytes.fromhex(fields[6])[:8])
                    self.assertEqual((count, operator), (dict(threats[role])[fields[5]], 3))
        self.assertEqual(self.ai[combat_ai.COUNTER_AIR_SCRIPT]['1'], '11,11')

    def test_tech_captures_use_correct_indices_and_unarmed_teams(self):
        captures = [f for f in self.new.values() if ' Capture ' in f[0]]
        self.assertEqual(len(captures), 15)
        buildings = list(self.rules['BuildingTypes'].values())
        for fields in captures:
            team = self.ai[fields[1]]
            self.assertEqual(fields[4], '7')
            self.assertEqual(sum(self.members(fields[1]).values()), 1)
            self.assertEqual(team['Max'], '1')
            self.assertEqual(team['AvoidThreats'], 'yes')
            script = self.ai[team['Script']]
            self.assertEqual(script['0'], f'46,{131072 + buildings.index(fields[5])}')
        for side in (1, 2, 3):
            oil = self.triggers[f'0F1BC{side}20-G']
            self.assertGreaterEqual(float(oil[8]), 500)


if __name__ == '__main__':
    unittest.main()
