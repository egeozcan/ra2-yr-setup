#!/usr/bin/env python3
"""Put the tested renderer setup into the game directory from public sources (no backups/ needed).

usage: install-renderer.py [WIDTHxHEIGHT]      (default 2560x1440, the tested resolution)

  ddraw.dll   cnc-ddraw 6.3.0.0, from its GitHub release (kept in installers/, checked by SHA256)
  ddraw.ini   the release's ddraw.ini with the tested settings (README, Working configuration)
  RA2MD.INI   [Video] ScreenWidth/ScreenHeight set to the resolution
apply-working.sh runs this when backups/working/ is missing, then adds the Wine DLL override.
Close the game first: it keeps ddraw.dll open.
"""
import hashlib, io, os, re, sys, urllib.request, zipfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "mod"))
import ra2paths

URL = "https://github.com/FunkyFr3sh/cnc-ddraw/releases/download/v6.3.0.0/cnc-ddraw.zip"
ZIP = os.path.join(HERE, "installers", "cnc-ddraw-v6.3.0.0.zip")
ZIP_SHA256 = "c024d0ea42ec2d9708dc0a19342d037de7307c291dc43c01948a7d8f06b4deca"
DLL_SHA256 = "f71bf13e02503f08b1604cfe8b906656417435586188c9b5f1865f55749dd639"
# The tested ddraw.ini, as changes to the release's: renderer=gdi, and [gamemd] in exclusive
# fullscreen at the game's own resolution (windowed=false; fullscreen=false is only the stretch switch).
DDRAW_SETTINGS = {
    "ddraw": {"renderer": "gdi", "border": "false", "savesettings": "0"},
    "gamemd": {"nonexclusive": "true", "noactivateapp": "false", "maxfps": "-1", "width": "0", "height": "0",
               "fullscreen": "false", "windowed": "false", "border": "false"},
}


def release():
    if os.path.exists(ZIP):
        data = open(ZIP, "rb").read()
    else:
        print(f"downloading {URL}")
        with urllib.request.urlopen(URL) as r:
            data = r.read()
    if hashlib.sha256(data).hexdigest() != ZIP_SHA256:
        raise SystemExit(f"cnc-ddraw zip has an unexpected SHA256 ({ZIP if os.path.exists(ZIP) else URL})")
    if not os.path.exists(ZIP):
        os.makedirs(os.path.dirname(ZIP), exist_ok=True)
        open(ZIP, "wb").write(data)
    return zipfile.ZipFile(io.BytesIO(data))


def set_keys(text, settings):
    """INI text with each [section] key set (replaced in place, else added at the section's end)."""
    lines = text.split("\r\n") if text else []
    for section, keys in settings.items():
        s = next((i for i, l in enumerate(lines) if l.strip().lower() == f"[{section.lower()}]"), None)
        if s is None:
            lines += [f"[{section}]", ""]
            s = len(lines) - 2
        e = next((i for i in range(s + 1, len(lines)) if lines[i].startswith("[")), len(lines))
        while e > s + 1 and (not lines[e - 1].strip() or lines[e - 1].startswith(";")):
            e -= 1   # the blank line and the comment above the next section stay with it
        for key, value in keys.items():
            i = next((i for i in range(s + 1, e) if re.match(rf"{re.escape(key)}\s*=", lines[i], re.I)), None)
            if i is None:
                lines.insert(e, f"{key}={value}")
                e += 1
            else:
                lines[i] = f"{key}={value}"
    return "\r\n".join(lines)


def main():
    size = sys.argv[1] if len(sys.argv) > 1 else "%dx%d" % ra2paths.DEFAULT_SIZE
    if not re.fullmatch(r"\d+x\d+", size):
        raise SystemExit(__doc__.split("\n\n")[1])
    width, height = size.split("x")
    game = ra2paths.GAME
    if not os.path.exists(os.path.join(game, "gamemd.exe")):
        raise SystemExit(f"no gamemd.exe in {game} (set RA2YR_GAME, see mod/ra2paths.py)")
    z = release()
    dll = z.read("ddraw.dll")
    assert hashlib.sha256(dll).hexdigest() == DLL_SHA256
    open(os.path.join(game, "ddraw.dll"), "wb").write(dll)
    ini = z.read("ddraw.ini").decode("latin-1")
    open(os.path.join(game, "ddraw.ini"), "w", encoding="latin-1", newline="").write(set_keys(ini, DDRAW_SETTINGS))
    print(f"{game}: ddraw.dll, ddraw.ini (cnc-ddraw 6.3.0.0)")
    path = os.path.join(game, "RA2MD.INI")
    text = open(path, encoding="latin-1").read().replace("\n", "\r\n") if os.path.exists(path) else ""
    text = set_keys(text, {"Video": {"ScreenWidth": width, "ScreenHeight": height}})
    open(path, "w", encoding="latin-1", newline="").write(text)
    print(f"{path}: {width}x{height}")


if __name__ == "__main__":
    main()
