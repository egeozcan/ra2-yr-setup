"""Native policy checks and hook-site checks against the installed 1.001 exe."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
GAME = Path('/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II')


class OilDefenseTests(unittest.TestCase):
    def test_native_decisions(self):
        source = r'''
        #include <assert.h>
        #include "oil-defense-policy.h"
        int main(void) {
            assert(oil_ai_active(5, 0, 0, 0, 1, 1, 1));
            assert(!oil_ai_active(5, 1, 0, 0, 1, 1, 1));
            assert(!oil_ai_active(5, 0, 1, 0, 1, 1, 1));
            assert(!oil_ai_active(5, 0, 2, 0, 1, 1, 1));
            assert(!oil_ai_active(0, 0, 0, 0, 1, 1, 1));
            assert(!oil_ai_active(4, 0, 0, 0, 1, 1, 1));
            assert(!oil_ai_active(5, 0, 0, 1, 1, 1, 1));
            assert(!oil_ai_active(5, 0, 0, 0, 0, 1, 1));
            assert(!oil_ai_active(5, 0, 0, 0, 1, 0, 1));
            assert(!oil_ai_active(5, 0, 0, 0, 1, 1, 0));
            assert(oil_defense_need(0, 0, 0, 0, 0) == OIL_NONE);
            assert(oil_defense_need(3, 0, 0, 0, 0) == OIL_GROUND);
            assert(oil_defense_need(3, 0, 1, 0, 1) == OIL_GROUND);
            assert(oil_defense_need(3, 0, 2, 0, 2) == OIL_NONE);
            assert(oil_defense_need(3, 1, 2, 0, 2) == OIL_AIR);
            assert(oil_defense_need(3, 1, 2, 1, 3) == OIL_NONE);
            assert(oil_defense_need(9, 9, 0, 0, 3) == OIL_NONE);
            assert(oil_defense_need(0, 3, 1, 1, 1) == OIL_NONE); // Gatling covers both
            assert(oil_defense_need(3, 0, 0, 1, 1) == OIL_GROUND); // replace lost gun
            assert(oil_can_budget(3000, 1500, 300, 200, 75, 1));
            assert(!oil_can_budget(2999, 1500, 300, 200, 75, 1));
            assert(!oil_can_budget(3000, 1500, 250, 200, 75, 1));
            assert(!oil_can_budget(3000, 1500, 300, 200, 75, -1));
            assert(!oil_can_budget(3000, 1500, 300, 200, 75, 0));
            assert(oil_can_budget(2000, 500, 200, 200, 0, 1));
            assert(oil_near(30, 40, 42, 40, OIL_THREAT_RADIUS));
            assert(!oil_near(30, 40, 43, 40, OIL_THREAT_RADIUS));
            assert(!oil_near(30, 40, 35, 45, OIL_DEFENSE_RADIUS));
            assert(oil_near(30, 40, 34, 44, OIL_DEFENSE_RADIUS));
        }
        '''
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp/'policy.c').write_text(source)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                            '-I', str(HERE), str(tmp/'policy.c'), '-o', str(tmp/'policy')], check=True)
            subprocess.run([str(tmp/'policy')], check=True)

    def test_verified_executable_hook_boundaries(self):
        data = (GAME/'gamemd.exe').read_bytes()
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        n = struct.unpack_from('<H', data, pe+6)[0]
        opt_size = struct.unpack_from('<H', data, pe+20)[0]
        base = struct.unpack_from('<I', data, pe+52)[0]
        sections = pe+24+opt_size

        def read(address, size):
            for i in range(n):
                virtual_size, rva, raw_size, raw = struct.unpack_from('<IIII', data, sections+i*40+8)
                if rva <= address-base < rva+min(virtual_size, raw_size):
                    offset = raw+address-base-rva
                    return data[offset:offset+size]
            self.fail(f'No section at {address:x}')

        self.assertEqual(read(0x4FE3E0, 7), bytes.fromhex('83 ec 30 53 55 8b e9'))
        self.assertEqual(read(0x5060B0, 6), bytes.fromhex('55 8b ec 83 e4 f8'))
        # Native caller consumes the requested building index and begins normal factory production.
        self.assertEqual(read(0x4F90B3, 6), bytes.fromhex('8b 86 4c 56 00 00'))
        # Structure sizes used for terrain placement use the stock thiscall entry points.
        self.assertEqual(read(0x464AC0, 6), bytes.fromhex('8a 81 03 17 00 00'))


if __name__ == '__main__':
    unittest.main()
