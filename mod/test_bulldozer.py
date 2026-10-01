"""Integration checks against the installed game's stock archives (no game launch)."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import csf
import bulldozer
import combat_ai

spec = importlib.util.spec_from_file_location('builder', Path(__file__).with_name('build-tesla-mod.py'))
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def ini(data):
    result, section = {}, None
    for line in data.decode('latin-1').splitlines():
        line = line.split(';')[0].strip()
        if line.startswith('['):
            section = result.setdefault(line[1:line.index(']')], {})
        elif section is not None and '=' in line:
            key, value = line.split('=', 1)
            section[key.strip()] = value.strip()
    return result


class BulldozerBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.out = Path(cls.tmp.name)
        builder.build(cls.tmp.name)
        cls.rules = ini((cls.out/'rulesmd.ini').read_bytes())
        cls.art = ini((cls.out/'artmd.ini').read_bytes())

    def test_existing_stock_rules_preserved(self):
        stock = ini(builder.stock('expandmd01.mix', 'rulesmd.ini'))
        for name, section in stock.items():
            for key, value in section.items():
                if (name, key) == ('General', 'TeamDelays'):
                    value = '900,2500,3500'
                value = builder.BALANCE.get((name, key), value)
                self.assertEqual(self.rules[name][key], value, (name, key))
        self.assertIn('ATTNK', self.rules['VehicleTypes'].values())
        self.assertIn('CHEATDEF', self.rules['BuildingTypes'].values())

    def test_buildability_and_asset_references(self):
        unit = self.rules['SBDOZR']
        self.assertEqual(list(self.rules['VehicleTypes'].values()).count('SBDOZR'), 1)
        self.assertEqual(set(unit['Owner'].split(',')), {'Russians','Confederation','Africans','Arabs'})
        self.assertEqual(unit['RequiredHouses'], unit['Owner'])
        self.assertEqual(unit['Prerequisite'], 'NAWEAP,NARADR')
        self.assertEqual(unit['Turret'], 'no')
        self.assertEqual(unit['CrateGoodie'], 'no')
        art = self.art[unit['Image']]
        for ext in ['vxl','hva']:
            self.assertTrue((self.out/f"{unit['Image'].lower()}.{ext}").is_file())
        self.assertTrue((self.out/(art['Cameo'].lower()+'.shp')).is_file())
        _, entries = csf.load(self.out/'ra2md.csf')
        self.assertIn(('Name:SBDOZR', 'Bulldozer'), [(n,v) for n,v,_ in entries])

    def test_damage_by_actual_stock_target_armor(self):
        armors = ['none','flak','plate','light','medium','heavy','wood','steel','concrete','special_1','special_2']
        wh = self.rules['DozerCrush']
        verses = [float(x.rstrip('%'))/100 for x in wh['Verses'].split(',')]
        self.assertEqual(len(verses), 11)
        for weapon_name, high, low in [('DozerBlade',1200,30), ('DozerBladeE',1800,45)]:
            weapon = self.rules[weapon_name]
            self.assertEqual(weapon['Warhead'], 'DozerCrush')
            self.assertEqual(float(weapon['Range']), 1.5)
            for target in ['E1','E2','SHK','GAPOWR','NAWEAP','GACNST']:
                damage = float(weapon['Damage'])*verses[armors.index(self.rules[target]['Armor'].lower())]
                self.assertEqual(damage, high, target)
            for target in ['HTNK','MTNK','APOC','SREF']:
                damage = float(weapon['Damage'])*verses[armors.index(self.rules[target]['Armor'].lower())]
                self.assertEqual(damage, low, target)

    def test_ai_registration_and_soviet_building_assault(self):
        ai = ini((self.out/'aimd.ini').read_bytes())
        stock = ini(builder.stock('ra2md.mix', 'localmd.mix/aimd.ini'))
        for name, section in stock.items():
            for key, value in section.items():
                if name == 'AITriggerTypes':
                    value = combat_ai.stock_trigger_value(value)
                self.assertEqual(ai[name][key], value, (name, key))
        self.assertIn(builder.AI_TEAM, ai['TeamTypes'].values())
        self.assertEqual(ai[builder.AI_TASKFORCE]['0'], '3,ATTNK')
        self.assertIn(bulldozer.AI_TEAM, ai['TeamTypes'].values())
        self.assertIn(bulldozer.AI_TASKFORCE, ai['TaskForces'].values())
        trigger = ai['AITriggerTypes'][bulldozer.AI_TRIGGER].split(',')
        self.assertEqual(len(trigger), 18)
        self.assertEqual(trigger[1:6], [bulldozer.AI_TEAM, '<all>', '5', '1', 'NARADR'])
        self.assertEqual(trigger[12], '2')  # Soviet only
        self.assertEqual(trigger[-3:], ['1', '1', '1'])  # all difficulties
        team = ai[trigger[1]]
        self.assertEqual(team['House'], '<none>')
        self.assertEqual(team['Max'], '1')
        self.assertEqual(team['Autocreate'], 'yes')
        members = {k: v for k, v in ai[team['TaskForce']].items() if k.isdigit()}
        self.assertEqual(members, {'0': '2,SBDOZR', '1': '4,HTNK'})
        script = ai[team['Script']]
        self.assertEqual(script['Name'], 'General Attack Buildings')
        self.assertEqual(script['2'], '0,2')

    def test_crlf_and_complete_install_manifest(self):
        for name in ['rulesmd.ini','artmd.ini','aimd.ini']:
            data = (self.out/name).read_bytes()
            self.assertNotIn(b'\n', data.replace(b'\r\n', b''))
        self.assertEqual(set(builder.MANAGED), {p.name for p in self.out.iterdir()})
        self.assertTrue(set(bulldozer.ASSETS).issubset(builder.MANAGED))


if __name__ == '__main__':
    unittest.main()
