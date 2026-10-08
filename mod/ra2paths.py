#!/usr/bin/env python3
"""Where the game, its Proton prefix and Steam are on this machine.

usage: ra2paths.py [game|prefix|steam|library|proton|runtime|screen]   (no argument: all of them)

Found through Steam: the library listed in libraryfolders.vdf that holds the game's app manifest.
Environment variables override each one:
  RA2YR_STEAM    Steam's root (default: the first of ~/.local/share/Steam, ~/.steam/steam and the
                 Flatpak Steam that has steamapps/libraryfolders.vdf)
  RA2YR_GAME     the game directory (the one with gamemd.exe)
  RA2YR_PREFIX   the Proton compatdata directory (the one with pfx/ in it)
  RA2YR_PROTON   the proton script to launch with (default: Proton - Experimental, in any library)
Nothing here raises: without Steam or the game, the paths point where a default install would be,
and whatever opens them says what is missing.

screen_size() is the game's resolution, from RA2MD.INI (install-renderer.py sets it).
"""
import os, re, sys

APP_ID = "2229850"
INSTALLDIR = "Command & Conquer Red Alert II"
STEAM_ROOTS = ["~/.local/share/Steam", "~/.steam/steam", "~/.var/app/com.valvesoftware.Steam/.local/share/Steam"]


def _vdf_values(path, key):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return []
    return [v.replace("\\\\", "\\") for v in re.findall(rf'"{key}"\s+"((?:[^"\\]|\\.)*)"', text)]


def find_steam():
    if os.environ.get("RA2YR_STEAM"):
        return os.environ["RA2YR_STEAM"]
    roots = [os.path.expanduser(r) for r in STEAM_ROOTS]
    return next((r for r in roots if os.path.exists(os.path.join(r, "steamapps", "libraryfolders.vdf"))), roots[0])


def libraries(steam):
    """Steam library roots, the main one first."""
    found = [steam] + _vdf_values(os.path.join(steam, "steamapps", "libraryfolders.vdf"), "path")
    out = []
    for lib in found:
        if lib not in out and os.path.realpath(lib) not in map(os.path.realpath, out):
            out.append(lib)
    return out


def find_library(steam):
    """The library with the game in it (the main one if none has it)."""
    libs = libraries(steam)
    manifest = os.path.join("steamapps", f"appmanifest_{APP_ID}.acf")
    return next((lib for lib in libs if os.path.exists(os.path.join(lib, manifest))), libs[0])


def find_game(library):
    names = _vdf_values(os.path.join(library, "steamapps", f"appmanifest_{APP_ID}.acf"), "installdir")
    return os.path.join(library, "steamapps", "common", names[0] if names else INSTALLDIR)


def find_tool(steam, *parts):
    """A Steam tool (Proton, the Linux runtime) from whichever library it is installed in."""
    for lib in libraries(steam):
        path = os.path.join(lib, "steamapps", "common", *parts)
        if os.path.exists(path):
            return path
    return os.path.join(steam, "steamapps", "common", *parts)


STEAM = find_steam()
LIBRARY = find_library(STEAM)
GAME = os.environ.get("RA2YR_GAME") or find_game(LIBRARY)
PREFIX = os.environ.get("RA2YR_PREFIX") or os.path.join(LIBRARY, "steamapps", "compatdata", APP_ID)
PROTON = os.environ.get("RA2YR_PROTON") or find_tool(STEAM, "Proton - Experimental", "proton")
RUNTIME = find_tool(STEAM, "SteamLinuxRuntime_4", "_v2-entry-point")
DEFAULT_SIZE = (2560, 1440)   # the tested resolution (README, Working configuration)


def screen_size(game=None):
    """(width, height) from [Video] in the game's RA2MD.INI; DEFAULT_SIZE if it has none."""
    try:
        with open(os.path.join(game or GAME, "RA2MD.INI"), encoding="latin-1") as f:
            text = f.read()
    except OSError:
        return DEFAULT_SIZE
    video = re.search(r"^\[Video\][^\[]*", text, re.M | re.I)
    size = [re.search(rf"^{key}\s*=\s*(\d+)", video.group(), re.M | re.I) if video else None
            for key in ("ScreenWidth", "ScreenHeight")]
    return (int(size[0].group(1)), int(size[1].group(1))) if all(size) else DEFAULT_SIZE


if __name__ == "__main__":
    paths = {"steam": STEAM, "library": LIBRARY, "game": GAME, "prefix": PREFIX, "proton": PROTON,
             "runtime": RUNTIME}
    if sys.argv[1:] == ["screen"]:
        print("%dx%d" % screen_size())
    elif len(sys.argv) == 2 and sys.argv[1] in paths:
        print(paths[sys.argv[1]])
    elif len(sys.argv) == 1:
        for name, path in paths.items():
            print(f"{name:8} {path}{'' if os.path.exists(path) else '   (missing)'}")
        print("screen   %dx%d" % screen_size())
    else:
        raise SystemExit(__doc__.split("\n\n")[1])
