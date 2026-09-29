"""Map discovery and launch preparation after moving large packs out of the game root."""
import configparser
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import spawn
import mixextract


def archive(inner, packet_name, label):
    packet = (f"[MultiMaps]\r\n1={inner}\r\n[{inner}]\r\nDescription={label}\r\n"
              "MinPlayers=2\r\nMaxPlayers=4\r\nGameMode=standard,navalwar\r\n").encode()
    map_data = b"[Basic]\r\nName=Test map\r\nMultiplayerOnly=1\r\n"
    files = [(inner + ".map", map_data), (packet_name + ".pkt", packet)]
    index, body = bytearray(), bytearray()
    for name, data in files:
        index += struct.pack("<iII", mixextract.mix_id(name), len(body), len(data))
        body += data
    return struct.pack("<IHI", 0, len(files), len(body)) + index + body, map_data


class MapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.game = Path(self.temp.name)
        self.pack = self.game / "Maps" / "2024"
        self.pack.mkdir(parents=True)
        self.patch = patch.object(spawn, "GAME", str(self.game))
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.filename = "2024 - Test map.yro"
        data, self.map_data = archive("CUSTOM01", Path(self.filename).stem, "Custom title")
        (self.pack / self.filename).write_bytes(data)

    def test_discovers_root_and_nested_archives_without_path_in_display_name(self):
        data, _ = archive("STOCK01", "stock", "NAME:STOCK")
        (self.game / "stock.mmx").write_bytes(data)
        (self.game / "broken.yro").write_bytes(b"bad")
        with patch.object(spawn, "strings", return_value={"name:stock": "Stock title"}):
            maps = spawn.list_maps()
        self.assertEqual({m["file"]: m["name"] for m in maps},
                         {"stock.mmx": "Stock title", "Maps/2024/" + self.filename: "2024 - Test map"})
        nested = next(m for m in maps if m["file"].startswith("Maps/"))
        self.assertEqual((nested["min"], nested["max"], nested["modes"]),
                         (2, 4, ["standard", "navalwar"]))

    def test_old_selection_resolves_but_existing_root_archive_takes_precedence(self):
        self.assertEqual(spawn.resolve_map(self.filename), "Maps/2024/" + self.filename)
        self.assertEqual(spawn.map_info(self.filename)["map"], "CUSTOM01")
        data, _ = archive("ROOT01", Path(self.filename).stem, "Root title")
        (self.game / self.filename).write_bytes(data)
        self.assertEqual(spawn.resolve_map(self.filename), self.filename)
        self.assertEqual(spawn.map_info(self.filename)["map"], "ROOT01")
        self.assertEqual(spawn.resolve_map("missing.yro"), "missing.yro")

    def test_prepare_extracts_nested_map_and_leaves_archives_intact(self):
        path = self.pack / self.filename
        original = path.read_bytes()
        for selection in (self.filename, "Maps/2024/" + self.filename):
            ini = configparser.ConfigParser(interpolation=None)
            ini.optionxform = str
            ini.read_dict({"Settings": {"Map": selection, "Name": "Tester"},
                           "AI1": {"Country": "8"}})
            spawn.prepare(ini)
            self.assertEqual((self.game / "yspawn.map").read_bytes(), self.map_data)
            written = (self.game / "yspawn.ini").read_bytes()
            self.assertIn(b"Scenario=yspawn.map\r\n", written)
            self.assertNotIn(b"Map=", written)
            self.assertEqual(path.read_bytes(), original)

    def test_saved_settings_and_presets_keep_relocated_selection(self):
        import skirmish
        settings = skirmish.normalize({"Map": self.filename})
        self.assertEqual(settings["Map"], "Maps/2024/" + self.filename)


if __name__ == "__main__":
    unittest.main()
