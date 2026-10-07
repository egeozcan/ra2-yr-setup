"""Check oil AI against the real stock archives, without launching the game."""
import struct
import unittest

import oil_ai
import combat_ai
from test_bulldozer import builder, ini


class OilAITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stock_bytes = builder.stock('ra2md.mix', 'localmd.mix/aimd.ini')
        cls.stock = ini(cls.stock_bytes)
        cls.data = builder.patch_ai(cls.stock_bytes.decode('latin-1')).encode('latin-1')
        cls.ai = ini(cls.data)
        cls.rules = ini(builder.stock('expandmd01.mix', 'rulesmd.ini'))
        cls.triggers = {
            key: value.split(',') for key, value in cls.ai['AITriggerTypes'].items()
            if key.startswith('0F1BC')
        }

    def test_existing_ai_and_lower_difficulties_preserved(self):
        for section, entries in self.stock.items():
            for key, value in entries.items():
                if section == 'AITriggerTypes':
                    value = combat_ai.stock_trigger_value(value)
                self.assertEqual(self.ai[section][key], value, (section, key))
        self.assertEqual(len(self.triggers), 12)
        for fields in self.triggers.values():
            self.assertEqual(len(fields), 18)
            self.assertEqual(fields[2], '<all>')
            self.assertEqual(fields[10], '1')  # skirmish enabled
            self.assertEqual(fields[13:15], ['0', '<none>'])
            self.assertEqual(fields[15:], ['0', '0', '1'])  # Brutal only

    def test_capture_conditions_priority_and_shared_limit(self):
        for side, _, template, engineer, _, _ in oil_ai.FACTIONS:
            capture_id = oil_ai.ids(side, 0)[1]
            capture = self.ai[capture_id]
            neutral = self.triggers[f'0F1BC{side}20-G']
            enemy = self.triggers[f'0F1BC{side}21-G']
            self.assertEqual(neutral[1], enemy[1])
            self.assertEqual(neutral[1], capture_id)
            self.assertEqual(neutral[4:6], ['7', 'CAOILD'])
            self.assertEqual(enemy[4:6], [str(combat_ai.COND_ENEMY_OWNS), 'CAOILD'])
            # Retain stock >= 1 encoding rather than assuming struct field order.
            stock_trigger = next(v.split(',') for v in self.stock['AITriggerTypes'].values()
                                 if v.split(',')[1] == template)
            self.assertEqual(neutral[6], stock_trigger[6])
            self.assertGreater(float(neutral[7]), float(stock_trigger[7]))
            self.assertGreater(float(neutral[8]), float(stock_trigger[9]))
            self.assertEqual(capture['Max'], '2')
            self.assertEqual(capture['Suicide'], 'no')
            self.assertEqual(capture['AvoidThreats'], 'yes')
            force = self.ai[capture['TaskForce']]
            self.assertEqual({k: v for k, v in force.items() if k.isdigit()},
                             {'0': f'1,{engineer}'})
            script = self.ai[capture['Script']]
            self.assertEqual({k: v for k, v in script.items() if k.isdigit()},
                             {'0': f'46,{131072 + oil_ai.OIL_INDEX}'})

    def test_defenders_only_requested_for_owned_oil_and_remain_assigned(self):
        for side, *_ in oil_ai.FACTIONS:
            for slot, role in ((2, 1), (3, 2)):
                trigger = self.triggers[f'0F1BC{side}2{slot}-G']
                self.assertEqual(trigger[4:6], [str(combat_ai.COND_AI_OWNS), 'CAOILD'])
                self.assertEqual(struct.unpack('<ii', bytes.fromhex(trigger[6])[:8]), (1, 3))
                self.assertEqual(trigger[12], str(side))
                self.assertEqual(trigger[1], oil_ai.ids(side, role)[1])
                team = self.ai[trigger[1]]
                self.assertEqual(team['House'], '<none>')
                self.assertEqual(team['Max'], '1')
                self.assertEqual(team['AreTeamMembersRecruitable'], 'no')
                self.assertEqual(team['IsBaseDefense'], 'no')
                self.assertEqual(team['Suicide'], 'no')
                self.assertEqual(team['Full'], 'no')  # keep working after casualties
                self.assertEqual(team['Aggressive'], 'yes')

    def test_defender_units_are_buildable_by_their_faction_at_early_tech(self):
        countries = {
            1: {'British', 'French', 'Germans', 'Americans', 'Alliance'},
            2: {'Russians', 'Confederation', 'Africans', 'Arabs'},
            3: {'YuriCountry'},
        }
        barracks = {1: 'GAPILE', 2: 'NAHAND', 3: 'YABRCK'}
        factories = {1: 'GAWEAP', 2: 'NAWEAP', 3: 'YAWEAP'}
        for side, *_ in oil_ai.FACTIONS:
            for role in (1, 2):
                force = self.ai[oil_ai.ids(side, role)[0]]
                members = [v.split(',') for k, v in force.items() if k.isdigit()]
                self.assertEqual(len(members), 2)
                for count, unit in members:
                    self.assertGreater(int(count), 0)
                    rules = self.rules[unit]
                    self.assertTrue(countries[side].issubset(set(rules['Owner'].split(','))))
                    self.assertEqual(rules['Prerequisite'], barracks[side] if role == 1 else factories[side])
                    self.assertLessEqual(int(rules['TechLevel']), 5)

    def test_guard_scripts_target_oil_and_loop_without_attacking_buildings(self):
        index = list(self.rules['BuildingTypes'].values()).index('CAOILD')
        self.assertEqual(index, oil_ai.OIL_INDEX)
        for script_id in (oil_ai.NEAR_GUARD_SCRIPT, oil_ai.PATROL_SCRIPT):
            script = self.ai[script_id]
            actions = [v.split(',') for k, v in script.items() if k.isdigit()]
            self.assertEqual(actions[0], ['58', str(131072 + index)])
            self.assertEqual(actions[-1], ['6', '1'])  # 1-based jump to first line
            self.assertTrue(all(int(a) in (58, 5, 6) for a, _ in actions))
            for action, argument in actions:
                if action == '58':
                    self.assertEqual(int(argument) % 65536, index)
                if action == '5':
                    self.assertGreater(int(argument), 0)
        patrol = self.ai[oil_ai.PATROL_SCRIPT]
        self.assertEqual(patrol['2'], f'58,{196608 + index}')

    def test_unique_registrations_references_and_crlf(self):
        for registry in ('TeamTypes', 'TaskForces', 'ScriptTypes'):
            values = list(self.ai[registry].values())
            self.assertEqual(len(values), len(set(values)))
        for side, *_ in oil_ai.FACTIONS:
            for role in range(3):
                force_id, team_id = oil_ai.ids(side, role)
                self.assertIn(force_id, self.ai['TaskForces'].values())
                self.assertIn(team_id, self.ai['TeamTypes'].values())
                team = self.ai[team_id]
                self.assertEqual(team['TaskForce'], force_id)
                self.assertIn(team['Script'], self.ai['ScriptTypes'].values())
        self.assertNotIn(b'\n', self.data.replace(b'\r\n', b''))
        with self.assertRaises(SystemExit):
            builder.patch_ai(self.data.decode('latin-1'))


if __name__ == '__main__':
    unittest.main()
