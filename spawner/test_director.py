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
            assert(!dir_should_launch(70000, 5, 0, 0, 30000, 0));
            assert(!dir_should_launch(4999, 10, 0, 0, 1000, 0));
            assert(dir_should_launch(5000, 10, 4000, 0, 1000, 0));
            assert(!dir_should_launch(5000, 10, 4500, 0, 1000, 0));
            assert(!dir_should_launch(5000, 10, 3000, 3000, 1000, 0));   /* defenses count half */
            assert(!dir_should_launch(7000, 10, 0, 0, 13000, 0));
            assert(dir_should_launch(8000, 10, 0, 0, 13000, 0));
            assert(!dir_should_launch(30000, 60, 90000, 0, 13000, 0));   /* no count override */
            assert(!dir_should_launch(60000, 10, 90000, 0, 13000, 0));   /* big, but outnumbered */
            assert(!dir_should_launch(60000, 10, 55000, 0, 13000, 0));   /* the defender wins at 1.1 */
            assert(dir_should_launch(60000, 10, 50000, 0, 13000, 0));
            assert(dir_should_launch(100000, 10, 99000, 0, 13000, 0));   /* parity from 100000 */
            assert(!dir_should_launch(99000, 10, 98000, 0, 13000, 0));
            assert(dir_should_launch(150000, 10, 180000, 0, 13000, 0));  /* 0.8 from 150000 */
            /* a long wait lowers the edge the army asks for, never below 0.8 of the opposition */
            assert(!dir_should_launch(40000, 50, 40000, 0, 30000, 5999));
            assert(dir_should_launch(40000, 50, 40000, 0, 30000, 6000));
            assert(!dir_should_launch(40000, 50, 50001, 0, 30000, 12000));
            assert(dir_should_launch(40000, 50, 50000, 0, 30000, 12000));
            /* enemy choice: a weak neighbour before a strong one, but not across the whole map */
            assert(dir_enemy_score(40, 20000) < dir_enemy_score(40, 40000));
            assert(dir_enemy_score(40, 30000) < dir_enemy_score(150, 15000));
            /* regroup: a forward section outgunned 2:1, with the body behind able to win together */
            assert(dir_should_regroup(2000, 5000, 10, 20000));
            assert(!dir_should_regroup(3000, 5000, 10, 20000));   /* not overwhelming */
            assert(!dir_should_regroup(2000, 5000, 4, 20000));    /* it is the body */
            assert(!dir_should_regroup(2000, 5000, 10, 6000));    /* the whole army is losing: retreat */
            assert(!dir_should_regroup(200, 1000, 10, 20000));    /* a skirmish */
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
            assert(dir_want_refinery(16000, 1, 2, 0, 40000));   /* one refinery: whatever the bank */
            assert(dir_want_refinery_plan(16000, 3, 3, 0, 40000, 1, 0));   /* a boom builds them rich */
            /* strategy: plan weights follow the setup */
            int w[PLAN_COUNT];
            dir_plan_weights(50, 1, 0, 0, w);                      /* a close duel: rush is possible */
            assert(w[PLAN_RUSH] > 0 && w[PLAN_RUSH] > w[PLAN_BOOM]);
            dir_plan_weights(50, 1, 3, 0, w);                      /* ...but not from a built base */
            assert(w[PLAN_RUSH] < w[PLAN_BALANCED] / 4);
            dir_plan_weights(-1, 1, 0, 0, w);                      /* nobody on foot: no rush */
            assert(w[PLAN_RUSH] == 0 && w[PLAN_BOOM] >= 20);
            dir_plan_weights(60, 6, 3, 0, w);                      /* a big free-for-all: rush is rare */
            assert(w[PLAN_RUSH] < 15 && w[PLAN_BOOM] >= 20 && w[PLAN_SIEGE] == 25 && w[PLAN_NAVAL] == 0);
            dir_plan_weights(74, 1, 0, 0, w);                   /* a duel at medium range: no boom */
            assert(w[PLAN_BOOM] == 0);
            dir_plan_weights(-1, 6, 3, 1, w);                   /* islands: the fleet is the way over */
            assert(w[PLAN_NAVAL] >= w[PLAN_BALANCED] && w[PLAN_RUSH] == 0);
            /* later plans: siege against a fortress, boom when behind in refineries, never a rush */
            dir_replan_weights(9000, 4000, 3, 3, 60, 0, 1, w);
            assert(w[PLAN_SIEGE] > w[PLAN_BALANCED] && w[PLAN_RUSH] == 0 && w[PLAN_NAVAL] == 0);
            dir_replan_weights(1000, 20000, 2, 4, 60, 0, 3, w);
            assert(w[PLAN_SIEGE] < w[PLAN_BALANCED] && w[PLAN_BOOM] == 30);
            dir_replan_weights(1000, 20000, 2, 4, 60, 0, 1, w);   /* a duel within reach: no boom */
            assert(w[PLAN_BOOM] == 0);
            dir_replan_weights(1000, 20000, 4, 4, -1, 1, 1, w);
            assert(w[PLAN_NAVAL] == 35 && w[PLAN_BOOM] == 5);
            /* the draw covers every plan with weight, and only those */
            int seen[PLAN_COUNT] = { 0 }, none_rush[PLAN_COUNT] = { 10, 0, 10, 10, 0 };
            for (unsigned r = 0; r < 1000; r++) {
                seen[dir_plan_pick(none_rush, r * 2654435761u)]++;
            }
            assert(seen[PLAN_RUSH] == 0 && seen[PLAN_NAVAL] == 0 && seen[PLAN_BALANCED] > 200 && seen[PLAN_BOOM] > 200 && seen[PLAN_SIEGE] > 200);
            int zero[PLAN_COUNT] = { 0 };
            assert(dir_plan_pick(zero, 7) == PLAN_BALANCED);
            /* balanced levers are the director as it was */
            const DirPlanLevers *b = &dir_plan_levers[PLAN_BALANCED];
            for (int a = 4000; a < 70000; a += 1500)
                for (int e = 0; e < 60000; e += 2500)
                    for (int wt = 0; wt <= 12000; wt += 6000)
                        assert(dir_should_launch(a, 10, e, e / 3, 13000, wt)
                               == dir_should_launch_plan(a, 10, e, e / 3, 13000, wt, b->floor_pct, b->edge, b->min_units));
            /* a rush launches a small army early; a boom waits for a bigger one */
            const DirPlanLevers *rush = &dir_plan_levers[PLAN_RUSH], *boom = &dir_plan_levers[PLAN_BOOM];
            assert(dir_should_launch_plan(2500, 5, 2000, 0, 5000, 0, rush->floor_pct, rush->edge, rush->min_units));
            assert(!dir_should_launch(2500, 5, 2000, 0, 5000, 0));
            assert(!dir_should_launch_plan(6000, 10, 2000, 0, 5000, 0, boom->floor_pct, boom->edge, boom->min_units));
            assert(dir_should_launch_plan(8000, 10, 2000, 0, 5000, 0, boom->floor_pct, boom->edge, boom->min_units));
            /* the plan bends the mix and the schedule */
            dir_role_shares(0, 0, 10000, 0, s);
            dir_plan_shares(&dir_plan_levers[PLAN_SIEGE], s);
            assert(s[ROLE_SIEGE] == 25 && s[ROLE_SUPPORT] == 5 && s[ROLE_MAIN] + s[ROLE_AA] + s[ROLE_SIEGE] + s[ROLE_SUPPORT] == 100);
            assert(dir_want_refinery_plan(3500, 2, 2, 0, 5000, boom->refinery_bonus, boom->refinery_early));
            assert(!dir_want_refinery(3500, 2, 2, 0, 5000));
            assert(dir_want_refinery_plan(16000, 4, 4, 0, 5000, 1, 0) && !dir_want_refinery_plan(16000, 5, 5, 0, 5000, 1, 0));
            /* posture: hold against a stronger army near home, press a clear lead, strike an empty base */
            assert(dir_posture(POSTURE_NORMAL, 10000, 8000, 16000, 8000, 8000, 1) == POSTURE_HOLD);
            assert(dir_posture(POSTURE_NORMAL, 10000, 8000, 14000, 8000, 8000, 1) == POSTURE_NORMAL);
            assert(dir_posture(POSTURE_HOLD, 10000, 8000, 11001, 8000, 8000, 1) == POSTURE_HOLD);   /* hysteresis */
            assert(dir_posture(POSTURE_HOLD, 10000, 8000, 10900, 8000, 8000, 1) == POSTURE_NORMAL);
            assert(dir_posture(POSTURE_NORMAL, 3000, 0, 3000, 0, 0, 1) == POSTURE_NORMAL);       /* a scout is no threat */
            assert(dir_posture(POSTURE_NORMAL, 16000, 10000, 0, 10000, 10000, 1) == POSTURE_PRESS);
            assert(dir_posture(POSTURE_NORMAL, 15900, 10000, 0, 10000, 10000, 1) == POSTURE_NORMAL);
            assert(dir_posture(POSTURE_PRESS, 13000, 10000, 0, 10000, 10000, 1) == POSTURE_PRESS);
            assert(dir_posture(POSTURE_NORMAL, 5000, 10000, 0, 10000, 3000, 2) == POSTURE_OPPORTUNITY);
            assert(dir_posture(POSTURE_NORMAL, 5000, 10000, 0, 10000, 3000, 1) == POSTURE_NORMAL);   /* duel: it's coming */
            assert(dir_posture(POSTURE_NORMAL, 5000, 10000, 2600, 10000, 3000, 2) == POSTURE_NORMAL); /* toward us */
            assert(dir_posture(POSTURE_NORMAL, 5000, 10000, 0, 10000, 3500, 2) == POSTURE_NORMAL);
            assert(dir_posture(POSTURE_NORMAL, 2000, 2000, 0, 2000, 0, 1) == POSTURE_NORMAL);   /* too small to matter */
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
        # The factory AI asks the house for its pick: building type in EDX (mov edx,[esi+0x520]), the
        # factory RTTI and a constant 0 pushed, then call 0x4FBD80, which returns with ret 8
        self.assertEqual(image[0x450319 - base:0x450332 - base],
                         bytes([0x8B, 0x96, 0x20, 0x05, 0, 0, 0x8B, 0x8E, 0x1C, 0x02, 0, 0, 0x53,
                                0x8B, 0x82, 0xB8, 0x0E, 0, 0, 0x50, 0xE8, 0x4E, 0xBA, 0x0A, 0x00]))
        self.assertEqual(image[0x4FBDC3 - base:0x4FBDC6 - base], bytes([0xC2, 0x08, 0x00]))
        # Construction yards fleeing: every "may the owner undeploy" call in Mission_Selling and the AI's
        # yardless-MCV Hunt rule call 0x50B730 (owner in ECX); TryToDeploy starts sub esp,18h / push ebx / push ebp
        for site in (0x449D29, 0x44A554, 0x44A802, 0x44A912, 0x44A99D, 0x736424):
            rel = (0x50B730 - (site + 5)) & 0xFFFFFFFF
            self.assertEqual(image[site - base:site + 5 - base], bytes([0xE8]) + rel.to_bytes(4, 'little'), hex(site))
        self.assertEqual(image[0x7393C0 - base:0x7393C5 - base], bytes([0x83, 0xEC, 0x18, 0x53, 0x55]))
        # A base plan node's own cell: owner in ECX, push cell*, push type, call 0x50B760 (ret 8)
        rel = (0x50B760 - (0x444FBA + 5)) & 0xFFFFFFFF
        self.assertEqual(image[0x444FB2 - base:0x444FBF - base],
                         bytes([0x8B, 0x8E, 0x1C, 0x02, 0, 0, 0x53, 0x50, 0xE8]) + rel.to_bytes(4, 'little'))
        self.assertEqual(image[0x50B77B - base:0x50B77E - base], bytes([0xC2, 0x08, 0x00]))
        # PassengerClass::GetTotalSize, on TechnoClass +0x114: push ecx / push ebx / mov ebx,[ecx] / mov ecx,[ecx+4]
        self.assertEqual(image[0x473460 - base:0x473467 - base], bytes([0x51, 0x53, 0x8B, 0x19, 0x8B, 0x49, 0x04]))
        # The base planner's type test: mov edi,[edx+ecx*4] / cmp edi,eax / jge 0x505E33 / cmp edi,-4; the
        # next entry at 0x505EAD (mov ecx,[esp+10h])
        self.assertEqual(image[0x505DA5 - base:0x505DB3 - base],
                         bytes([0x8B, 0x3C, 0x8A, 0x3B, 0xF8, 0x0F, 0x8D, 0x83, 0, 0, 0, 0x83, 0xFF, 0xFC]))
        self.assertEqual(image[0x505EAD - base:0x505EB1 - base], bytes([0x8B, 0x4C, 0x24, 0x10]))
        # BuildingClass::Sell does nothing unless +0x6E9 (build-up art present) is set
        self.assertEqual(image[0x447113 - base:0x447119 - base], bytes([0x8A, 0x86, 0xE9, 0x06, 0, 0]))


if __name__ == '__main__':
    unittest.main()
