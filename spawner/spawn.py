#!/usr/bin/env python3
"""Launch Yuri's Revenge straight into a skirmish, skipping the menus.

usage: spawn.py install        build yspawn.dll, make gamemd-spawn.exe, copy both into the game dir
       spawn.py uninstall      remove everything install and run put into the game dir
       spawn.py run [MAP]      start a skirmish with the settings in yspawn.ini (MAP overrides Map=)

gamemd-spawn.exe is a copy of the game's gamemd.exe that also imports yspawn.dll (add_import.py);
the stock gamemd.exe, and so the normal Steam launch, is left untouched. yspawn.dll replaces the
main menu with a routine that sets up the skirmish from yspawn.ini (see yspawn.c). When the match
ends the game exits instead of returning to the menu.
"""
import configparser, os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "mod"))
import csf, mixextract
from add_import import main as add_import

GAME = "/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II"
PREFIX = "/mnt/data/SteamLibrary/steamapps/compatdata/2229850"
STEAM = os.path.expanduser("~/.local/share/Steam")
GAMESCOPE = os.path.join(HERE, "..", "yuri-gamescope.py")
FILES = ["gamemd-spawn.exe", "yspawn.dll", "yspawn.ini", "yspawn.map", "yspawn.log"]


def running():
    # Wine names the process after the exe, cut to 15 characters
    return subprocess.run(["pgrep", "-x", "gamemd.exe|gamemd-spawn.ex"], capture_output=True).returncode == 0


def sync_ddraw_ini():
    """cnc-ddraw picks its per-game section by exe name. Its stock [gamemd-spawn] section (meant for CnCNet)
    lacks windowed=false, so give it the same settings as the tested [gamemd] section."""
    path = os.path.join(GAME, "ddraw.ini")
    lines = open(path, newline="", encoding="latin-1").read().split("\r\n")

    def body(name):
        s = lines.index(f"[{name}]")
        e = next((i for i in range(s + 1, len(lines)) if lines[i].startswith("[")), len(lines))
        while e > s + 1 and (not lines[e - 1].strip() or lines[e - 1].startswith(";")):
            e -= 1   # keep the blank line and the comment that belong to the next section
        return s, e

    s, e = body("gamemd")
    settings = lines[s + 1:e]
    s, e = body("gamemd-spawn")
    if lines[s + 1:e] != settings:
        lines[s + 1:e] = settings
        open(path, "w", newline="", encoding="latin-1").write("\r\n".join(lines))
        print("ddraw.ini: [gamemd-spawn] now matches [gamemd]")


def install():
    subprocess.run([os.path.join(HERE, "build.sh")], check=True)
    add_import(os.path.join(GAME, "gamemd.exe"), os.path.join(GAME, "gamemd-spawn.exe"))
    shutil.copy(os.path.join(HERE, "yspawn.dll"), GAME)
    sync_ddraw_ini()
    print("installed gamemd-spawn.exe, yspawn.dll")


def uninstall():
    for f in FILES:
        p = os.path.join(GAME, f)
        if os.path.exists(p):
            os.remove(p)
            print("removed", f)


def read_config():
    ini = configparser.ConfigParser(inline_comment_prefixes=(";",), interpolation=None)
    ini.optionxform = str
    ini.read(os.path.join(HERE, "yspawn.ini"))
    return ini


def _ini_sections(text):
    """Minimal INI reader for map packets: {section: {key: value}}, keys as written."""
    out, sec = {}, None
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        if line.startswith("[") and "]" in line:
            sec = out.setdefault(line[1:line.index("]")], {})
        elif sec is not None and "=" in line:
            k, v = line.split("=", 1)
            sec[k.strip()] = v.strip()
    return out


def map_info(archive):
    """Read the .pkt packet inside a .mmx/.yro archive. The map inside is named by the packet
    ([MultiMaps] 1=AMAZON01 in amazon.mmx), not always after the archive."""
    data = open(os.path.join(GAME, archive), "rb").read()
    base = os.path.splitext(os.path.basename(archive))[0]
    info = {"file": archive, "map": base, "label": "", "min": 2, "max": 8, "modes": ["standard"], "data": data}
    try:
        pkt = _ini_sections(mixextract.extract(data, base.lower() + ".pkt").decode("latin-1"))
        inner = next(iter(pkt.get("MultiMaps", {}).values()), base)
        sec = next((v for k, v in pkt.items() if k.lower() == inner.lower()), {})
        info.update(map=inner, label=sec.get("Description", ""),
                    min=int(sec.get("MinPlayers", 2)), max=int(sec.get("MaxPlayers", 8)),
                    modes=[m.strip().lower() for m in sec.get("GameMode", "standard").split(",")])
    except (KeyError, ValueError):
        pass
    return info


def strings():
    """The game's string table as {label.lower(): text}; the loose ra2md.csf (installed by the mod)
    wins over the one in langmd.mix, as in the game."""
    path = os.path.join(GAME, "ra2md.csf")
    if os.path.exists(path):
        entries = csf.load(path)[1]
    else:
        data = mixextract.extract(open(os.path.join(GAME, "langmd.mix"), "rb").read(), "ra2md.csf")
        with tempfile.NamedTemporaryFile(suffix=".csf") as f:
            f.write(data)
            f.flush()
            entries = csf.load(f.name)[1]
    return {name.lower(): value for name, value, _ in entries}


def list_maps():
    """Every skirmish map in the game directory, with its display name, sorted by name."""
    try:
        names = strings()
    except Exception:
        names = {}
    maps = []
    for f in sorted(os.listdir(GAME), key=str.lower):
        if os.path.splitext(f)[1].lower() in (".mmx", ".yro"):
            try:
                info = map_info(f)
            except Exception:
                continue
            del info["data"]
            info["name"] = names.get(info["label"].lower()) or os.path.splitext(f)[0]
            maps.append(info)
    return sorted(maps, key=lambda m: m["name"].lower())


def prepare(ini):
    """Extract the map named by Map= and write the game-dir yspawn.ini that yspawn.dll reads.
    ini: a ConfigParser laid out like spawner/yspawn.ini. Returns the map archive name."""
    settings = ini["Settings"]
    archive = settings.pop("Map")
    info = map_info(archive)
    open(os.path.join(GAME, "yspawn.map"), "wb").write(mixextract.extract(info["data"], info["map"].lower() + ".map"))
    settings["Scenario"] = "yspawn.map"
    # write a comment-free copy with CRLF line endings for GetPrivateProfile*
    lines = []
    for sec in ini.sections():
        lines += [f"[{sec}]"] + [f"{k}={v}" for k, v in ini[sec].items()] + [""]
    open(os.path.join(GAME, "yspawn.ini"), "w", newline="\r\n").write("\n".join(lines))
    return archive


def launch():
    """Start gamemd-spawn.exe the way Steam starts the game; returns at once."""
    sync_ddraw_ini()   # apply-working.sh restores a ddraw.ini without our [gamemd-spawn] settings
    env = dict(os.environ, STEAM_COMPAT_DATA_PATH=PREFIX, STEAM_COMPAT_CLIENT_INSTALL_PATH=STEAM,
               STEAM_COMPAT_INSTALL_PATH=GAME, STEAM_COMPAT_APP_ID="2229850",
               SteamAppId="2229850", SteamGameId="2229850")
    cmd = ["/usr/bin/python3", GAMESCOPE,
           os.path.join(STEAM, "steamapps/common/SteamLinuxRuntime_4/_v2-entry-point"),
           "--verb=waitforexitandrun", "--",
           os.path.join(STEAM, "steamapps/common/Proton - Experimental/proton"),
           "waitforexitandrun", os.path.join(GAME, "gamemd-spawn.exe")]
    subprocess.Popen(cmd, cwd=GAME, env=env, start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def installed():
    return os.path.exists(os.path.join(GAME, "gamemd-spawn.exe"))


def steam_running():
    return subprocess.run(["pgrep", "-x", "steam"], capture_output=True).returncode == 0


def run(map_override=None):
    if not installed():
        raise SystemExit("not installed; run: spawn.py install")
    ini = read_config()
    if map_override:
        ini["Settings"]["Map"] = map_override
    print(f"map {prepare(ini)}; starting")
    launch()


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd not in ("install", "uninstall", "run"):
        raise SystemExit(__doc__)
    if running():
        raise SystemExit("Yuri's Revenge is running; close it first.")
    if cmd == "install":
        install()
    elif cmd == "uninstall":
        uninstall()
    else:
        run(*sys.argv[2:3])
