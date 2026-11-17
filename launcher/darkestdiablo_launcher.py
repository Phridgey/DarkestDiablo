"""Darkest Diablo launcher window.

Run from source:  python launcher/darkestdiablo_launcher.py
Build the .exe:   tools\\build-launcher.cmd

First run: finds the D2R install, asks where the Darkest Diablo game folder goes, copies the
game there (the normal install is never changed), sets up D2RLoader, downloads the mod.
After that: shows Update when a new release is out, otherwise Play.

The look is a nod to the old installer menus: a dark screen, a gold title, a short list of
text choices that light up under the mouse, and a few embers. All of it is drawn here; there
are no image or sound files. It is silent on purpose.
"""
import queue, random, sys, threading, webbrowser
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, font as tkfont, messagebox, simpledialog

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ddcore  # noqa: E402

W, H = 760, 520
BG_TOP, BG_BOTTOM = (8, 6, 5), (34, 10, 6)
GOLD, GOLD_HI, GOLD_DIM = "#c8a25a", "#f3dc9a", "#6b5a3a"
TEXT, TEXT_DIM, RED = "#cfc3a8", "#7d7465", "#c0392b"


def pick_font(*names):
    have = {f.lower() for f in tkfont.families()}
    return next((n for n in names if n.lower() in have), "Times")


class Launcher:
    def __init__(self, root):
        self.root = root
        root.title("Darkest Diablo")
        root.resizable(False, False)
        root.configure(bg="black")
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.c = tk.Canvas(root, width=W, height=H, highlightthickness=0, bg="black")
        self.c.pack()
        serif = pick_font("Palatino Linotype", "Book Antiqua", "Georgia", "Times New Roman", "Liberation Serif")
        self.f_title = (serif, 34, "bold")
        self.f_sub = (serif, 12, "italic")
        self.f_menu = (serif, 17)
        self.f_text = (serif, 11)
        self.f_small = (serif, 9)

        self.settings = ddcore.load_settings()
        self.events = queue.Queue()
        self.busy = False
        self.release = None
        self.embers = []
        self.draw_backdrop()
        self.root.after(50, self.pump)
        self.root.after(40, self.tick_embers)
        self.start()

    # ------------------------------------------------------------ drawing

    def draw_backdrop(self):
        c = self.c
        steps = 64
        for i in range(steps):
            t = i / (steps - 1)
            col = "#%02x%02x%02x" % tuple(int(a + (b - a) * t ** 1.6) for a, b in zip(BG_TOP, BG_BOTTOM))
            c.create_rectangle(0, i * H / steps, W, (i + 1) * H / steps + 1, fill=col, outline="")
        # a faint glow under the title, like firelight
        for i in range(12):
            r = 230 - i * 12
            col = "#%02x%02x%02x" % (12 + i * 2, 7 + i // 2, 5 + i // 3)
            c.create_oval(W / 2 - r * 1.6, 105 - r * 0.45, W / 2 + r * 1.6, 105 + r * 0.45, fill=col, outline="")
        # stone frame: two thin lines with small diamonds in the corners
        for inset, col in ((10, GOLD_DIM), (15, "#3a3022")):
            c.create_rectangle(inset, inset, W - inset, H - inset, outline=col)
        for x, y in ((12, 12), (W - 12, 12), (12, H - 12), (W - 12, H - 12)):
            c.create_polygon(x, y - 6, x + 6, y, x, y + 6, x - 6, y, fill=GOLD, outline="")
        # title, letter-spaced, with a dark shadow
        title = "D A R K E S T   D I A B L O"
        c.create_text(W / 2 + 3, 93, text=title, font=self.f_title, fill="#000000")
        c.create_text(W / 2, 90, text=title, font=self.f_title, fill=GOLD)
        c.create_line(W / 2 - 230, 128, W / 2 + 230, 128, fill=GOLD_DIM)
        c.create_polygon(W / 2, 122, W / 2 + 7, 128, W / 2, 134, W / 2 - 7, 128, fill=RED, outline=GOLD_DIM)
        c.create_text(W / 2, 150, text="a roguelite for Diablo II: Resurrected", font=self.f_sub, fill=TEXT_DIM)

    def tick_embers(self):
        c = self.c
        if len(self.embers) < 22 and random.random() < 0.3:
            x = random.uniform(30, W - 30)
            r = random.uniform(1, 2.2)
            item = c.create_oval(x - r, H - 20 - r, x + r, H - 20 + r, fill="#e0742a", outline="")
            if c.find_withtag("ui"):
                c.tag_lower(item, "ui")
            self.embers.append([item, random.uniform(-0.25, 0.25), random.uniform(0.5, 1.2), random.randint(160, 320)])
        alive = []
        for e in self.embers:
            item, dx, dy, life = e
            e[3] -= 1
            if e[3] <= 0:
                c.delete(item)
                continue
            c.move(item, dx + random.uniform(-0.2, 0.2), -dy)
            if e[3] < 60:
                c.itemconfigure(item, fill="#7a3416" if e[3] > 30 else "#3d1a0b")
            alive.append(e)
        self.embers = alive
        self.root.after(40, self.tick_embers)

    def clear(self):
        self.c.delete("ui")

    def text(self, y, s, color=TEXT, fnt=None, width=620):
        return self.c.create_text(W / 2, y, text=s, fill=color, font=fnt or self.f_text, width=width,
                                  justify="center", tags="ui")

    def menu(self, items, top=300, gap=38):
        """Text choices like the old installer: gold, brighter with a marker under the mouse."""
        y = top
        for label, action in items:
            enabled = action is not None
            tid = self.c.create_text(W / 2, y, text=label, font=self.f_menu,
                                     fill=GOLD if enabled else TEXT_DIM, tags="ui")
            if enabled:
                tag = f"item{tid}"
                self.c.addtag_withtag(tag, tid)
                x1, y1, x2, y2 = self.c.bbox(tid)
                marks = [self.c.create_text(x1 - 18, y, text="✦", fill=RED, font=self.f_text, state="hidden", tags=("ui", tag)),
                         self.c.create_text(x2 + 18, y, text="✦", fill=RED, font=self.f_text, state="hidden", tags=("ui", tag))]
                hit = self.c.create_rectangle(x1 - 30, y1 - 4, x2 + 30, y2 + 4, outline="", fill="", tags=("ui", tag))
                self.c.tag_lower(hit, tid)

                def enter(_e, tid=tid, marks=marks):
                    self.c.itemconfigure(tid, fill=GOLD_HI)
                    for m in marks:
                        self.c.itemconfigure(m, state="normal")
                    self.c.configure(cursor="hand2")

                def leave(_e, tid=tid, marks=marks):
                    self.c.itemconfigure(tid, fill=GOLD)
                    for m in marks:
                        self.c.itemconfigure(m, state="hidden")
                    self.c.configure(cursor="")

                self.c.tag_bind(tag, "<Enter>", enter)
                self.c.tag_bind(tag, "<Leave>", leave)
                self.c.tag_bind(tag, "<Button-1>", lambda _e, a=action: (self.c.configure(cursor=""), a()))
            y += gap

    def path_row(self, y, label, value, on_change, ok=None):
        self.c.create_text(70, y, text=label, anchor="w", fill=TEXT_DIM, font=self.f_small, tags="ui")
        shown = value or "(not chosen)"
        if len(shown) > 70:
            shown = shown[:30] + " ... " + shown[-35:]
        col = TEXT if ok is None else ("#9fbf7a" if ok else "#d9775f")
        self.c.create_rectangle(70, y + 10, W - 170, y + 38, outline=GOLD_DIM, fill="#0d0907", tags="ui")
        self.c.create_text(80, y + 24, text=shown, anchor="w", fill=col, font=self.f_text, tags="ui")
        b = self.c.create_text(W - 120, y + 24, text="Browse...", fill=GOLD, font=self.f_text, tags="ui")
        self.c.addtag_withtag(f"b{b}", b)
        self.c.tag_bind(f"b{b}", "<Enter>", lambda _e: self.c.itemconfigure(b, fill=GOLD_HI))
        self.c.tag_bind(f"b{b}", "<Leave>", lambda _e: self.c.itemconfigure(b, fill=GOLD))
        self.c.tag_bind(f"b{b}", "<Button-1>", lambda _e: on_change())

    def progress_screen(self, title):
        self.clear()
        self.text(215, title, GOLD, self.f_menu)
        self.status_id = self.text(255, "", TEXT_DIM, self.f_small)
        self.c.create_rectangle(110, 285, W - 110, 305, outline=GOLD_DIM, fill="#0d0907", tags="ui")
        self.bar = self.c.create_rectangle(112, 287, 112, 303, outline="", fill="#9c3b1c", tags="ui")
        self.bar_glow = self.c.create_line(112, 288, 112, 288, fill="#e08a4a", tags="ui")
        self.pct_id = self.text(325, "", TEXT_DIM, self.f_small)

    def set_progress(self, done, total, label):
        if not hasattr(self, "bar"):
            return
        frac = 0 if not total else max(0.0, min(1.0, done / total))
        x = 112 + (W - 224 - 4) * frac
        self.c.coords(self.bar, 112, 287, x, 303)
        self.c.coords(self.bar_glow, 112, 288, x, 288)
        self.c.itemconfigure(self.status_id, text=label)
        self.c.itemconfigure(self.pct_id, text=f"{frac * 100:.0f}%" + (f"   {ddcore.gb(done)} of {ddcore.gb(total)}" if total > 1 else ""))

    # ------------------------------------------------------------ background work

    def run(self, work, done=None, failed=None):
        """Run work() off the window thread; then done(result) or failed(error) on it."""
        self.busy = True

        def go():
            try:
                self.events.put(("done", done, work()))
            except Exception as e:  # shown to the player, never swallowed
                self.events.put(("failed", failed, e))
        threading.Thread(target=go, daemon=True).start()

    def report(self, done, total, label):
        self.events.put(("progress", None, (done, total, label)))

    def pump(self):
        try:
            while True:
                kind, cb, value = self.events.get_nowait()
                if kind == "progress":
                    self.set_progress(*value)
                    continue
                self.busy = False
                if kind == "done" and cb:
                    cb(value)
                elif kind == "failed":
                    (cb or self.show_error)(value)
        except queue.Empty:
            pass
        self.root.after(50, self.pump)

    def on_close(self):
        if self.busy and not messagebox.askyesno("Darkest Diablo", "Still working. Quit anyway?\n"
                                                 "A stopped copy picks up where it left off next time."):
            return
        self.root.destroy()

    def show_error(self, e):
        self.clear()
        self.text(220, "Something went wrong", RED, self.f_menu)
        self.text(265, str(e), TEXT)
        self.menu([("Try again", self.start), ("Quit", self.root.destroy)], top=340)

    # ------------------------------------------------------------ flow

    def start(self):
        game = self.settings.get("game_dir")
        if game and ddcore.is_d2r_folder(game):
            self.main_screen()
        else:
            if not self.settings.get("d2r_dir"):
                found = ddcore.find_d2r_installs()
                if found:
                    self.settings["d2r_dir"] = found[0]
            self.settings.setdefault("game_dir", str(Path.home() / "Games" / "DarkestDiablo"))
            self.setup_screen()

    def setup_screen(self):
        self.clear()
        s = self.settings
        d2r = s.get("d2r_dir")
        wanted = (self.release.manifest.get("d2r_version") if self.release else None) or ddcore.FALLBACK_D2R_VERSION
        have = ddcore.read_build_version(d2r) if d2r else None
        ok = bool(d2r) and ddcore.is_d2r_folder(d2r) and ddcore.version_matches(have, wanted)
        if not d2r:
            note = "Couldn't find Diablo II: Resurrected. Point to the folder that has D2R.exe in it."
        elif not ddcore.is_d2r_folder(d2r):
            note = "That folder has no D2R.exe and .build.info in it."
        elif not ok:
            note = (f"This D2R is build {have}; Darkest Diablo needs build {wanted}. "
                    "Point to a copy of that build.")
        else:
            note = f"Found build {have}. It is only read from, never changed."
        self.text(185, "Setup", GOLD, self.f_menu)
        self.path_row(205, "YOUR DIABLO II: RESURRECTED", d2r, self.choose_d2r, ok)
        self.text(262, note, TEXT_DIM, self.f_small)
        self.path_row(275, "PUT THE DARKEST DIABLO GAME FOLDER HERE", s.get("game_dir"), self.choose_game)
        target_err = ddcore.check_game_target(d2r, s["game_dir"]) if d2r else None
        self.text(332, target_err or "A full copy of the game goes here (about as big as your D2R folder).",
                  RED if target_err else TEXT_DIM, self.f_small)
        self.menu([("Install", self.install if ok and not target_err else None), ("Quit", self.root.destroy)], top=385)

    def choose_d2r(self):
        p = filedialog.askdirectory(title="Your Diablo II: Resurrected folder (has D2R.exe)",
                                    initialdir=self.settings.get("d2r_dir") or None)
        if p:
            self.settings["d2r_dir"] = str(Path(p))
            self.setup_screen()

    def choose_game(self):
        p = filedialog.askdirectory(title="Where should the Darkest Diablo game folder go?", mustexist=False)
        if p:
            p = Path(p)
            if p.exists() and any(p.iterdir()) and not ddcore.is_d2r_folder(p):
                p = p / "DarkestDiablo"  # don't spill the game into a folder that has other things in it
            self.settings["game_dir"] = str(p)
            self.setup_screen()

    def install(self):
        s = self.settings
        self.progress_screen("Copying the game")
        self.run(lambda: ddcore.copy_game(s["d2r_dir"], s["game_dir"], lambda d, t, n: self.report(d, t, n)),
                 lambda _: self.after_copy())

    def after_copy(self):
        ddcore.save_settings(self.settings)
        if ddcore.has_d2rloader(self.settings["game_dir"]):
            self.install_mod()
        else:
            self.d2rloader_screen()

    def d2rloader_screen(self):
        self.clear()
        self.text(190, "One more piece: D2RLoader", GOLD, self.f_menu)
        self.text(245, "Darkest Diablo runs through D2RLoader (version 1.3.1 or later). It comes from "
                       "Nexus Mods, which needs a free account, so it can't be downloaded for you. "
                       "Download the zip there, then choose it here.", TEXT)
        self.menu([("Open the D2RLoader page", lambda: webbrowser.open(ddcore.D2RLOADER_PAGE)),
                   ("Choose the downloaded zip", self.choose_loader_zip),
                   ("Quit", self.root.destroy)], top=330)

    def choose_loader_zip(self):
        z = filedialog.askopenfilename(title="The D2RLoader zip you downloaded",
                                       filetypes=[("Zip files", "*.zip")], initialdir=str(Path.home() / "Downloads"))
        if not z:
            return
        try:
            ddcore.install_d2rloader(z, self.settings["game_dir"])
        except Exception as e:
            messagebox.showerror("Darkest Diablo", str(e))
            return
        self.install_mod()

    def install_mod(self):
        self.progress_screen("Fetching Darkest Diablo")
        game = self.settings["game_dir"]

        def work():
            self.release = self.release or self.fetch_release()
            ddcore.update(game, self.release, self.report)
        self.run(work, lambda _: self.main_screen(), self.release_failed)

    def fetch_release(self):
        return ddcore.latest_release(self.settings.get("token"))

    def release_failed(self, e):
        if isinstance(e, ddcore.NeedToken):
            return self.token_screen()
        self.show_error(e)

    def token_screen(self):
        self.clear()
        self.text(190, "The updates are private for now", GOLD, self.f_menu)
        self.text(250, "Darkest Diablo's repository is private, so the launcher needs a GitHub access "
                       "token that can read it (a fine-grained token with Contents: read on "
                       f"{ddcore.REPO}). It is kept only on this PC.", TEXT)
        installed = ddcore.read_install(self.settings.get("game_dir", "")) if self.settings.get("game_dir") else None
        items = [("Enter a token", self.ask_token), ("Try again", self.main_screen if installed else self.install_mod)]
        if installed:
            items.append(("Play without updating", self.play))
        items.append(("Quit", self.root.destroy))
        self.menu(items, top=330)

    def ask_token(self):
        t = simpledialog.askstring("GitHub token", "Paste the token:", show="*", parent=self.root)
        if t:
            self.settings["token"] = t.strip()
            ddcore.save_settings(self.settings)
            self.release = None
            if ddcore.read_install(self.settings["game_dir"]):
                self.main_screen()
            else:
                self.install_mod()

    def main_screen(self):
        self.clear()
        game = self.settings["game_dir"]
        installed = ddcore.read_install(game)
        dev = ddcore.is_dev_link(game)
        if dev:
            self.text(205, "Developer build", GOLD, self.f_menu)
            self.text(240, "Mods\\darkestdiablo links to your own build (tools\\link.cmd), "
                           "so the launcher doesn't update it.", TEXT_DIM, self.f_small)
            return self.menu_main(update=None)
        version = installed["version"] if installed and ddcore.mod_dir(game).exists() else None
        self.text(205, f"Version {version}" if version else "Not installed yet", GOLD, self.f_menu)
        self.check_id = self.text(240, "Looking for updates...", TEXT_DIM, self.f_small)
        self.menu_main(update=None, playable=bool(version))

        def done(rel):
            self.release = rel
            if not self.c.find_withtag(self.check_id):
                return
            have = ddcore.read_build_version(game)
            warn = "" if ddcore.version_matches(have, rel.manifest.get("d2r_version")) else \
                f"\nThis game copy is build {have}; release {rel.version} is made for build {rel.manifest.get('d2r_version')}."
            if version and ddcore.version_key(rel.version) <= ddcore.version_key(version):
                self.c.itemconfigure(self.check_id, text="Up to date." + warn)
                return
            name, size = rel.best_download(version)
            what = "Update" if version else "Install"
            self.c.itemconfigure(self.check_id, text=f"{what} {rel.version} is ready ({ddcore.gb(size)})." + warn
                                 + (f"\n{rel.notes.strip().splitlines()[0][:90]}" if rel.notes.strip() else ""))
            self.clear_menu_and(lambda: self.menu_main(update=rel, playable=bool(version)))

        def failed(e):
            if not self.c.find_withtag(self.check_id):
                return
            if isinstance(e, ddcore.NeedToken):
                msg = ("Your GitHub token was not accepted." if self.settings.get("token")
                       else "Updates need a GitHub token (the repository is private).")
                self.clear_menu_and(lambda: self.menu_main(update=None, playable=bool(version), token=True))
            else:
                msg = f"Couldn't check for updates: {e}"
            self.c.itemconfigure(self.check_id, text=msg)
        self.run(self.fetch_release, done, failed)

    def clear_menu_and(self, draw):
        for item in self.c.find_withtag("menu"):
            self.c.delete(item)
        draw()

    def menu_main(self, update, playable=True, token=False):
        items = []
        if update is not None:
            items.append((f"Update to {update.version}" if playable else "Install Darkest Diablo", self.install_mod))
        if playable or update is None:
            items.append(("Play", self.play if playable or ddcore.is_dev_link(self.settings["game_dir"]) else None))
        if token:
            items.append(("Enter a GitHub token", self.ask_token))
        items.append(("Open game folder", lambda: webbrowser.open(Path(self.settings["game_dir"]).as_uri())))
        items.append(("Quit", self.root.destroy))
        before = set(self.c.find_withtag("ui"))
        self.menu(items, top=300)
        for item in set(self.c.find_withtag("ui")) - before:
            self.c.addtag_withtag("menu", item)

    def play(self):
        game = self.settings["game_dir"]
        if not ddcore.battlenet_running() and not messagebox.askyesno(
                "Darkest Diablo", "The Battle.net app isn't running, and the game needs it open.\n\n"
                                  "Open Battle.net first, then press Yes. Start anyway?"):
            return
        if not self.settings.get("default_mod_tip_shown"):
            messagebox.showinfo("First time only", "When D2RLoader opens: click the gear under Play, set "
                                "Default mod to \"darkestdiablo\", press Apply and let it restart.")
            self.settings["default_mod_tip_shown"] = True
            ddcore.save_settings(self.settings)
        try:
            ddcore.play(game)
        except Exception as e:
            return messagebox.showerror("Darkest Diablo", str(e))
        self.root.after(1500, self.root.destroy)


def main():
    root = tk.Tk()
    Launcher(root)
    root.mainloop()


if __name__ == "__main__":
    main()
