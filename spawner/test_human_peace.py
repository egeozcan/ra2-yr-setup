"""Config checks plus execution of the actual 32-bit guards in an x86 emulator.

Run with unittest. Engine checks need unicorn, pefile and capstone, the installed
1.001 exe, and a symbol-bearing DLL built without -s (PEACE_TEST_DLL overrides
its location). They do not start or interfere with a running game.
"""
import os
from pathlib import Path
import re
import struct
import subprocess
import unittest

HERE = Path(__file__).resolve().parent
GAME = Path('/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II')
DLL = Path(os.environ.get('PEACE_TEST_DLL', HERE / 'yspawn-test.dll'))


class SettingsTests(unittest.TestCase):
    def test_defaults_presets_and_written_match_settings(self):
        subprocess.run(['/usr/bin/python3', '-c', '''
import configparser
from unittest.mock import patch
import skirmish
s = skirmish.default_settings()
assert s['HumanInPeace'] is False
assert skirmish.normalize({'Name': 'Old preset'})['HumanInPeace'] is False
for enabled in (False, True):
    s['HumanInPeace'] = enabled
    restored = skirmish.normalize(s)
    assert restored['HumanInPeace'] is enabled
    ini = skirmish.build_config(restored)
    assert ini['Settings']['HumanInPeace'] == str(int(enabled))
    assert ini['Settings']['Team'] == str(s['Team'])
    assert ini['AI1']['Team'] == str(s['AI'][0]['Team'])
    assert ini['Settings']['Superweapons'] == '1'
old = configparser.ConfigParser()
old.read_dict({'Settings': {}, 'AI1': {}})
with patch.object(skirmish.spawn, 'read_config', return_value=old):
    assert skirmish.default_settings()['HumanInPeace'] is False
'''], cwd=HERE, check=True)


class EngineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import capstone
            import pefile
            import unicorn
            from unicorn import x86_const
        except ImportError:
            raise unittest.SkipTest('Engine tests require unicorn, pefile and capstone')
        if not DLL.exists() or not (GAME / 'gamemd.exe').exists():
            raise unittest.SkipTest('Engine tests require a symbol-bearing DLL and the installed game')
        cls.pefile, cls.unicorn, cls.reg = pefile, unicorn, x86_const
        cls.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        cls.dll = pefile.PE(str(DLL))
        symbols = subprocess.check_output(['objdump', '-t', str(DLL)], text=True)
        cls.symbols = {}
        for section, value, name in re.findall(r'\(sec\s+(\d+)\).*?0x([0-9a-f]+) (_\S+)', symbols):
            if int(section) > 0:
                cls.symbols[name] = (cls.dll.OPTIONAL_HEADER.ImageBase
                                    + cls.dll.sections[int(section)-1].VirtualAddress + int(value, 16))

    def setUp(self):
        u = self.unicorn
        self.uc = u.Uc(u.UC_ARCH_X86, u.UC_MODE_32)
        for pe in (self.pefile.PE(str(GAME / 'gamemd.exe')), self.dll):
            size = (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095
            self.uc.mem_map(pe.OPTIONAL_HEADER.ImageBase, size)
            self.uc.mem_write(pe.OPTIONAL_HEADER.ImageBase, pe.get_memory_mapped_image())
        self.uc.mem_map(0x10000000, 0x200000)  # houses and objects
        self.uc.mem_map(0x20000000, 0x100000)  # trampolines
        self.uc.mem_map(0x30000000, 0x100000)  # stack
        self.uc.mem_map(0x31000000, 0x10000)   # imports and sentinel
        self.stop = 0x3100F000
        self.imports, self.calls, self.next_alloc = {}, [], 0x20000000
        for desc in self.dll.DIRECTORY_ENTRY_IMPORT:
            for imp in desc.imports:
                address = 0x31000000 + len(self.imports) * 16
                self.imports[address] = imp.name.decode() if imp.name else str(imp.ordinal)
                self.put(imp.address, address)
                self.uc.mem_write(address, b'\xc3')
        self.uc.hook_add(u.UC_HOOK_CODE, self.on_code)
        self.human, self.ai, self.ai2 = 0x10000000, 0x10020000, 0x10040000
        self.uc.mem_write(self.human + 0x1EC, b'\x01')
        for i, house in enumerate((self.human, self.ai, self.ai2)):
            self.put(house + 0x30, i)
            self.put(house + 0x34, 0x10100000 + 0x200 * i)
        self.put(0xA8B238, 5)
        self.vector(0xA80228, [self.human, self.ai, self.ai2], 0x10110000)
        self.player_obj = self.object(0x10060000, self.human)
        self.enemy_obj = self.object(0x10061000, self.ai2)
        self.firer = self.object(0x10062000, self.ai)
        self.vector(0xA8EC78, [self.player_obj, self.enemy_obj, self.firer], 0x10111000)

    def put(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value & 0xFFFFFFFF))

    def get(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def vector(self, address, items, storage):
        self.put(address + 4, storage)
        self.put(address + 0x10, len(items))
        for i, item in enumerate(items):
            self.put(storage + 4*i, item)

    def object(self, address, owner):
        self.put(address, 0x7F5C70)
        self.put(address + 0x14, 7)
        self.put(address + 0x21C, owner)
        self.put(address + 0x6C, 100)
        self.put(address + 0x9C, 50*256 + 128)
        self.put(address + 0xA0, 50*256 + 128)
        return address

    def on_code(self, uc, address, size, _):
        if address not in self.imports:
            return
        name = self.imports[address]
        esp = uc.reg_read(self.reg.UC_X86_REG_ESP)
        args = [self.get(esp + 4 + 4*i) for i in range(8)]
        clean, value = 0, 1
        if name == 'memcmp':
            a, b = bytes(uc.mem_read(args[0], args[2])), bytes(uc.mem_read(args[1], args[2]))
            value = (a > b) - (a < b)
        elif name == 'memcpy':
            uc.mem_write(args[0], bytes(uc.mem_read(args[1], args[2])))
            value = args[0]
        elif name == 'memset':
            uc.mem_write(args[0], bytes([args[1] & 255]) * args[2])
            value = args[0]
        elif name == 'VirtualAlloc':
            value = self.next_alloc
            self.next_alloc += (args[1]+4095) & ~4095
            clean = 16
        elif name == 'VirtualProtect':
            self.put(args[3], 0x20)
            clean = 16
        elif name == 'FlushInstructionCache':
            clean = 12
        elif name == 'GetCurrentProcess':
            value = -1
        elif name.startswith('original:'):
            clean = int(name.split(':')[1])
            self.calls.append((uc.reg_read(self.reg.UC_X86_REG_ECX), args))
            value = 123
        else:
            raise AssertionError(f'Unexpected import: {name}')
        uc.reg_write(self.reg.UC_X86_REG_EAX, value & 0xFFFFFFFF)
        uc.reg_write(self.reg.UC_X86_REG_EIP, self.get(esp))
        uc.reg_write(self.reg.UC_X86_REG_ESP, esp + 4 + clean)

    def call(self, address, this=0, args=(), clean=None):
        if isinstance(address, str):
            address = self.symbols['_' + address]
        esp = 0x30080000
        self.uc.mem_write(esp, struct.pack('<' + 'I'*(len(args)+1), self.stop, *args))
        self.uc.reg_write(self.reg.UC_X86_REG_ESP, esp)
        self.uc.reg_write(self.reg.UC_X86_REG_ECX, this)
        self.uc.emu_start(address, self.stop, count=1000000)
        self.assertEqual(self.uc.reg_read(self.reg.UC_X86_REG_EIP), self.stop)
        if clean is not None:
            self.assertEqual(self.uc.reg_read(self.reg.UC_X86_REG_ESP), esp+4+clean)
        return self.uc.reg_read(self.reg.UC_X86_REG_EAX)

    def install(self):
        self.assertEqual(self.call('patch_human_peace', clean=0), 1)

    def original(self, pointer, clean):
        address = 0x3100E000 + len(self.calls)*16
        self.imports[address] = f'original:{clean}'
        self.put(pointer, address)

    def test_all_hook_instructions_match_and_end_at_boundaries(self):
        source = (HERE/'human-peace.h').read_text()
        for address, byte_list, size in re.findall(r'\{(0x[0-9A-F]+), \{([^}]+)\}, (\d+), peace_', source):
            expected = bytes(int(b, 0) for b in byte_list.split(','))
            address, size = int(address, 0), int(size)
            self.assertEqual(bytes(self.uc.mem_read(address, size)), expected)
            instructions = list(self.cs.disasm(expected, address))
            self.assertEqual(sum(i.size for i in instructions), size)
            self.assertFalse(any(i.mnemonic.startswith(('j', 'call', 'ret')) for i in instructions))
        self.install()  # also validates all 24 vtable slots

    def test_unknown_executable_refuses_all_peace_patches(self):
        before = bytes(self.uc.mem_read(0x6FCDB0, 5))
        self.uc.mem_write(0x6CC390, b'\x90')
        self.assertEqual(self.call('patch_human_peace'), 0)
        self.assertEqual(bytes(self.uc.mem_read(0x6FCDB0, 5)), before)
        self.assertEqual(self.get(0x7F5C70+0x3CC), 0x741340)

    def test_magnetron_carry_wrapper_remains_in_the_vehicle_fire_chain(self):
        self.call('patch_magnetron')
        carry = self.symbols['_unit_fire']
        self.assertEqual(self.get(0x7F603C), carry)
        self.install()
        original = self.symbols['_peace_original']
        self.assertEqual(self.get(original + 4*(6*3+2)), carry)
        self.assertEqual(self.call(self.get(0x7F603C), self.firer, (self.player_obj, 0), 8), 0)

    def test_every_class_blocks_attacks_damage_capture_and_allows_ai_combat(self):
        self.install()
        original = self.symbols['_peace_original']
        damage = 0x10120000
        self.put(damage, 100)
        for i, vtable in enumerate((0x7E22A4, 0x7E3EBC, 0x7EB058, 0x7F5C70)):
            with self.subTest(kind=i):
                self.put(self.firer, vtable)
                self.put(self.player_obj, vtable)
                self.assertEqual(self.call(self.get(vtable+0x3CC), self.firer,
                                           (self.player_obj, 0), 8), 0)
                self.assertEqual(self.call(self.get(vtable+0x3C0), self.firer,
                                           (self.player_obj, 0, 0), 12), 5)
                self.assertEqual(self.call(self.get(vtable+0x16C), self.player_obj,
                                           (damage, 0, 0, 0, 0, 0, 0), 28), 0)
                self.assertEqual(self.get(damage), 100)  # never zero a shared area-damage argument
                self.assertEqual(self.call(self.get(vtable+0x3D4), self.player_obj, (self.ai, 1), 8), 0)
                for j, slot, args, clean in (
                    (1, 0x3C8, (self.player_obj,), 4),
                    (2, 0x3CC, (self.enemy_obj, 0), 8),
                    (3, 0x3D4, (self.human, 1), 8),
                    (4, 0x3C0, (self.enemy_obj, 0, 0), 12),
                    (5, 0x480, (self.player_obj, 0), 8),
                ):
                    self.original(original + 4*(6*i+j), clean)
                    self.call(self.get(vtable+slot), self.firer, args, clean)
                    self.assertEqual(self.calls[-1][0], self.firer)
                    self.assertEqual(self.calls[-1][1][0], 0 if j in (1, 5) else args[0])
                self.original(original + 4*(6*i), 28)
                self.put(damage, 0xFFFFFF9C)  # repair
                self.call(self.get(vtable+0x16C), self.player_obj, (damage, 0, 0, 0, 0, 0, 0), 28)
                self.assertEqual(self.calls[-1][0], self.player_obj)
                self.put(damage, 100)

    def test_target_evaluation_and_capture_manager_exclude_human(self):
        self.install()
        threat = 0x10120000
        self.put(threat, 999)
        self.assertEqual(self.call(0x6F7CA0, self.firer,
                                   (0, 0, 0, self.player_obj, threat, 0, 0), 28), 0)
        self.assertEqual(self.get(threat), 0)
        manager = 0x10121000
        self.put(manager+0x48, self.firer)
        for address in (0x471C90, 0x471D40):
            self.assertEqual(self.call(address, manager, (self.player_obj,), 4), 0)
        # A cell is a smaller object with no Techno owner.
        cell = 0x10122000
        self.assertEqual(self.call('peace_protected', args=(cell,)), 0)

    def test_superweapons_avoid_human_area_but_can_fire_elsewhere(self):
        self.install()
        super, target = 0x10120000, 0x10120100
        self.put(super+0x2C, self.ai)
        self.original(self.symbols['_peace_sw_original'], 8)
        for x, y, blocked in ((50,50,True), (82,82,True), (83,50,False), (5,5,False)):
            self.uc.mem_write(target, struct.pack('<hh', x, y))
            before = len(self.calls)
            self.call(0x6CC390, super, (target, 0), 8)
            self.assertEqual(len(self.calls) == before, blocked)
        # Human superweapons retain their normal launch path.
        self.put(super+0x2C, self.human)
        self.uc.mem_write(target, struct.pack('<hh', 50, 50))
        before = len(self.calls)
        self.call(0x6CC390, super, (target, 1), 8)
        self.assertEqual(len(self.calls), before+1)

    def test_anger_uses_other_ai_and_one_opponent_skips_superweapon_selectors(self):
        self.install()
        nodes = 0x10120000
        self.put(self.ai+0x5608, nodes)
        self.put(self.ai+0x5614, 2)
        self.put(nodes, self.human)
        self.put(nodes+4, 10000)
        self.put(nodes+8, self.ai2)
        self.put(nodes+12, 50)
        self.call(0x504790, self.ai, (100, self.human), 8)
        self.assertEqual(self.get(nodes+4), 0)
        self.assertEqual(self.get(self.ai+0x5600), 2)
        self.put(self.ai+0x5600, 0)  # stale human enemy
        self.original(self.symbols['_peace_ai_sw_original'], 0)
        self.call(0x5098F0, self.ai, clean=0)
        self.assertEqual(self.get(self.ai+0x5600), 2)
        self.assertEqual(self.calls[-1][0], self.ai)
        self.put(0xA80238, 2)  # just the human and one AI
        self.put(self.ai+0x5600, 0)
        before = len(self.calls)
        self.call(0x5098F0, self.ai, clean=0)
        self.assertEqual(self.get(self.ai+0x5600), 0xFFFFFFFF)
        self.assertEqual(len(self.calls), before)
        for house in (self.human, self.ai, self.ai2):
            self.assertEqual(self.get(house+0x5788), 0)  # no alliance was invented

    def test_initial_enemy_candidate_preserves_registers(self):
        self.install()
        for candidate, expected in ((self.human, 0x4FD6FE), (self.ai2, 0x4FD61F)):
            self.uc.reg_write(self.reg.UC_X86_REG_EBX, self.ai)
            self.uc.reg_write(self.reg.UC_X86_REG_ESI, candidate)
            self.uc.reg_write(self.reg.UC_X86_REG_EDI, 0x11223344)
            self.uc.reg_write(self.reg.UC_X86_REG_ESP, 0x30080000)
            self.uc.emu_start(0x4FD616, expected, count=100000)
            self.assertEqual(self.uc.reg_read(self.reg.UC_X86_REG_EIP), expected)
            self.assertEqual(self.uc.reg_read(self.reg.UC_X86_REG_ESP), 0x30080000)
            self.assertEqual(self.uc.reg_read(self.reg.UC_X86_REG_EDI), 0x11223344)

    def test_delayed_dominator_skips_all_control_side_effects(self):
        self.install()
        self.put(0xA9FAC8, self.ai)
        for target, expected in ((self.player_obj, 0x53B364), (self.enemy_obj, 0x53B27C)):
            self.uc.reg_write(self.reg.UC_X86_REG_ESI, target)
            self.uc.reg_write(self.reg.UC_X86_REG_EDI, 0x11223344)
            self.uc.reg_write(self.reg.UC_X86_REG_ESP, 0x30080000)
            self.uc.emu_start(0x53B276, expected, count=100000)
            self.assertEqual(self.uc.reg_read(self.reg.UC_X86_REG_EIP), expected)
            self.assertEqual(self.uc.reg_read(self.reg.UC_X86_REG_ESP), 0x30080000)
            self.assertEqual(self.uc.reg_read(self.reg.UC_X86_REG_EDI), 0x11223344)


if __name__ == '__main__':
    unittest.main()
