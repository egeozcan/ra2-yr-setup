#!/usr/bin/python3
"""Skirmish Setup: a window for choosing the map, players and options, then starting the quick
skirmish launcher (spawn.py) with them.

usage: skirmish.py                    open the window
       skirmish.py --dry-run          same, but Start only writes yspawn.ini/.map into the game dir
       skirmish.py --install-desktop  add "Skirmish Setup" to the desktop's application menu

Needs the system Python (/usr/bin/python3), which has GTK 4 and libadwaita; the linuxbrew python3
on PATH has no gi module. The last used settings are kept in ~/.config/ra2-yr-setup/skirmish.json;
spawner/yspawn.ini (used by spawn.py run) is not changed.
"""
import configparser, json, os, random, subprocess, sys, threading
import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, GLib, Graphene, Gtk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spawn                        # first: it puts ../mod on sys.path
import mappreview, mixextract

APP_ID = "local.ra2yr.SkirmishSetup"
SAVED = os.path.expanduser("~/.config/ra2-yr-setup/skirmish.json")
DESKTOP = os.path.expanduser("~/.local/share/applications/ra2yr-skirmish-setup.desktop")

RANDOM = -1
COUNTRIES = ["America", "Korea", "France", "Germany", "Britain", "Libya", "Iraq", "Cuba", "Russia", "Yuri"]
COLORS = [("Gold", "#e8c43a"), ("Red", "#d02a2a"), ("Blue", "#3160d8"), ("Green", "#35a53a"),
          ("Orange", "#ee8a24"), ("Sky blue", "#43c4e8"), ("Purple", "#8a45c9"), ("Pink", "#f070b4")]
DIFFICULTIES = ["Hard", "Medium", "Easy"]          # yspawn.ini: 0 hard, 1 medium, 2 easy
SPEEDS = ["Fastest", "Faster", "Fast", "Normal", "Slow", "Slower", "Slowest"]   # GameSpeed 0..6
SWITCHES = [("Bases", "Start with a base (MCV)"), ("ShortGame", "Short game"),
            ("Superweapons", "Superweapons"), ("Crates", "Crates"), ("MCVRedeploy", "MCV repacks"),
            ("BuildOffAlly", "Build off ally's base"), ("BridgeDestroy", "Destroyable bridges")]
NUMBERS = [("Credits", "Starting credits", 1000, 100000, 1000),
           ("UnitCount", "Starting units", 0, 10, 1), ("TechLevel", "Tech level", 1, 10, 1)]


# ---- settings: plain dict, saved as JSON ----
def default_settings():
    """Defaults come from spawner/yspawn.ini, the file spawn.py run uses."""
    ini = spawn.read_config()
    s = ini["Settings"]
    out = {"Map": s.get("Map", "Tsunami.mmx"), "Name": s.get("Name", "Commander"), "Country": s.getint("Country", 0),
           "Color": s.getint("Color", 0), "GameSpeed": s.getint("GameSpeed", 0)}
    for key, _, lo, _, _ in NUMBERS:
        out[key] = s.getint(key, lo)
    for key, _ in SWITCHES:
        out[key] = bool(s.getint(key, 1))
    out["AI"] = [{"Country": ini[sec].getint("Country", 8), "Color": ini[sec].getint("Color", 1),
                  "Difficulty": ini[sec].getint("Difficulty", 2)}
                 for sec in ini.sections() if sec.startswith("AI")]
    return out


def load_settings():
    s = default_settings()
    try:
        s.update(json.load(open(SAVED)))
    except (OSError, ValueError):
        pass
    return s


def save_settings(s):
    os.makedirs(os.path.dirname(SAVED), exist_ok=True)
    json.dump(s, open(SAVED, "w"), indent=1)


def problems(s, map_info):
    """Reasons the settings can't be started, for the status line; empty when all is well."""
    out = []
    players = 1 + len(s["AI"])
    if not map_info:
        out.append("Choose a map.")
    elif players > map_info["max"]:
        out.append(f"{short_name(map_info)} has room for {map_info['max']} players; you have {players}.")
    if not s["AI"]:
        out.append("Add at least one opponent.")
    fixed = [c for c in [s["Color"]] + [a["Color"] for a in s["AI"]] if c != RANDOM]
    if len(fixed) != len(set(fixed)):
        out.append("Two players have the same colour.")
    return out


def build_config(s):
    """The ConfigParser spawn.prepare() expects, with every Random resolved."""
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    colors = [s["Color"]] + [a["Color"] for a in s["AI"]]
    free = [c for c in range(len(COLORS)) if c not in colors]
    random.shuffle(free)
    colors = [c if c != RANDOM else free.pop() for c in colors]
    pick = lambda c: random.randrange(len(COUNTRIES)) if c == RANDOM else c
    name = "".join(ch for ch in s["Name"] if 32 <= ord(ch) < 127).strip()[:19] or "Commander"
    # GameMode 1 = Battle: mpmodesmd.ini says it is the only mode that allows AI players
    ini["Settings"] = {"Map": s["Map"], "Name": name, "Country": pick(s["Country"]), "Color": colors[0],
                       "GameMode": 1, "GameSpeed": s["GameSpeed"]}
    for key, *_ in NUMBERS:
        ini["Settings"][key] = str(s[key])
    for key, _ in SWITCHES:
        ini["Settings"][key] = "1" if s[key] else "0"
    for i, ai in enumerate(s["AI"], 1):
        ini[f"AI{i}"] = {"Country": pick(ai["Country"]), "Color": colors[i], "Difficulty": ai["Difficulty"]}
    return ini


# ---- widgets ----
CSS = "".join(f".swatch-{i} {{ background: {rgb}; border-radius: 3px; min-width: 14px; min-height: 14px; }}\n"
              for i, (_, rgb) in enumerate(COLORS)) + """
.swatch-random { border: 1px dashed alpha(currentColor, .5); border-radius: 3px; min-width: 12px; min-height: 12px; }
.map-preview { border-radius: 8px; background: black; }
"""


def color_factory():
    """List item factory for colour choices: a swatch and the name."""
    f = Gtk.SignalListItemFactory()

    def setup(_, item):
        box = Gtk.Box(spacing=8)
        swatch = Gtk.Box(valign=Gtk.Align.CENTER)
        box.append(swatch)
        box.append(Gtk.Label(xalign=0))
        item.set_child(box)

    def bind(_, item):
        swatch, label = item.get_child().get_first_child(), item.get_child().get_last_child()
        text = item.get_item().get_string()
        label.set_text(text)
        idx = next((i for i, (name, _) in enumerate(COLORS) if name == text), RANDOM)
        swatch.set_css_classes([f"swatch-{idx}" if idx != RANDOM else "swatch-random"])

    f.connect("setup", setup)
    f.connect("bind", bind)
    return f


def short_name(m):
    """Map name without the "(2-4)" player count some of them carry."""
    return m["name"].rsplit(" (", 1)[0] if m["name"].endswith(")") else m["name"]


def choice_model(names, with_random):
    return Gtk.StringList.new((["Random"] if with_random else []) + list(names))


class OpponentRow(Adw.ActionRow):
    def __init__(self, win, ai):
        super().__init__()
        self.win, self.ai = win, ai
        self.country = Gtk.DropDown(model=choice_model(COUNTRIES, True), valign=Gtk.Align.CENTER)
        self.country.set_selected(ai["Country"] + 1)
        self.color = Gtk.DropDown(model=choice_model([c for c, _ in COLORS], True), valign=Gtk.Align.CENTER)
        self.color.set_factory(color_factory())
        self.color.set_selected(ai["Color"] + 1)
        self.difficulty = Gtk.DropDown(model=choice_model(DIFFICULTIES, False), valign=Gtk.Align.CENTER)
        self.difficulty.set_selected(ai["Difficulty"])
        remove = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER,
                            tooltip_text="Remove opponent", css_classes=["flat"])
        for w in (self.country, self.color, self.difficulty, remove):
            self.add_suffix(w)
        self.country.connect("notify::selected", self.changed)
        self.color.connect("notify::selected", self.changed)
        self.difficulty.connect("notify::selected", self.changed)
        remove.connect("clicked", lambda _: win.remove_ai(self))

    def changed(self, *_):
        self.ai.update(Country=self.country.get_selected() - 1, Color=self.color.get_selected() - 1,
                       Difficulty=self.difficulty.get_selected())
        self.win.refresh()


class Window(Adw.ApplicationWindow):
    def __init__(self, app, dry_run):
        super().__init__(application=app, title="Skirmish Setup", default_width=1150, default_height=780)
        self.s, self.dry_run = load_settings(), dry_run
        self.maps, self.map_rows, self.ai_rows = {}, [], []
        self.game_running, self.launched_at, self.installing = False, 0, False

        # header
        self.start = Gtk.Button(label="Start Skirmish", css_classes=["suggested-action", "pill"])
        self.start.connect("clicked", self.on_start)
        self.title = Adw.WindowTitle(title="Skirmish Setup", subtitle="Yuri's Revenge")
        header = Adw.HeaderBar(title_widget=self.title)
        header.pack_end(self.start)

        # banners: launcher not installed / Steam not running
        self.install_banner = Adw.Banner(title="The quick launcher is not installed in the game directory.",
                                         button_label="Install")
        self.install_banner.connect("button-clicked", self.on_install)
        self.steam_banner = Adw.Banner(title="Steam is not running. The game needs it; start Steam first.")

        # sidebar: map list
        self.search = Gtk.SearchEntry(placeholder_text="Search maps", margin_start=12, margin_end=12,
                                      margin_top=6, margin_bottom=6)
        self.search.connect("search-changed", lambda *_: self.map_list.invalidate_filter())
        self.map_list = Gtk.ListBox(css_classes=["navigation-sidebar"])
        self.map_list.set_filter_func(self.map_visible)
        self.map_list.connect("row-selected", self.on_map_selected)
        self.map_list.set_placeholder(Gtk.Label(label="Loading maps…", margin_top=24, css_classes=["dim-label"]))
        self.side_scroll = side_scroll = Gtk.ScrolledWindow(vexpand=True, child=self.map_list)
        random_map = Gtk.Button(label="Random map", margin_start=12, margin_end=12, margin_bottom=12)
        random_map.connect("clicked", self.on_random_map)
        sidebar = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        sidebar.append(self.search)
        sidebar.append(side_scroll)
        sidebar.append(random_map)

        # content: settings
        page = Adw.PreferencesPage()
        page.add(self.build_map_group())
        page.add(self.build_player_group())
        page.add(self.build_ai_group())
        page.add(self.build_game_group())
        page.add(self.build_rules_group())

        split = Adw.OverlaySplitView(sidebar=sidebar, content=page, min_sidebar_width=260,
                                     max_sidebar_width=340, sidebar_width_fraction=0.28)
        self.toasts = Adw.ToastOverlay(child=split)
        view = Adw.ToolbarView(content=self.toasts)
        view.add_top_bar(header)
        view.add_top_bar(self.install_banner)
        view.add_top_bar(self.steam_banner)
        self.set_content(view)

        self.connect("close-request", self.on_close)
        self.check_environment()
        GLib.timeout_add_seconds(2, self.check_environment)
        threading.Thread(target=self.load_maps, daemon=True).start()
        self.refresh()

    # ---- groups ----
    def build_map_group(self):
        g = Adw.PreferencesGroup()
        self.preview = Gtk.Picture(content_fit=Gtk.ContentFit.CONTAIN, height_request=240,
                                   css_classes=["map-preview"], overflow=Gtk.Overflow.HIDDEN)
        self.map_title = Gtk.Label(css_classes=["title-2"], xalign=0, margin_top=12)
        self.map_sub = Gtk.Label(css_classes=["dim-label"], xalign=0)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        for w in (self.preview, self.map_title, self.map_sub):
            box.append(w)
        g.add(box)
        return g

    def build_player_group(self):
        g = Adw.PreferencesGroup(title="You")
        name = Adw.EntryRow(title="Name", text=self.s["Name"], max_length=19)
        name.connect("changed", lambda e: self.set("Name", e.get_text()))
        country = Adw.ComboRow(title="Country", model=choice_model(COUNTRIES, True))
        country.set_selected(self.s["Country"] + 1)
        country.connect("notify::selected", lambda r, _: self.set("Country", r.get_selected() - 1))
        color = Adw.ComboRow(title="Colour", model=choice_model([c for c, _ in COLORS], True))
        color.set_factory(color_factory())
        color.set_selected(self.s["Color"] + 1)
        color.connect("notify::selected", lambda r, _: self.set("Color", r.get_selected() - 1))
        for r in (name, country, color):
            g.add(r)
        return g

    def build_ai_group(self):
        self.ai_group = Adw.PreferencesGroup(title="Opponents", description="Country, colour and difficulty")
        self.add_ai_button = Gtk.Button(icon_name="list-add-symbolic", tooltip_text="Add opponent",
                                        css_classes=["flat"], valign=Gtk.Align.CENTER)
        self.add_ai_button.connect("clicked", self.on_add_ai)
        self.ai_group.set_header_suffix(self.add_ai_button)
        for ai in self.s["AI"]:
            self.add_ai_row(ai)
        return self.ai_group

    def build_game_group(self):
        g = Adw.PreferencesGroup(title="Game")
        speed = Adw.ComboRow(title="Game speed", model=Gtk.StringList.new(SPEEDS))
        speed.set_selected(self.s["GameSpeed"])
        speed.connect("notify::selected", lambda r, _: self.set("GameSpeed", r.get_selected()))
        g.add(speed)
        for key, title, lo, hi, step in NUMBERS:
            row = Adw.SpinRow.new_with_range(lo, hi, step)
            row.set_title(title)
            row.set_value(self.s[key])
            row.connect("notify::value", lambda r, _, k=key: self.set(k, int(r.get_value())))
            g.add(row)
        return g

    def build_rules_group(self):
        g = Adw.PreferencesGroup(title="Rules")
        for key, title in SWITCHES:
            row = Adw.SwitchRow(title=title, active=self.s[key])
            row.connect("notify::active", lambda r, _, k=key: self.set(k, r.get_active()))
            g.add(row)
        return g

    # ---- state ----
    def set(self, key, value):
        self.s[key] = value
        self.refresh()

    def current_map(self):
        return self.maps.get(self.s["Map"])

    def refresh(self):
        info = self.current_map()
        if info:
            self.add_ai_button.set_sensitive(1 + len(self.s["AI"]) < info["max"])
            self.title.set_subtitle(f"{short_name(info)} · {1 + len(self.s['AI'])} players")
        issues = problems(self.s, info) if self.maps else ["Loading maps…"]
        if not self.dry_run:
            if self.game_running:
                issues.insert(0, "Yuri's Revenge is running.")
            if self.install_banner.get_revealed() or self.installing:
                issues.insert(0, "The quick launcher is not installed.")
            if self.steam_banner.get_revealed():
                issues.insert(0, "Steam is not running.")
        self.start.set_sensitive(not issues)
        self.start.set_tooltip_text(issues[0] if issues else None)
        self.ai_group.set_description(issues[0] if issues and self.maps else "Country, colour and difficulty")

    def check_environment(self):
        # the game takes a few seconds to appear after a start; count it as running meanwhile
        self.game_running = spawn.running() or GLib.get_monotonic_time() - self.launched_at < 20_000_000
        self.install_banner.set_revealed(not spawn.installed() and not self.installing)
        self.steam_banner.set_revealed(not spawn.steam_running())
        self.refresh()
        return True

    # ---- maps ----
    def load_maps(self):
        try:
            maps = spawn.list_maps()
        except Exception as e:
            GLib.idle_add(self.toast, f"Could not read maps: {e}")
            return
        GLib.idle_add(self.fill_maps, maps)

    def fill_maps(self, maps):
        self.maps = {m["file"]: m for m in maps}
        for m in maps:
            name = short_name(m)
            players = f"{m['min']}–{m['max']} players" if m["min"] != m["max"] else f"{m['max']} players"
            row = Adw.ActionRow(title=GLib.markup_escape_text(name), subtitle=players)
            row.map = m
            self.map_rows.append(row)
            self.map_list.append(row)
        self.map_list.set_placeholder(Gtk.Label(label="No maps match", margin_top=24, css_classes=["dim-label"]))
        if self.s["Map"] not in self.maps:
            self.s["Map"] = maps[0]["file"]
        self.select_map(self.s["Map"])
        self.map_list.get_selected_row().grab_focus()

    def map_visible(self, row):
        m, text = row.map, self.search.get_text().lower()
        return "standard" in m["modes"] and (not text or text in m["name"].lower() or text in m["file"].lower())

    def select_map(self, file):
        for row in self.map_rows:
            if row.map["file"] == file:
                self.map_list.select_row(row)
                GLib.idle_add(self.scroll_to, row)
                return

    def scroll_to(self, row):
        ok, point = row.compute_point(self.map_list, Graphene.Point())
        if ok:
            adj = self.side_scroll.get_vadjustment()
            adj.set_value(point.y - (adj.get_page_size() - row.get_height()) / 2)
        return False

    def on_map_selected(self, _, row):
        if row is None:
            return
        m = row.map
        self.s["Map"] = m["file"]
        self.map_title.set_text(short_name(m))
        players = f"{m['min']}–{m['max']} players" if m["min"] != m["max"] else f"{m['max']} players"
        self.map_sub.set_text(f"{m['file']} · {players}")
        self.preview.set_paintable(self.map_texture(m))
        self.refresh()

    def map_texture(self, m):
        try:
            info = spawn.map_info(m["file"])
            text = mixextract.extract(info["data"], info["map"].lower() + ".map").decode("latin-1")
            w, h, rgb = mappreview.preview(text)
        except Exception:
            return None
        # double the pixels so the scaled-up picture stays crisp
        rows = [rgb[y * w * 3:(y + 1) * w * 3] for y in range(h)]
        rows = [b"".join(r[x:x + 3] * 2 for x in range(0, len(r), 3)) for r in rows]
        data = b"".join(r + r for r in rows)
        return Gdk.MemoryTexture.new(w * 2, h * 2, Gdk.MemoryFormat.R8G8B8, GLib.Bytes.new(data), w * 6)

    def on_random_map(self, _):
        rows = [r for r in self.map_rows if self.map_visible(r) and r.map["max"] >= 1 + len(self.s["AI"])]
        if rows:
            self.select_map(random.choice(rows).map["file"])

    # ---- opponents ----
    def add_ai_row(self, ai):
        row = OpponentRow(self, ai)
        self.ai_rows.append(row)
        self.ai_group.add(row)
        self.renumber()

    def renumber(self):
        for i, row in enumerate(self.ai_rows, 1):
            row.set_title(f"Opponent {i}")

    def on_add_ai(self, _):
        info = self.current_map()
        if info and 1 + len(self.s["AI"]) >= info["max"]:
            return
        used = {self.s["Color"]} | {a["Color"] for a in self.s["AI"]}
        color = next((c for c in range(len(COLORS)) if c not in used), RANDOM)
        ai = {"Country": RANDOM, "Color": color, "Difficulty": 2}
        self.s["AI"].append(ai)
        self.add_ai_row(ai)
        self.refresh()

    def remove_ai(self, row):
        self.s["AI"].remove(row.ai)
        self.ai_rows.remove(row)
        self.ai_group.remove(row)
        self.renumber()
        self.refresh()

    # ---- actions ----
    def toast(self, text):
        self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(text), timeout=4))

    def on_start(self, _):
        self.check_environment()
        if not self.start.get_sensitive():
            return
        save_settings(self.s)
        ini = build_config(self.s)
        try:
            spawn.prepare(ini)
            if not self.dry_run:
                spawn.launch()
        except Exception as e:
            self.toast(f"Could not start: {e}")
            return
        st = ini["Settings"]
        who = ", ".join(f"{COUNTRIES[int(ini[s]['Country'])]} ({DIFFICULTIES[int(ini[s]['Difficulty'])].lower()})"
                        for s in ini.sections() if s.startswith("AI"))
        verb = "Wrote yspawn.ini (dry run)" if self.dry_run else "Starting"
        self.toast(f"{verb}: {COUNTRIES[int(st['Country'])]} vs {who}")
        if not self.dry_run:
            self.launched_at = GLib.get_monotonic_time()
            self.check_environment()

    def on_close(self, *_):
        save_settings(self.s)
        return False

    def on_install(self, _):
        self.installing = True
        self.install_banner.set_revealed(False)
        self.toast("Installing: building yspawn.dll (the first build downloads a compiler image)…")

        def work():
            r = subprocess.run([sys.executable, os.path.join(HERE, "spawn.py"), "install"],
                               capture_output=True, text=True)
            msg = "Launcher installed" if r.returncode == 0 else \
                f"Install failed: {(r.stderr or r.stdout).strip().splitlines()[-1:] or r.returncode}"
            GLib.idle_add(self.install_done, msg)

        threading.Thread(target=work, daemon=True).start()

    def install_done(self, msg):
        self.installing = False
        self.toast(msg)
        self.check_environment()


def install_desktop():
    os.makedirs(os.path.dirname(DESKTOP), exist_ok=True)
    open(DESKTOP, "w").write(f"""[Desktop Entry]
Type=Application
Name=Skirmish Setup
GenericName=Yuri's Revenge quick skirmish
Comment=Choose a map, opponents and options, then start Yuri's Revenge straight into a skirmish
Exec=/usr/bin/python3 "{os.path.abspath(__file__)}"
Icon=steam_icon_2229850
Terminal=false
Categories=Game;
StartupWMClass={APP_ID}
""")
    print("wrote", DESKTOP)


def main():
    if "--install-desktop" in sys.argv:
        return install_desktop()
    app = Adw.Application(application_id=APP_ID)
    dry_run = "--dry-run" in sys.argv

    def activate(app):
        css = Gtk.CssProvider()
        css.load_from_string(CSS)
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        (app.get_active_window() or Window(app, dry_run)).present()

    app.connect("activate", activate)
    app.run([sys.argv[0]])


if __name__ == "__main__":
    main()
