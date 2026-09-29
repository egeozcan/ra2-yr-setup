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

    def test_navy_bombards_require_own_shipyard(self):
        fleets = [f for f in self.new.values() if f[0].startswith('Brutal Navy Bombard')]
        self.assertEqual(len(fleets), 3)
        for fields in fleets:
            self.assertEqual(fields[4], '0')  # AIOwns, not EnemyOwns
            self.assertIn(fields[5], ('GAYARD', 'NAYARD', 'YAYARD'))
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
                self.assertEqual(fields[4:6], ['0', yard])

    def test_air_strikes_fit_docks_and_korea_uses_black_eagles(self):
        strikes = [f for f in self.new.values() if '-plane Strike' in f[0]]
        self.assertEqual(len(strikes), 10)
        for fields in strikes:
            self.assertEqual(fields[4], '0')  # own docks, not the enemy's
            country = fields[2]
            self.assertEqual(fields[5], 'AMRADR' if country == 'Americans' else 'GAAIRC')
            units = self.members(fields[1])
            unit = 'BEAG' if country == 'Alliance' else 'ORCA'
            self.assertEqual(set(units), {unit})
            count, op = struct.unpack('<ii', bytes.fromhex(fields[6])[:8])
            self.assertEqual((count, op), (1, 2) if units[unit] == 4 else (2, 3))
            self.assertLessEqual(units[unit], count * int(self.rules[fields[5]]['NumberOfDocks']))

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
