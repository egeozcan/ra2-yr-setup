"""bench.py bookkeeping without launching the game: result CSVs, the backup of the user's launcher
files, the yspawn.ini it writes, and its DirectorFlags masks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import bench
import startbase

HERE = Path(__file__).resolve().parent
HEADER = ("frame,ms,house,country,human,director,defeated,buildings,cost_infantry,cost_vehicles,"
          "cost_aircraft,killed_units,killed_buildings,flags,plan")


def row(frame, house, country, human=0, director=0, defeated=0, buildings=10, army=1000):
    return (f"{frame},{frame * 10},{house},{country},{human},{director},{defeated},{buildings},"
            f"{army},0,0,0,0,98239,0")


class ResultTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def write(self, *lines, name="yspawn-bench.csv", where=None):
        path = (where or self.dir) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("".join(line + "\n" for line in lines))
        return path

    def test_games_that_never_sampled_are_no_result(self):
        # 0 bytes: the DLL opened the CSV but never flushed its header (hung, crashed or refused)
        for lines in ((), (HEADER,), (HEADER, "result,0,error,-1")):
            self.write(*lines)
            self.assertIsNone(bench.bench_rows(str(self.dir / "yspawn-bench.csv")))
            self.assertTrue(bench.summary(str(self.dir)).endswith("no result"))
            self.assertIsNone(bench.outcome(str(self.dir)))
            self.assertIsNone(bench.faction_result(str(self.dir)))
            self.assertEqual(bench.placements(str(self.dir)), [])

    def test_no_survivor_is_a_draw(self):
        self.write(HEADER, row(300, 0, "Americans", director=1), row(300, 1, "Russians"),
                   row(300, 2, "Allied", human=1), row(600, 0, "Americans", director=1, defeated=1),
                   row(600, 1, "Russians", defeated=1), "result,600,win,-1")
        self.assertEqual(bench.faction_result(str(self.dir)), (["Americans", "Russians"], "draw"))
        self.assertIn("win h-1 @600", bench.summary(str(self.dir)))

    def test_suite_replays_games_that_never_started(self):
        matches = [("A.mmx", 3, [(0, 0, 1, 0), (8, 1, 0, 0)], None),
                   ("B.mmx", 3, [(0, 0, 1, 0), (8, 1, 0, 0)], None)]
        self.write(where=self.dir / "00-A")
        self.write(HEADER, row(300, 0, "Americans"), where=self.dir / "01-B")
        with patch.object(bench, "run", return_value="played") as run, patch("builtins.print"):
            bench.suite_list(str(self.dir), matches)
        self.assertEqual([c.args[0] for c in run.call_args_list], [str(self.dir / "00-A")])

    def test_start_base_zero_keeps_the_mcv(self):
        ini = bench.write_ini("A.mmx", [(0, 0, 1, 0)], -1, 1000, 0, 1, {"StartBase": "0"})
        self.assertNotIn("StartBase", ini)
        self.assertNotIn("StartBase", ini["Settings"])
        with patch.object(startbase, "country_name", str), \
                patch.object(startbase, "section", return_value={"Americans": "GACNST"}) as section:
            ini = bench.write_ini("A.mmx", [(0, 0, 1, 0)], -1, 1000, 0, 1, {"StartBase": "2"})
        self.assertEqual(section.call_args.args[1], 2)
        self.assertEqual(dict(ini["StartBase"]), {"Americans": "GACNST"})
        self.assertNotIn("StartBase", ini["Settings"])


class BackupTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.game = Path(temp.name) / "game"
        self.game.mkdir()
        for target, value in ((bench.spawn, "GAME"), (bench, "BACKUP")):
            p = patch.object(target, value, str(Path(temp.name) / ("game" if value == "GAME" else "backup")))
            p.start()
            self.addCleanup(p.stop)

    def files(self):
        return {p.name: p.read_text() for p in self.game.iterdir()}

    def bench_writes(self):
        for name in bench.SAVED:
            (self.game / name).write_text("Benchmark=1\n" if name == "yspawn.ini" else "bench")

    def test_files_the_user_lacked_are_removed(self):
        bench.snapshot()
        self.bench_writes()
        bench.put_back()
        self.assertEqual(self.files(), {})
        self.bench_writes()          # a killed run: its files stay until the next run or restore
        bench.snapshot()             # must not save the benchmark's files as the user's
        bench.restore()
        self.assertEqual(self.files(), {})
        self.assertFalse(Path(bench.BACKUP).exists())

    def test_user_files_played_between_runs_are_not_overwritten(self):
        user = {"yspawn.ini": "[Settings]\nName=Me\n", "yspawn.log": "old", "yspawn.map": "map"}
        for name, text in user.items():
            (self.game / name).write_text(text)
        bench.snapshot()
        self.bench_writes()
        bench.put_back()
        self.assertEqual(self.files(), user)
        user["yspawn.log"] = "the user's last game"   # played with skirmish.py, no restore
        (self.game / "yspawn.log").write_text(user["yspawn.log"])
        bench.snapshot()
        self.bench_writes()
        bench.put_back()
        self.assertEqual(self.files(), user)
        self.bench_writes()          # killed run, then the next one
        bench.snapshot()
        bench.put_back()
        self.assertEqual(self.files(), user)


class FlagTests(unittest.TestCase):
    def test_director_default_mirrors_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "flags.c").write_text('#include <stdio.h>\n#include "director-policy.h"\n'
                                         'int main(void) { printf("%d", DIR_F_DEFAULT); return 0; }\n')
            subprocess.run(["cc", "-Wno-unused-function", "-I", str(HERE), str(tmp / "flags.c"),
                            "-o", str(tmp / "flags")], check=True)
            default = int(subprocess.run([str(tmp / "flags")], capture_output=True, text=True,
                                         check=True).stdout)
        self.assertEqual(bench.DIR_DEFAULT, default)
        self.assertEqual(bench.NOMIND, default | 32768)       # DIR_F_NO_ANTIMIND
        self.assertEqual(bench.NOPOSTURE, default | 65536)    # DIR_F_NO_POSTURE


if __name__ == "__main__":
    unittest.main()
