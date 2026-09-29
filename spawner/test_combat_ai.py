"""Native policy checks and execution of the actual x86 siege wrapper.

Engine tests reuse test_human_peace's Unicorn harness and symbol-bearing DLL.
Build as described in README; dependencies are unicorn, pefile and capstone.
"""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

import test_human_peace as emulator

HERE = Path(__file__).resolve().parent


class PolicyTests(unittest.TestCase):
    def test_expansion_budget_timing_limits_and_variety(self):
        source = r'''
        #include <assert.h>
        #include "combat-ai-policy.h"
        int main(void) {
            assert(combat_expand(2700, 10, 2, 8000, 100, 2000, 50, 1, 1));
            assert(!combat_expand(2699, 10, 2, 8000, 100, 2000, 50, 1, 1));
            assert(!combat_expand(2700, 9, 2, 8000, 100, 2000, 50, 1, 1));
            assert(!combat_expand(2700, 10, 1, 8000, 100, 2000, 50, 1, 1));
            assert(!combat_expand(2700, 10, 2, 7999, 100, 2000, 50, 1, 1));
            assert(!combat_expand(2700, 10, 2, 8000, 99, 2000, 50, 1, 1));
            assert(!combat_expand(2700, 10, 2, 8000, 100, 2000, 50, 2, 1));
            assert(!combat_expand(2700, 10, 2, 8000, 100, 2000, 50, 0, 1));
            assert(!combat_expand(2700, 10, 2, 8000, 100, 2000, 50, 1, 0));
            unsigned seen = 0;
            for (unsigned seed = 1; seed <= 100; seed++) {
                unsigned choice = combat_choices(seed, 1);
                assert(choice == combat_choices(seed, 1));
                seen |= 1u << choice;
            }
            assert(seen == 15); // none, factory, airport, both all occur
            assert(combat_siege_mission(1));
            assert(combat_siege_mission(2));
            assert(combat_siege_mission(15));
            assert(combat_siege_mission(29));
            assert(!combat_siege_mission(5)); // don't move sleeping/guarding units
        }
        '''
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'policy.c').write_text(source)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(HERE),
                            str(tmp / 'policy.c'), '-o', str(tmp / 'policy')], check=True)
            subprocess.run([str(tmp / 'policy')], check=True)


class EngineTests(unittest.TestCase):
    setUpClass = classmethod(emulator.EngineTests.setUpClass.__func__)
    put = emulator.EngineTests.put
    get = emulator.EngineTests.get
    vector = emulator.EngineTests.vector
    object = emulator.EngineTests.object
    call = emulator.EngineTests.call

    def setUp(self):
        emulator.EngineTests.setUp(self)
        self.uc.mem_write(self.firer + 0x74, b'\x01')
        self.uc.mem_write(self.firer + 0x90, b'\x01')
        self.put(self.firer + 0xAC, 1)
        self.put(self.firer + 0x6C4, 0x10120000)
        self.uc.mem_write(0x10120024, b'SREF\0')
        self.put(self.firer + 0x2B4, self.enemy_obj)
        self.put(self.firer + 0x5A4, self.enemy_obj)
        self.weapon, self.close = 0, True
        self.put(0xA8ED84, 8)
        self.imports[0x7360C0] = 'unit_original'
        self.actions = []
        self.types = {}
        self.can_place = True
        self.remove_on_ai = False
        self.uc.mem_write(0x10130000, bytes(self.uc.mem_read(0x7F5C70, 0x600)))
        self.put(self.firer, 0x10130000)
        for i, (slot, name) in enumerate(((0x2E4, 'select'), (0x3A8, 'range'),
            (0x480, 'destination'), (0x500, 'stop'), (0x1E8, 'mission'), (0x3C8, 'target'))):
            address = 0x31008000 + i * 16
            self.imports[address] = name
            self.put(0x10130000 + slot, address)

    def string(self, address):
        data = bytearray()
        while (b := bytes(self.uc.mem_read(address + len(data), 1))) != b'\0':
            data.extend(b)
        return data.decode('ascii')

    def on_code(self, uc, address, size, _):
        name = self.imports.get(address)
        if name not in ('strlen', 'strspn', 'strcspn', '_strnicmp', '_stricmp', 'select',
                        'range', 'destination', 'stop', 'mission', 'target', 'unit_original',
                        'cost', 'buildable', 'place', 'can_place', 'build_original'):
            return emulator.EngineTests.on_code(self, uc, address, size, _)
        esp = uc.reg_read(self.reg.UC_X86_REG_ESP)
        args = [self.get(esp + 4 + 4*i) for i in range(4)]
        value, clean = 0, 0
        if name == 'strlen':
            value = len(self.string(args[0]))
        elif name in ('strspn', 'strcspn'):
            chars = self.string(args[1])
            for ch in self.string(args[0]):
                if (ch in chars) != (name == 'strspn'):
                    break
                value += 1
        elif name == '_strnicmp':
            a, b = (bytes(uc.mem_read(args[i], args[2])).lower() for i in (0, 1))
            value = (a > b) - (a < b)
        elif name == '_stricmp':
            a, b = self.string(args[0]).lower(), self.string(args[1]).lower()
            value = (a > b) - (a < b)
        elif name == 'cost':
            value = 1000 if self.types[uc.reg_read(self.reg.UC_X86_REG_ECX)] in ('GAAIRC', 'AMRADR') else 2000
        elif name == 'buildable':
            value, clean = (0 if self.types[args[0]] == 'AMRADR' else 1), 12
        elif name == 'place':
            self.actions.append((name, args))
            value, clean = args[0], 16
            uc.mem_write(args[0], struct.pack('<hh', 60, 60))
        elif name == 'can_place':
            value, clean = int(self.can_place), 8
        elif name == 'build_original':
            value = 123
        elif name == 'unit_original':
            if self.remove_on_ai:
                self.vector(0xA8EC78, [self.player_obj, self.enemy_obj], 0x10111000)
                uc.mem_unmap(self.firer, 0x1000)
        else:
            self.actions.append((name, args))
            if name == 'select':
                value, clean = self.weapon, 4
            elif name == 'range':
                value, clean = int(self.close), 8
            elif name == 'destination':
                self.put(self.firer + 0x5A4, args[0])
                clean = 8
            elif name == 'mission':
                self.put(self.firer + 0xAC, args[0])
                clean = 8
            elif name == 'target':
                self.put(self.firer + 0x2B4, args[0])
                clean = 4
        uc.reg_write(self.reg.UC_X86_REG_EAX, value & 0xFFFFFFFF)
        uc.reg_write(self.reg.UC_X86_REG_EIP, self.get(esp))
        uc.reg_write(self.reg.UC_X86_REG_ESP, esp + 4 + clean)

    def test_siege_stops_in_range_retains_target_and_selects_boomer_secondary(self):
        for unit in ('SREF', 'V3', 'DRED', 'CARRIER', 'BSUB'):
            self.actions.clear()
            self.uc.mem_write(0x10120024, unit.encode() + b'\0')
            self.put(self.firer + 0x5A4, self.enemy_obj)
            self.weapon = 1 if unit == 'BSUB' else 0
            self.call('unit_ai', this=self.firer, clean=0)
            self.assertEqual(self.get(self.firer + 0x5A4), 0)
            self.assertEqual(self.get(self.firer + 0x2B4), self.enemy_obj)
            self.assertEqual([n for n, _ in self.actions],
                             ['select', 'range', 'destination', 'stop', 'mission', 'target'])
            self.assertEqual(self.actions[1][1][:2], [self.enemy_obj, self.weapon])
            self.assertEqual(self.get(self.firer + 0xAC), 1)

    def test_out_of_range_keeps_approaching(self):
        self.close = False
        self.call('unit_ai', this=self.firer)
        self.assertEqual([n for n, _ in self.actions], ['select', 'range'])
        self.assertEqual(self.get(self.firer + 0x5A4), self.enemy_obj)

    def test_unit_deleted_by_stock_ai_is_not_dereferenced(self):
        self.remove_on_ai = True
        self.call('unit_ai', this=self.firer, clean=0)
        self.assertFalse(self.actions)

    def test_human_lower_difficulties_and_guard_orders_are_untouched(self):
        for owner, difficulty, mission in ((self.human, 0, 1), (self.ai, 1, 1),
                                           (self.ai, 2, 1), (self.ai, 0, 5)):
            self.put(self.firer + 0x21C, owner)
            self.put(owner + 0x184, difficulty)
            self.put(self.firer + 0xAC, mission)
            self.call('unit_ai', this=self.firer)
            self.assertFalse(self.actions)
            self.assertEqual(self.get(self.firer + 0x5A4), self.enemy_obj)

    def test_verified_engine_slots_and_native_placement_callback(self):
        for slot, address in ((0x2E4, 0x746CD0), (0x3A8, 0x6F77B0),
                              (0x480, 0x741970), (0x500, 0x4D55C0), (0x1E8, 0x5B35E0)):
            self.assertEqual(self.get(0x7F5C70 + slot), address)
        self.assertEqual(bytes(self.uc.mem_read(0x444FCB, 7)), bytes.fromhex('6a ff 68 80 5f 50 00'))
        # Native comparator dispatch: op 1 <=, op 3 >=; count is first word.
        self.assertEqual(self.get(0x41F010 + 4), 0x41EF68)
        self.assertEqual(self.get(0x41F010 + 12), 0x41EF94)

    def expansion_fixture(self):
        self.put(self.ai + 0x60, 1)  # Construction Yard
        self.put(self.ai + 0x15C, 2)  # refineries
        self.uc.mem_write(self.ai + 0x1EE, b'\x01')
        self.put(self.ai + 0x564C, -1)
        self.put(self.ai + 0x30C, 10000)
        self.put(self.ai + 0x53A4, 1000)
        self.put(self.ai + 0x53A8, 300)
        self.put(self.ai + 0x2F0, 10)
        self.put(0xA8ED84, 3000)
        # Choose both expansions; this deterministic value comes from policy.
        self.put(0xA8ED94, 1)
        ids = ('GAYARD', 'GAWEAP', 'GAAIRC', 'AMRADR', 'GAPOWR')
        pointers = []
        for i, id in enumerate(ids):
            at = 0x10140000 + i * 0x2000
            pointers.append(at)
            self.types[at] = id
            self.put(at, 0x10130000)
            self.uc.mem_write(at + 0x24, id.encode() + b'\0')
            self.put(at + 0xEE4, 50)
        self.vector(0xA83C68, pointers, 0x10160000)
        buildings = []
        for i, type_index in enumerate((1, 2) + (4,) * 8):
            b = self.object(0x10080000 + i * 0x2000, self.ai)
            self.uc.mem_write(b + 0x74, b'\x01')
            self.uc.mem_write(b + 0x90, b'\x01')
            self.put(b + 0x520, pointers[type_index])  # BuildingClass::Type
            buildings.append(b)
        self.vector(0xA8EB40, buildings, 0x10161000)
        self.imports.update({0x4F7870: 'buildable', 0x464AC0: 'can_place',
                             0x31009000: 'cost', 0x31009010: 'place', 0x31009020: 'build_original'})
        self.put(0x10130000 + 0xAC, 0x31009000)
        self.call('patch_oil_defenses')
        self.put(self.symbols['_oil_place_original'], 0x31009010)
        self.put(self.symbols['_oil_build_original'], 0x31009020)
        return pointers, buildings

    def test_shipyard_request_uses_native_placement_and_real_production(self):
        pointers, _ = self.expansion_fixture()
        self.assertEqual(self.call(0x4FE3E0, this=self.ai, clean=0), 123)
        self.assertEqual(self.get(self.ai + 0x564C), 0)  # GAYARD registry index
        self.assertEqual(self.actions[0][1][1:], [pointers[0], 0x505F80, 0xFFFFFFFF])

    def test_blocked_coast_and_low_cash_do_not_stall_production(self):
        self.expansion_fixture()
        self.can_place = False
        self.call(0x4FE3E0, this=self.ai)
        self.assertEqual(self.get(self.ai + 0x564C), 0xFFFFFFFF)
        self.assertTrue(self.actions)  # placement was actually checked
        self.actions.clear()
        self.put(0xA8ED84, 4000)
        self.put(self.ai + 0x30C, 2000)
        self.call(0x4FE3E0, this=self.ai)
        self.assertEqual(self.get(self.ai + 0x564C), 0xFFFFFFFF)
        self.assertFalse(self.actions)

    def test_second_factory_and_airbase_use_normal_queue_and_stop_at_two(self):
        pointers, buildings = self.expansion_fixture()
        # Find a seed choosing both, with the same defined unsigned arithmetic.
        for seed in range(1, 100):
            n = seed ^ (2 * 0x9E3779B9 & 0xFFFFFFFF)
            n ^= n >> 16
            n = n * 0x85EBCA6B & 0xFFFFFFFF
            n ^= n >> 13
            if n & 3 == 3:
                self.put(0xA8ED94, seed)
                break
        for frame, owned_type, expected_index in ((3000, 0, 1), (4000, 1, 2), (5000, 2, None)):
            b = self.object(0x100A0000 + len(buildings) * 0x2000, self.ai)
            self.uc.mem_write(b + 0x74, b'\x01')
            self.uc.mem_write(b + 0x90, b'\x01')
            self.put(b + 0x520, pointers[owned_type])
            buildings.append(b)
            self.vector(0xA8EB40, buildings, 0x10161000)
            self.put(self.ai + 0x564C, -1)
            self.put(0xA8ED84, frame)
            self.call(0x4FE3E0, this=self.ai)
            self.assertEqual(self.get(self.ai + 0x564C),
                             0xFFFFFFFF if expected_index is None else expected_index)


if __name__ == '__main__':
    unittest.main()
