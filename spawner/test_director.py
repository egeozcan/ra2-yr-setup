"""Brutal strategy director: pure policy checks and hook boundaries.

The director's live behaviour is measured with bench.py (AI-vs-AI matches); these tests pin the
decision rules and confirm the production hooks still match the installed executable.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
GAME = Path('/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II')


class PolicyTests(unittest.TestCase):
    def test_policy(self):
        source = r'''
        #include <assert.h>
        #include "director-policy.h"
        int main(void) {
            /* launch: needs a real army and an edge over the enemy, but never waits forever */
            assert(!dir_should_launch(70000, 5, 0, 0, 30000));
            assert(!dir_should_launch(4999, 10, 0, 0, 1000));
            assert(dir_should_launch(5000, 10, 4000, 0, 1000));
            assert(!dir_should_launch(5000, 10, 4500, 0, 1000));
            assert(!dir_should_launch(5000, 10, 3000, 3000, 1000));   /* defenses count half */
            assert(!dir_should_launch(7000, 10, 0, 0, 13000));
            assert(dir_should_launch(8000, 10, 0, 0, 13000));
            assert(!dir_should_launch(30000, 60, 90000, 0, 13000));   /* no count override */
            assert(dir_should_launch(60000, 10, 90000, 0, 13000));
            /* retreat: losing at the front after losses, or bled out against resistance */
            assert(!dir_should_retreat(1000, 10000, 1000, 0));
            assert(dir_should_retreat(2900, 10000, 2900, 100));
            assert(dir_should_retreat(6900, 10000, 3000, 4000));
            assert(!dir_should_retreat(7000, 10000, 3000, 4000));
            assert(!dir_should_retreat(6900, 10000, 4000, 5000));
            /* defend: ignore scouts; while attacking only big raids recall the army */
            assert(!dir_base_threat(1499, 0, DIR_GATHER));
            assert(dir_base_threat(1500, 10000, DIR_GATHER));
            assert(!dir_base_threat(2000, 10000, DIR_ATTACK));
            assert(dir_base_threat(6000, 10000, DIR_ATTACK));
            assert(dir_base_threat(800, 10000, DIR_DEFEND) && !dir_base_threat(799, 10000, DIR_DEFEND));
            /* composition follows the enemy */
            int s[ROLE_COUNT];
            dir_role_shares(0, 0, 10000, 0, s);
            assert(s[ROLE_AA] == 10 && s[ROLE_SUPPORT] == 10 && s[ROLE_SIEGE] == 10 && s[ROLE_MAIN] == 70);
            dir_role_shares(10000, 0, 0, 7000, s);
            assert(s[ROLE_AA] >= 49 && s[ROLE_SIEGE] == 25 && s[ROLE_MAIN] == 25);
            dir_role_shares(0, 10000, 0, 3000, s);
            assert(s[ROLE_SUPPORT] >= 39 && s[ROLE_SIEGE] == 18);
            int have[ROLE_COUNT] = { 7000, 0, 1000, 1000 }, all[ROLE_COUNT] = { 1, 1, 1, 1 };
            dir_role_shares(0, 0, 10000, 0, s);
            assert(dir_pick_role(s, have, all) == ROLE_AA);
            int no_aa[ROLE_COUNT] = { 1, 0, 1, 1 };
            assert(dir_pick_role(s, have, no_aa) != ROLE_AA);
            int none[ROLE_COUNT] = { 0, 0, 0, 0 };
            assert(dir_pick_role(s, have, none) == -1);
            /* factories scale with idle cash, only after the opening */
            assert(dir_wanted_factories(50000, 1000, 2) == 1);
            assert(dir_wanted_factories(4000, 5000, 2) == 1);
            assert(dir_wanted_factories(5000, 5000, 1) == 2);
            assert(dir_wanted_factories(12000, 5000, 1) == 3);
            assert(dir_wanted_factories(25000, 5000, 1) == 3);
            assert(dir_wanted_factories(25000, 5000, 2) == 4);
            assert(dir_can_spend(3500, 1000, 2500) && !dir_can_spend(3499, 1000, 2500));
            assert(dir_want_refinery(16000, 3, 3, 0, 5000));
            assert(!dir_want_refinery(16000, 3, 3, 1, 5000));   /* idle harvesters: ore gone or cut off */
            assert(!dir_want_refinery(16000, 4, 4, 0, 5000) && !dir_want_refinery(5000, 1, 1, 0, 5000));
            assert(!dir_want_refinery(16000, 3, 3, 0, 12000));
        }
        '''
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'policy.c').write_text(source)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I', str(HERE),
                            str(tmp / 'policy.c'), '-o', str(tmp / 'policy')], check=True)
            subprocess.run([str(tmp / 'policy')], check=True)


class HookTests(unittest.TestCase):
    @unittest.skipUnless((GAME / 'gamemd.exe').exists(), 'game not installed')
    def test_production_hook_prologues_match_executable(self):
        import pefile
        pe = pefile.PE(str(GAME / 'gamemd.exe'))
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        prologue = bytes([0x81, 0xEC, 0xDC, 0x04, 0x00, 0x00])
        for address in (0x4FEA60, 0x4FEEE0):   # unit and infantry production pickers
            self.assertEqual(image[address - base:address - base + 6], prologue, hex(address))
        # LiberateMember reads FootClass::Team at +0x5D4 and returns with ret 0xC (three arguments)
        self.assertEqual(image[0x6EA879 - base:0x6EA87F - base], bytes([0x8B, 0x85, 0xD4, 0x05, 0, 0]))
        self.assertEqual(image[0x6EA88F - base:0x6EA892 - base], bytes([0xC2, 0x0C, 0x00]))
        # SetFocus is a plain store to TechnoClass+0x218, which Area_Guard reads as its centre
        self.assertEqual(image[0x70C614 - base:0x70C61A - base], bytes([0x89, 0x81, 0x18, 0x02, 0, 0]))


if __name__ == '__main__':
    unittest.main()
