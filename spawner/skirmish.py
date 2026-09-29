#!/usr/bin/python3
"""Skirmish Setup: a window for choosing the map, players and options, then starting the quick
skirmish launcher (spawn.py) with them.

usage: skirmish.py                    open the window
       skirmish.py --dry-run          same, but Start only writes yspawn.ini/.map into the game dir
       skirmish.py --install-desktop  add "Skirmish Setup" to the desktop's application menu

Needs the system Python (/usr/bin/python3), which has GTK 4 and libadwaita; the linuxbrew python3
on PATH has no gi module. The last used settings are kept in ~/.config/ra2-yr-setup/skirmish.json
and named presets in skirmish-presets.json next to it; spawner/yspawn.ini (used by spawn.py run) is not changed.
Starting base (like Age of Empires II's Empire Wars): everyone starts with the buildings of the chosen tier already up
instead of an MCV; startbase.py works out which.
"""
import configparser, json, os, random, subprocess, sys, threading
import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, GLib, Graphene, Gtk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spawn                        # first: it puts ../mod on sys.path
import mappreview, mixextract, startbase

APP_ID = "local.ra2yr.SkirmishSetup"
SAVED = os.path.expanduser("~/.config/ra2-yr-setup/skirmish.json")
PRESETS = os.path.expanduser("~/.config/ra2-yr-setup/skirmish-presets.json")
DESKTOP = os.path.expanduser("~/.local/share/applications/ra2yr-skirmish-setup.desktop")

RANDOM = -1
COUNTRIES = ["America", "Korea", "France", "Germany", "Britain", "Libya", "Iraq", "Cuba", "Russia", "Yuri"]
COLORS = [("Gold", "#e8c43a"), ("Red", "#d02a2a"), ("Blue", "#3160d8"), ("Green", "#35a53a"),
          ("Orange", "#ee8a24"), ("Sky blue", "#43c4e8"), ("Purple", "#8a45c9"), ("Pink", "#f070b4")]
DIFFICULTIES = ["Hard", "Medium", "Easy"]          # yspawn.ini: 0 hard, 1 medium, 2 easy
TEAMS = ["None", "A", "B", "C", "D"]               # yspawn.ini Team: -1 none, 0..3 = A..D
SPEEDS = ["Fastest", "Faster", "Fast", "Normal", "Slow", "Slower", "Slowest"]   # GameSpeed 0..6
SWITCHES = [("Bases", "Start with a base (MCV)"), ("ShortGame", "Short game"),
            ("Superweapons", "Superweapons"), ("HumanInPeace", "Human in peace"),
            ("Crates", "Crates"), ("MCVRedeploy", "MCV repacks"),
            ("BuildOffAlly", "Build off ally's base"), ("BridgeDestroy", "Destroyable bridges")]
NUMBERS = [("Credits", "Starting credits", 1000, 100000, 1000),
           ("UnitCount", "Starting units", 0, 10, 1), ("TechLevel", "Tech level", 1, 10, 1)]


# ---- settings: plain dict, saved as JSON ----
def default_settings():
    """Defaults come from spawner/yspawn.ini, the file spawn.py run uses."""
    ini = spawn.read_config()
    s = ini["Settings"]
    out = {"Map": s.get("Map", "Tsunami.mmx"), "Name": s.get("Name", "Commander"), "Country": s.getint("Country", 0),
           "Color": s.getint("Color", 0), "Start": s.getint("Start", RANDOM), "Team": s.getint("Team", RANDOM),
           "GameSpeed": s.getint("GameSpeed", 0), "StartBase": 0}
    for key, _, lo, _, _ in NUMBERS:
        out[key] = s.getint(key, lo)
    for key, _ in SWITCHES:
        out[key] = bool(s.getint(key, 0 if key == "HumanInPeace" else 1))
    out["AI"] = [{"Country": ini[sec].getint("Country", 8), "Color": ini[sec].getint("Color", 1),
                  "Difficulty": ini[sec].getint("Difficulty", 2), "Start": ini[sec].getint("Start", RANDOM),
                  "Team": ini[sec].getint("Team", RANDOM)}
                 for sec in ini.sections() if sec.startswith("AI")]
    return out


def normalize(saved):
    """Settings as the window expects them: defaults filled in, and a copy so nothing is shared with `saved`."""
    s = default_settings()
    s.update(json.loads(json.dumps(saved)))
    s["Map"] = spawn.resolve_map(s["Map"])
    for p in [s] + s["AI"]:   # settings saved before start positions and teams existed
        p.setdefault("Start", RANDOM)
        p.setdefault("Team", RANDOM)
    return s


def read_json(path):
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return {}


def write_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(data, open(path, "w"), indent=1)


def load_settings():
    return normalize(read_json(SAVED))


def save_settings(s):
    write_json(SAVED, s)


def load_presets():
    """{name: settings} of the presets saved under a name, sorted by name."""
    return dict(sorted(read_json(PRESETS).items(), key=lambda kv: kv[0].lower()))


def save_presets(presets):
    write_json(PRESETS, presets)


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
    everyone = [s] + s["AI"]
    for key, what in (("Color", "colour"), ("Start", "start position")):
        fixed = [p[key] for p in everyone if p[key] != RANDOM]
        if len(fixed) != len(set(fixed)):
            out.append(f"Two players have the same {what}.")
    if map_info and any(p["Start"] >= map_info["max"] for p in everyone):
        out.append(f"{short_name(map_info)} has only {map_info['max']} start positions.")
    teams = {p["Team"] for p in everyone}
    if len(teams) == 1 and RANDOM not in teams:
        out.append("Everyone is on the same team, so there is nobody to fight.")
    if s["StartBase"]:
        tier = startbase.TIERS[s["StartBase"]]
        try:
            countries = {startbase.country_name(c) for p in everyone
                         for c in (range(len(COUNTRIES)) if p["Country"] == RANDOM else [p["Country"]])}
            need = startbase.tech_level(countries, s["StartBase"])
        except Exception as e:
            out.append(f"Could not work out the {tier.lower()} bases from the game's rules: {e}")
        else:
            if s["TechLevel"] < need:
                out.append(f"A {tier.lower()} starting base needs tech level {need} or higher.")
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
                       "Start": s["Start"], "Team": s["Team"], "GameMode": 1, "GameSpeed": s["GameSpeed"]}
    for key, *_ in NUMBERS:
        ini["Settings"][key] = str(s[key])
    for key, _ in SWITCHES:
        ini["Settings"][key] = "1" if s[key] else "0"
    for i, ai in enumerate(s["AI"], 1):
        ini[f"AI{i}"] = {"Country": pick(ai["Country"]), "Color": colors[i], "Difficulty": ai["Difficulty"],
                         "Start": ai["Start"], "Team": ai["Team"]}
    if s["StartBase"]:
        ini["Settings"]["Bases"] = "1"   # the bases replace the MCVs
        countries = dict.fromkeys(startbase.country_name(ini[sec]["Country"]) for sec in ini.sections())
        ini["StartBase"] = startbase.section(countries, s["StartBase"])
    return ini


# ---- widgets ----
CSS = "".join(f".swatch-{i} {{ background: {rgb}; border-radius: 3px; min-width: 14px; min-height: 14px; }}\n"
              for i, (_, rgb) in enumerate(COLORS)) + """
.swatch-random { border: 1px dashed alpha(currentColor, .5); border-radius: 3px; min-width: 12px; min-height: 12px; }
.map-preview { border-radius: 8px; background: black; }
.players { padding: 12px; }
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


def dropdown(model, selected, on_change, factory=None):
    d = Gtk.DropDown(model=model, valign=Gtk.Align.CENTER)
    if factory:
        d.set_factory(factory)
    d.set_selected(selected)
    d.connect("notify::selected", lambda w, _: on_change(w.get_selected()))
    return d


def pills(options, selected, on_change):
    """A pill-shaped segmented control, one toggle per (label, value) left to right."""
    g = Adw.ToggleGroup(css_classes=["round"], valign=Gtk.Align.CENTER, can_shrink=False)
    for label, value in options:
        g.add(Adw.Toggle(label=label, name=str(value)))
    g.set_active_name(str(selected))
    g.connect("notify::active-name", lambda w, _: w.get_active_name() and on_change(int(w.get_active_name())))
    return g


class Window(Adw.ApplicationWindow):
    def __init__(self, app, dry_run):
        super().__init__(application=app, title="Skirmish Setup", default_width=1150, default_height=780)
        self.s, self.dry_run = load_settings(), dry_run
        self.maps, self.map_rows, self.start_points, self.preview_size = {}, [], {}, None
        self.game_running, self.launched_at, self.installing = False, 0, False

        # header
        self.start = Gtk.Button(label="Start Skirmish", css_classes=["suggested-action", "pill"])
        self.start.connect("clicked", self.on_start)
        self.title = Adw.WindowTitle(title="Skirmish Setup", subtitle="Yuri's Revenge")
        header = Adw.HeaderBar(title_widget=self.title)
        header.pack_start(self.build_presets_menu())
        header.pack_end(self.start)

        # banners: launcher not installed / Steam not running
        self.install_banner = Adw.Banner(title="The quick launcher is not installed in the game directory.",
                                         button_label="Install")
        self.install_banner.connect("button-clicked", self.on_install)
        self.steam_banner = Adw.Banner(title="Steam is not running. The game needs it; start Steam first.")

        # sidebar: map list
        self.search = Gtk.SearchEntry(placeholder_text="Search by name", margin_start=12, margin_end=12,
                                      margin_top=6, margin_bottom=6)
        self.search.connect("search-changed", lambda *_: self.map_list.invalidate_filter())
        self.player_count = dropdown(choice_model(["Any"] + [str(n) for n in range(2, 9)], False), 0,
                                     lambda _: self.map_list.invalidate_filter())
        self.player_count.set_tooltip_text("Filter by maximum player count")
        player_filter = Gtk.Box(spacing=12, margin_start=12, margin_end=12, margin_bottom=6)
        player_filter.append(Gtk.Label(label="Ma_x players", use_underline=True, xalign=0, hexpand=True,
                                       mnemonic_widget=self.player_count))
        player_filter.append(self.player_count)
        self.map_list = Gtk.ListBox(css_classes=["navigation-sidebar"])
        self.map_list.set_filter_func(self.map_visible)
        self.map_list.connect("row-selected", self.on_map_selected)
        self.map_list.set_placeholder(Gtk.Label(label="Loading maps…", margin_top=24, css_classes=["dim-label"]))
        self.side_scroll = side_scroll = Gtk.ScrolledWindow(vexpand=True, child=self.map_list)
        random_map = Gtk.Button(label="Random map", margin_start=12, margin_end=12, margin_bottom=12)
        random_map.connect("clicked", self.on_random_map)
        sidebar = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        sidebar.append(self.search)
        sidebar.append(player_filter)
        sidebar.append(side_scroll)
        sidebar.append(random_map)

        # content: settings
        page = self.page = Adw.PreferencesPage()
        page.add(self.build_map_group())
        self.build_settings_groups()

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
    def build_settings_groups(self):
        """The groups below the map, built from self.s; called again to show loaded settings."""
        for g in getattr(self, "settings_groups", []):
            self.page.remove(g)
        self.settings_groups = [self.build_players_group(), self.build_game_group(), self.build_rules_group()]
        for g in self.settings_groups:
            self.page.add(g)

    def build_map_group(self):
        g = Adw.PreferencesGroup()
        self.preview = Gtk.Picture(content_fit=Gtk.ContentFit.CONTAIN, height_request=260,
                                   css_classes=["map-preview"], overflow=Gtk.Overflow.HIDDEN)
        self.marks = Gtk.DrawingArea(can_target=False)
        self.marks.set_draw_func(self.draw_marks)
        overlay = Gtk.Overlay(child=self.preview)
        overlay.add_overlay(self.marks)
        self.map_title = Gtk.Label(css_classes=["title-2"], xalign=0, margin_top=12)
        self.map_sub = Gtk.Label(css_classes=["dim-label"], xalign=0)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        for w in (overlay, self.map_title, self.map_sub):
            box.append(w)
        g.add(box)
        return g

    def build_players_group(self):
        g = self.players_group = Adw.PreferencesGroup(title="Players")
        self.add_ai_button = Gtk.Button(icon_name="list-add-symbolic", tooltip_text="Add opponent",
                                        css_classes=["flat"], valign=Gtk.Align.CENTER)
        self.add_ai_button.connect("clicked", self.on_add_ai)
        g.set_header_suffix(self.add_ai_button)
        name = Adw.EntryRow(title="Your name", text=self.s["Name"], max_length=19)
        name.connect("changed", lambda e: self.set("Name", e.get_text()))
        g.add(name)
        self.player_grid = Gtk.Grid(column_spacing=8, row_spacing=8)
        card = Gtk.Box(css_classes=["card", "players"], margin_top=12)
        card.append(self.player_grid)
        g.add(card)
        self.rebuild_players()
        return g

    def rebuild_players(self):
        """One grid row per player: you first, then the AI opponents."""
        grid = self.player_grid
        while (child := grid.get_first_child()) is not None:
            grid.remove(child)
        info = self.current_map()
        starts = info["max"] if info else 8
        for col, text in enumerate(["", "Country", "Colour", "Team", "Start", "Difficulty"]):
            grid.attach(Gtk.Label(label=text, xalign=0, css_classes=["caption-heading", "dim-label"]), col, 0, 1, 1)
        for row, p in enumerate([self.s] + self.s["AI"], 1):
            human = p is self.s
            if p["Start"] >= starts:
                p["Start"] = RANDOM
            upd = lambda key, off=-1, p=p: (lambda v: self.set_player(p, key, v + off))
            label = Gtk.Label(label="You" if human else f"AI {row - 1}", xalign=0, width_chars=4,
                              css_classes=["heading"] if human else [])
            cells = [label,
                     dropdown(choice_model(COUNTRIES, True), p["Country"] + 1, upd("Country")),
                     dropdown(choice_model([c for c, _ in COLORS], True), p["Color"] + 1, upd("Color"),
                              color_factory()),
                     pills([(t, i - 1) for i, t in enumerate(TEAMS)], p["Team"], upd("Team", 0)),
                     dropdown(choice_model([str(n) for n in range(1, starts + 1)], True), p["Start"] + 1,
                              upd("Start"))]
            if human:
                cells.append(Gtk.Label(label="Human", xalign=0, css_classes=["dim-label"]))
            else:
                cells.append(pills([(d, i) for i, d in enumerate(DIFFICULTIES)][::-1], p["Difficulty"],
                                   upd("Difficulty", 0)))
                remove = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER,
                                    tooltip_text="Remove opponent", css_classes=["flat"])
                remove.connect("clicked", lambda _, p=p: self.remove_ai(p))
                cells.append(remove)
            cells[1].set_hexpand(True)
            cells[2].set_hexpand(True)
            for col, w in enumerate(cells):
                grid.attach(w, col, row, 1, 1)
        self.marks.queue_draw()

    def set_player(self, p, key, value):
        p[key] = value
        self.marks.queue_draw()
        self.refresh()

    def build_game_group(self):
        g = Adw.PreferencesGroup(title="Game")
        speed = Adw.ComboRow(title="Game speed", model=Gtk.StringList.new(SPEEDS))
        speed.set_selected(self.s["GameSpeed"])
        speed.connect("notify::selected", lambda r, _: self.set("GameSpeed", r.get_selected()))
        g.add(speed)
        base = Adw.ComboRow(title="Starting base", model=Gtk.StringList.new(startbase.TIERS))
        base.set_selected(self.s["StartBase"])
        base.set_subtitle(startbase.TIER_TEXT[self.s["StartBase"]])
        base.connect("notify::selected", lambda r, _: (r.set_subtitle(startbase.TIER_TEXT[r.get_selected()]),
                                                       self.set("StartBase", r.get_selected())))
        g.add(base)
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
            if key == "HumanInPeace":
                row.set_subtitle("AI leaves you alone, including superweapons, on any team.")
            row.connect("notify::active", lambda r, _, k=key: self.set(k, r.get_active()))
            g.add(row)
            if key == "Bases":
                self.bases_row = row
        return g

    # ---- state ----
    def set(self, key, value):
        self.s[key] = value
        self.refresh()

    def current_map(self):
        return self.maps.get(self.s["Map"])

    def refresh(self):
        # a starting base takes the place of the MCV, so it needs bases on
        self.bases_row.set_sensitive(not self.s["StartBase"])
        if self.s["StartBase"] and not self.s["Bases"]:
            self.bases_row.set_active(True)   # calls refresh again through set()
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
        self.players_group.set_description(issues[0] if issues and self.maps else None)

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
        m, text = row.map, self.search.get_text().strip().casefold()
        count = self.player_count.get_selected()
        return ("standard" in m["modes"] and (count == 0 or m["max"] == count + 1)
                and (not text or text in m["name"].casefold() or text in m["file"].casefold()))

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
        self.rebuild_players()   # the Start choices depend on the map
        self.refresh()

    def map_texture(self, m):
        try:
            info = spawn.map_info(m["file"])
            text = mixextract.extract(info["data"], info["map"].lower() + ".map").decode("latin-1")
            w, h, rgb = mappreview.preview(text)
            self.start_points, self.preview_size = mappreview.start_points(text), (w, h)
        except Exception:
            self.start_points, self.preview_size = {}, None
            return None
        # double the pixels so the scaled-up picture stays crisp
        rows = [rgb[y * w * 3:(y + 1) * w * 3] for y in range(h)]
        rows = [b"".join(r[x:x + 3] * 2 for x in range(0, len(r), 3)) for r in rows]
        data = b"".join(r + r for r in rows)
        return Gdk.MemoryTexture.new(w * 2, h * 2, Gdk.MemoryFormat.R8G8B8, GLib.Bytes.new(data), w * 6)

    def draw_marks(self, area, cr, width, height):
        """Numbered start positions over the preview, filled with the colour of the player who has chosen it."""
        if not self.preview_size:
            return
        pw, ph = self.preview_size
        scale = min(width / pw, height / ph)
        ox, oy = (width - pw * scale) / 2, (height - ph * scale) / 2
        taken = {p["Start"]: p["Color"] for p in [self.s] + self.s["AI"] if p["Start"] != RANDOM}
        cr.select_font_face("sans-serif", 0, 1)
        cr.set_font_size(13)
        for n, (x, y) in self.start_points.items():
            if n >= self.current_map()["max"]:
                continue
            x, y = ox + x * scale, oy + y * scale
            cr.new_path()
            cr.arc(x, y, 11, 0, 6.2832)
            if n in taken and taken[n] != RANDOM:
                rgb = COLORS[taken[n]][1]
                cr.set_source_rgb(*(int(rgb[i:i + 2], 16) / 255 for i in (1, 3, 5)))
            else:
                cr.set_source_rgba(0, 0, 0, .65 if n not in taken else .9)
            cr.fill_preserve()
            cr.set_source_rgb(1, 1, 1)
            cr.set_line_width(1.5)
            cr.stroke()
            ext = cr.text_extents(str(n + 1))
            cr.move_to(x - ext.x_bearing - ext.width / 2, y - ext.y_bearing - ext.height / 2)
            cr.show_text(str(n + 1))

    def on_random_map(self, _):
        rows = [r for r in self.map_rows if self.map_visible(r) and r.map["max"] >= 1 + len(self.s["AI"])]
        if rows:
            self.select_map(random.choice(rows).map["file"])

    # ---- opponents ----
    def on_add_ai(self, _):
        info = self.current_map()
        if info and 1 + len(self.s["AI"]) >= info["max"]:
            return
        used = {self.s["Color"]} | {a["Color"] for a in self.s["AI"]}
        color = next((c for c in range(len(COLORS)) if c not in used), RANDOM)
        self.s["AI"].append({"Country": RANDOM, "Color": color, "Difficulty": 2, "Start": RANDOM, "Team": RANDOM})
        self.rebuild_players()
        self.refresh()

    def remove_ai(self, ai):
        self.s["AI"].remove(ai)
        self.rebuild_players()
        self.refresh()

    # ---- presets ----
    def build_presets_menu(self):
        """Header menu: save the current settings under a name, and load or delete saved ones."""
        self.preset_name = Gtk.Entry(placeholder_text="Preset name", hexpand=True)
        self.preset_name.connect("activate", self.on_save_preset)
        save = Gtk.Button(label="Save", css_classes=["suggested-action"])
        save.connect("clicked", self.on_save_preset)
        entry_box = Gtk.Box(spacing=6)
        entry_box.append(self.preset_name)
        entry_box.append(save)
        self.preset_list = Gtk.ListBox(css_classes=["boxed-list"], selection_mode=Gtk.SelectionMode.NONE)
        self.preset_list.set_placeholder(Gtk.Label(label="No saved presets", margin_top=12, margin_bottom=12,
                                                   css_classes=["dim-label"]))
        self.preset_list.connect("row-activated", lambda _, row: self.load_preset(row.preset))
        scroll = Gtk.ScrolledWindow(child=self.preset_list, propagate_natural_height=True, max_content_height=360,
                                    hscrollbar_policy=Gtk.PolicyType.NEVER)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, width_request=300,
                      margin_start=6, margin_end=6, margin_top=6, margin_bottom=6)
        box.append(entry_box)
        box.append(scroll)
        self.presets_popover = Gtk.Popover(child=box)
        button = Gtk.MenuButton(icon_name="document-open-symbolic", tooltip_text="Presets",
                                popover=self.presets_popover)
        button.connect("notify::active", lambda b, _: b.get_active() and self.fill_presets())
        return button

    def fill_presets(self):
        self.preset_list.remove_all()
        for name in load_presets():
            row = Adw.ActionRow(title=GLib.markup_escape_text(name), activatable=True)
            row.preset = name
            delete = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER,
                                tooltip_text="Delete preset", css_classes=["flat"])
            delete.connect("clicked", lambda _, n=name: self.delete_preset(n))
            row.add_suffix(delete)
            self.preset_list.append(row)

    def on_save_preset(self, _):
        name = self.preset_name.get_text().strip()
        if not name:
            self.preset_name.grab_focus()
            return
        presets = load_presets()
        verb = "Updated" if name in presets else "Saved"
        presets[name] = json.loads(json.dumps(self.s))
        save_presets(presets)
        self.preset_name.set_text("")
        self.fill_presets()
        self.toast(f"{verb} preset “{name}”")

    def delete_preset(self, name):
        presets = load_presets()
        presets.pop(name, None)
        save_presets(presets)
        self.fill_presets()
        self.toast(f"Deleted preset “{name}”")

    def load_preset(self, name):
        saved = load_presets().get(name)
        self.presets_popover.popdown()
        if saved is None:
            return self.toast(f"Preset “{name}” is gone")
        self.s = normalize(saved)
        missing = self.maps and self.s["Map"] not in self.maps
        if missing:
            self.s["Map"] = self.map_list.get_selected_row().map["file"]
        self.build_settings_groups()
        if self.maps:   # otherwise fill_maps selects self.s["Map"] when the maps arrive
            self.player_count.set_selected(0)
            self.search.set_text("")
            row = next(r for r in self.map_rows if r.map["file"] == self.s["Map"])
            if row is self.map_list.get_selected_row():
                self.on_map_selected(None, row)   # select_row would not signal; the Start choices need the map
            else:
                self.select_map(self.s["Map"])
        self.refresh()
        self.toast(f"Loaded preset “{name}”" + (", but its map is missing; kept the current one" if missing else ""))

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
