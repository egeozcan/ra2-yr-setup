"""bench.py bookkeeping without launching the game: result CSVs, the backup of the user's launcher
files, the yspawn.ini it writes, and its DirectorFlags masks."""
from pathlib import Path
import os
import subprocess
import tempfile
import time
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


class ResolutionTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.game = Path(temp.name) / "game"
        self.game.mkdir()
        for target, value in ((bench.spawn, "GAME"), (bench, "BACKUP")):
            p = patch.object(target, value, str(Path(temp.name) / ("game" if value == "GAME" else "backup")))
            p.start()
            self.addCleanup(p.stop)

    def test_only_the_resolution_changes(self):
        user = ("[Options]\r\nGameSpeed=2\r\n\r\n[Video]\r\nScreenWidth=2560\r\nStretchMovies=no\r\n"
                "ScreenHeight=1440\r\n\r\n[Audio]\r\nScreenWidth=7\r\n")
        ini = self.game / "RA2MD.INI"
        ini.write_bytes(user.encode("latin-1"))
        bench.snapshot()
        bench.set_resolution(str(self.game), 800, 600)
        self.assertEqual(ini.read_bytes().decode("latin-1"),
                         user.replace("Width=2560", "Width=800").replace("Height=1440", "Height=600"))
        bench.put_back()
        self.assertEqual(ini.read_bytes().decode("latin-1"), user)

    def prepare(self, benchmark):
        import configparser
        ini = configparser.ConfigParser(interpolation=None)
        ini.optionxform = str
        ini["Settings"] = {"Map": "A.mmx", **({"Benchmark": "1"} if benchmark else {})}
        with patch.object(bench.spawn, "running", return_value=False), \
                patch.object(bench.spawn, "map_info", return_value={"data": b"", "map": "a"}), \
                patch.object(bench.spawn.mixextract, "extract", return_value=b"map"):
            bench.spawn.prepare(ini)

    def test_a_killed_run_is_undone_before_the_users_game(self):
        user = b"[Video]\r\nScreenWidth=2560\r\nScreenHeight=1440\r\n"
        ini = self.game / "RA2MD.INI"
        ini.write_bytes(user)
        for _ in range(2):                               # as run() does; the first run is killed
            bench.snapshot()
            self.prepare(benchmark=True)                 # (a benchmark's own leaves it alone)
            bench.set_resolution(str(self.game), 800, 600)
            self.assertIn(b"ScreenWidth=800", ini.read_bytes())
        self.prepare(benchmark=False)                    # the user's own game puts theirs back
        self.assertEqual(ini.read_bytes(), user)
        self.assertFalse(Path(bench.BACKUP).exists())
        self.assertNotIn("Benchmark", (self.game / "yspawn.ini").read_text())

    def test_missing_keys_are_added_to_video(self):
        ini = self.game / "RA2MD.INI"
        ini.write_bytes(b"[Video]\r\nStretchMovies=no\r\n\r\n[Audio]\r\n")
        bench.set_resolution(str(self.game), 640, 480)
        self.assertEqual(ini.read_bytes(),
                         b"[Video]\r\nScreenWidth=640\r\nScreenHeight=480\r\nStretchMovies=no\r\n\r\n[Audio]\r\n")
        ini.write_bytes(b"[Audio]\r\nSoundVolume=0.7\r\n")
        bench.set_resolution(str(self.game), 800, 600)
        self.assertEqual(ini.read_bytes(),
                         b"[Audio]\r\nSoundVolume=0.7\r\n[Video]\r\nScreenWidth=800\r\nScreenHeight=600\r\n")


class SlotTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.game = self.root / "game"
        (self.game / "Maps").mkdir(parents=True)
        for name, text in (("gamemd-spawn.exe", "exe"), ("yspawn.dll", "dll 1"), ("ra2md.mix", "mix"),
                           ("yspawn.log", "the user's"), ("RA2MD.INI", "[Video]\r\nScreenWidth=2560\r\n"),
                           ("ddraw.ini", "[ddraw]\r\nsinglecpu=true\r\n\r\n[gamemd-spawn]\r\nminfps=-1\r\n"
                                         "singlecpu=true\r\n\r\n[other]\r\nsinglecpu=true\r\n")):
            (self.game / name).write_bytes(text.encode())
        (self.root / "compat" / "pfx").mkdir(parents=True)
        (self.root / "compat" / "pfx" / "user.reg").write_text("reg")
        for target, value, path in ((bench.spawn, "GAME", self.game), (bench.spawn, "PREFIX", self.root / "compat"),
                                    (bench, "FARM", self.root / "farm")):
            p = patch.object(target, value, str(path))
            p.start()
            self.addCleanup(p.stop)

    def test_a_slot_links_the_game_and_copies_what_it_writes(self):
        game, prefix = map(Path, bench.make_slot(2))
        self.assertTrue((game / "ra2md.mix").is_symlink() and (game / "Maps").is_symlink())
        for name in ("gamemd-spawn.exe", "yspawn.dll", "RA2MD.INI", "ddraw.ini"):
            self.assertFalse((game / name).is_symlink(), name)
        self.assertFalse((game / "yspawn.log").exists())   # the user's log stays theirs
        self.assertEqual((prefix / "pfx" / "user.reg").read_text(), "reg")
        self.assertEqual((game / "ddraw.ini").read_bytes(),
                         b"[ddraw]\r\nsinglecpu=true\r\n\r\n[gamemd-spawn]\r\nsinglecpu=false\r\nminfps=-1\r\n"
                         b"\r\n[other]\r\nsinglecpu=true\r\n")
        (self.game / "yspawn.dll").write_text("dll 2")      # installed since
        (self.game / "new.mix").write_text("mix")
        bench.make_slot(2)
        self.assertEqual((game / "yspawn.dll").read_text(), "dll 2")
        self.assertTrue((game / "new.mix").is_symlink())

    def test_slots_are_never_shared(self):
        import threading
        busy, seen, lock = set(), [], threading.Lock()

        def fake_run(out, *args, slot=None, **kw):
            with lock:
                self.assertNotIn(slot, busy)
                busy.add(slot)
                seen.append(slot)
            time.sleep(0.02)
            with lock:
                busy.discard(slot)
            return f"{out}: win h1 @100 (1s)"

        todo = [(f"o{i}", "A.mmx", [], 3, None, i + 1) for i in range(9)]
        with patch.object(bench, "run", fake_run), patch.object(bench, "JOBS", 3), \
                patch.object(bench.spawn, "sync_ddraw_ini"), patch("builtins.print"):
            lines = bench.play(todo, 1000)
        self.assertEqual(len(lines), 9)
        self.assertEqual(set(seen), {1, 2, 3})

    def test_three_that_never_start_stop_every_slot(self):
        stopped = []
        with patch.object(bench, "run", lambda out, *a, **kw: f"{out}: no result"), patch.object(bench, "JOBS", 2), \
                patch.object(bench, "stop", lambda game=None: stopped.append(game)), \
                patch.object(bench.spawn, "sync_ddraw_ini"), patch("builtins.print"):
            with self.assertRaises(SystemExit):
                bench.play([(str(self.root / f"o{i}"), "A.mmx", [], 3, None) for i in range(6)], 1000)
        self.assertEqual(sorted(stopped), [str(self.root / "farm" / f"slot{k}" / "game") for k in (1, 2)])

    def test_games_cut_short_are_played_again(self):
        def fake_run(out, *args, slot=None, **kw):
            Path(out).mkdir()
            rows = [HEADER, row(300, 0, "Americans")] + (["result,300,win,0"] if out.endswith("0") else [])
            (Path(out) / "yspawn-bench.csv").write_text("".join(r + "\n" for r in rows))
            if out.endswith("2"):
                raise KeyboardInterrupt
            return f"{out}: played"
        outs = [str(self.root / f"o{i}") for i in range(3)]
        with patch.object(bench, "run", fake_run), patch.object(bench, "JOBS", 3), \
                patch.object(bench, "stop", lambda game=None: None), \
                patch.object(bench.spawn, "sync_ddraw_ini"), patch("builtins.print"):
            with self.assertRaises(KeyboardInterrupt):
                bench.play([(o, "A.mmx", [], 3, None) for o in outs], 1000)
        self.assertEqual([bool(bench.bench_rows(os.path.join(o, "yspawn-bench.csv"))) for o in outs],
                         [True, False, False])


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
