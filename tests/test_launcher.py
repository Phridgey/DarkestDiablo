"""Tests for the launcher's install and update logic (no network, no game needed).

Run: python -m unittest discover tests
"""
import json, os, sys, tempfile, unittest, zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "launcher"))
sys.path.insert(0, str(ROOT / "tools"))
import ddcore  # noqa: E402
import release  # noqa: E402


class FakeRelease:
    """Stands in for ddcore.Release, serving files from a local folder."""

    def __init__(self, folder):
        self.folder = Path(folder)
        self.manifest = json.loads((self.folder / "manifest.json").read_text())
        self.version = self.manifest["version"]
        self.assets = {p.name: {"size": p.stat().st_size} for p in self.folder.iterdir()}
        self.token = None
        self.notes = ""

    best_download = ddcore.Release.best_download

    def asset_url(self, name):
        return (self.folder / name).as_uri()


def fake_download(url, dest, token=None, progress=lambda d, t: None):
    from urllib.parse import urlparse
    from urllib.request import url2pathname
    data = Path(url2pathname(urlparse(url).path)).read_bytes()
    Path(dest).write_bytes(data)
    progress(len(data), len(data))


class Workspace(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = Path(self.tmp.name)
        self.build = self.t / "build" / "darkestdiablo"
        self.out = self.t / "build" / "release"
        self.game = self.t / "game"
        self.game.mkdir()
        (self.game / "D2R.exe").write_text("exe")
        (self.game / ".build.info").write_text("Branch!STRING:0|Version!STRING:0|Product!STRING:0\nus|3.4.95032|osi\n")
        self.published = {}  # version -> manifest
        self.patches = [mock.patch.object(release, "BUILD", self.build), mock.patch.object(release, "OUT", self.out),
                        mock.patch.object(release, "earlier_manifests", lambda: dict(self.published)),
                        mock.patch.object(ddcore, "download", fake_download)]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def write_build(self, files):
        import shutil
        if self.build.exists():
            shutil.rmtree(self.build)
        for rel, data in files.items():
            p = self.build / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)

    def make_release(self, version):
        with mock.patch.object(sys, "argv", ["release.py", version]):
            release.main()
        folder = self.out / version
        self.published[version] = json.loads((folder / "manifest.json").read_text())
        return FakeRelease(folder)

    def installed_files(self):
        base = ddcore.mod_dir(self.game)
        return {p.relative_to(base).as_posix(): p.read_bytes() for p in base.rglob("*") if p.is_file()}


V1 = {"modinfo.json": b'{"name":"darkestdiablo"}',
      "darkestdiablo.mpq/data/global/excel/levels.txt": b"levels v1",
      "darkestdiablo.mpq/data/global/excel/monstats.txt": b"monstats v1",
      "d2rloader/plugins/darkestdiablo.dll": b"dll v1",
      "d2rloader/logs/darkestdiablo.log": b"local log, never shipped"}


class UpdateTests(Workspace):
    def test_install_then_delta_update(self):
        self.write_build(V1)
        r1 = self.make_release("0.1.0")
        self.assertNotIn("d2rloader/logs/darkestdiablo.log", r1.manifest["files"])
        ddcore.update(self.game, r1)
        self.assertEqual(ddcore.read_install(self.game)["version"], "0.1.0")
        self.assertEqual(len(self.installed_files()), 4)

        v2 = dict(V1)
        v2["darkestdiablo.mpq/data/global/excel/levels.txt"] = b"levels v2"
        del v2["darkestdiablo.mpq/data/global/excel/monstats.txt"]
        v2["darkestdiablo.mpq/data/global/excel/skills.txt"] = b"skills new"
        self.write_build(v2)
        r2 = self.make_release("0.2.0")
        name, _ = r2.best_download("0.1.0")
        self.assertEqual(name, "darkestdiablo-delta-from-0.1.0.zip")
        with zipfile.ZipFile(self.out / "0.2.0" / name) as z:
            self.assertEqual(sorted(z.namelist()), ["darkestdiablo.mpq/data/global/excel/levels.txt",
                                                    "darkestdiablo.mpq/data/global/excel/skills.txt", "delta.json"])
        ddcore.update(self.game, r2)
        files = self.installed_files()
        self.assertEqual(files["darkestdiablo.mpq/data/global/excel/levels.txt"], b"levels v2")
        self.assertNotIn("darkestdiablo.mpq/data/global/excel/monstats.txt", files)
        self.assertEqual(ddcore.read_install(self.game)["version"], "0.2.0")

    def test_old_install_gets_full_package(self):
        self.write_build(V1)
        r1 = self.make_release("0.1.0")
        self.assertEqual(r1.best_download(None)[0], "darkestdiablo-full.zip")
        self.assertEqual(r1.best_download("0.0.1")[0], "darkestdiablo-full.zip")

    def test_locally_changed_file_falls_back_to_full(self):
        self.write_build(V1)
        ddcore.update(self.game, self.make_release("0.1.0"))
        (ddcore.mod_dir(self.game) / "modinfo.json").write_bytes(b"edited by hand!!!!!!!!!")
        v2 = dict(V1, **{"d2rloader/plugins/darkestdiablo.dll": b"dll v2"})
        self.write_build(v2)
        ddcore.update(self.game, self.make_release("0.2.0"))
        self.assertEqual(self.installed_files()["modinfo.json"], V1["modinfo.json"])

    def test_corrupt_download_changes_nothing(self):
        self.write_build(V1)
        r1 = self.make_release("0.1.0")
        ddcore.update(self.game, r1)
        bad = self.t / "bad.zip"
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("modinfo.json", b"tampered")
        manifest = dict(r1.manifest, version="0.2.0")
        with self.assertRaises(RuntimeError):
            ddcore.apply_package(bad, self.game, manifest, ddcore.read_install(self.game))
        self.assertEqual(self.installed_files()["modinfo.json"], V1["modinfo.json"])
        self.assertEqual(ddcore.read_install(self.game)["version"], "0.1.0")

    def test_unsafe_paths_refused(self):
        for bad in ["../evil.dll", "/abs/x", "C:/x", "a/../../b"]:
            with self.assertRaises(RuntimeError):
                ddcore.safe_rel(bad)

    def test_release_refuses_lower_version_and_no_change(self):
        self.write_build(V1)
        self.make_release("0.2.0")
        with self.assertRaises(SystemExit):
            self.make_release("0.1.0")
        with self.assertRaises(SystemExit):
            self.make_release("0.3.0")  # nothing changed

    @unittest.skipIf(os.name == "nt", "symlinks need extra rights on Windows")
    def test_dev_link_left_alone(self):
        self.write_build(V1)
        r1 = self.make_release("0.1.0")
        (self.game / "Mods").mkdir()
        os.symlink(self.build, ddcore.mod_dir(self.game))
        self.assertTrue(ddcore.is_dev_link(self.game))
        with self.assertRaises(RuntimeError):
            ddcore.update(self.game, r1)


class SetupTests(unittest.TestCase):
    def test_build_version(self):
        with tempfile.TemporaryDirectory() as t:
            Path(t, ".build.info").write_text("Branch!STRING:0|Version!STRING:0\nus|3.4.95032\n")
            self.assertEqual(ddcore.read_build_version(t), "3.4.95032")
            self.assertTrue(ddcore.version_matches("3.4.95032", "3.4.95032"))
            self.assertFalse(ddcore.version_matches("3.4.95999", "3.4.95032"))

    def test_copy_skips_mod_leftovers_and_resumes(self):
        with tempfile.TemporaryDirectory() as t:
            src, dst = Path(t, "d2r"), Path(t, "copy")
            for rel in ["D2R.exe", ".build.info", "Data/data.000", "Mods/other/x.txt", "D2RHUD.dll", "D2RHUD_Config.txt"]:
                (src / rel).parent.mkdir(parents=True, exist_ok=True)
                (src / rel).write_text(rel)
            ddcore.copy_game(src, dst)
            got = sorted(p.relative_to(dst).as_posix() for p in dst.rglob("*") if p.is_file())
            self.assertEqual(got, [".build.info", "D2R.exe", "Data/data.000"])
            ddcore.copy_game(src, dst)  # second run skips everything

    def test_game_folder_never_inside_d2r(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertIsNotNone(ddcore.check_game_target(t, t))
            self.assertIsNotNone(ddcore.check_game_target(t, Path(t, "sub")))
            self.assertIsNone(ddcore.check_game_target(Path(t, "a"), Path(t, "b")))
            with self.assertRaises(RuntimeError):
                ddcore.copy_game(t, Path(t, "sub"))

    def test_d2rloader_zip_with_subfolder(self):
        with tempfile.TemporaryDirectory() as t:
            z = Path(t, "loader.zip")
            with zipfile.ZipFile(z, "w") as f:
                f.writestr("D2RLoader-1.3.1/D2RLoader.exe", "exe")
                f.writestr("D2RLoader-1.3.1/d2rloader/config/d2rloader.toml", "x")
            ddcore.install_d2rloader(z, t)
            self.assertTrue(ddcore.has_d2rloader(t))
            self.assertTrue(Path(t, "d2rloader/config/d2rloader.toml").is_file())


import ddmain  # noqa: E402


class MainChannelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = Path(self.tmp.name)
        self.game = self.t / "game"
        self.game.mkdir()
        self.ws = ddmain.workspace(self.game)

    def tearDown(self):
        self.tmp.cleanup()

    def built(self, files):
        import shutil
        b = self.ws / "build" / "darkestdiablo"
        if b.exists():
            shutil.rmtree(b)
        for rel, data in files.items():
            (b / rel).parent.mkdir(parents=True, exist_ok=True)
            (b / rel).write_bytes(data)

    def test_source_refresh_keeps_vanilla_and_build(self):
        (self.ws / "vanilla" / "data").mkdir(parents=True)
        (self.ws / "vanilla" / "data" / "x.txt").write_text("game file")
        (self.ws / "build").mkdir()
        (self.ws / "src").mkdir()
        (self.ws / "src" / "stale.py").write_text("deleted on main")
        zp = self.t / "src.zip"
        with zipfile.ZipFile(zp, "w") as z:
            z.writestr("DarkestDiablo-abc/", "")
            z.writestr("DarkestDiablo-abc/tools/build.py", "print('hi')")
            z.writestr("DarkestDiablo-abc/tools/vanilla_files.txt", "# comment\ndata/global/excel/\n\n  data/x  # why\n")

        def fake(url, dest, token=None, progress=lambda d, t: None):
            self.assertIn("codeload.github.com", url)
            Path(dest).write_bytes(zp.read_bytes())
        with mock.patch.object(ddcore, "download", fake):
            ddmain.fetch_source("abc", self.ws, lambda *a: None)
        self.assertTrue((self.ws / "vanilla" / "data" / "x.txt").is_file())
        self.assertTrue((self.ws / "build").is_dir())
        self.assertFalse((self.ws / "src" / "stale.py").exists())
        self.assertEqual((self.ws / "tools" / "build.py").read_text(), "print('hi')")
        self.assertEqual(ddmain.vanilla_list(self.ws), ["data/global/excel/", "data/x"])

    def test_vanilla_extracted_once_per_list_and_build(self):
        (self.game / ".build.info").write_text("Branch!STRING:0|Version!STRING:0\nus|3.4.95032\n")
        (self.ws / "tools").mkdir(parents=True)
        (self.ws / "tools" / "vanilla_files.txt").write_text("data/global/excel/\n")
        calls = []

        def run(args, **kw):
            calls.append(args)
            return mock.Mock(returncode=0, stdout="", stderr="12 files")
        with mock.patch.object(ddmain, "find_extractor", lambda: Path("extract.exe")), \
                mock.patch.object(ddmain.subprocess, "run", run):
            ddmain.ensure_vanilla(self.game, self.ws, lambda *a: None)
            ddmain.ensure_vanilla(self.game, self.ws, lambda *a: None)
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][3:], ["data/global/excel/"])
            self.assertEqual((self.ws / "vanilla" / "D2R_VERSION").read_text(), "3.4.95032")
            (self.ws / "tools" / "vanilla_files.txt").write_text("data/global/excel/\ndata/global/ui/layouts/\n")
            ddmain.ensure_vanilla(self.game, self.ws, lambda *a: None)
            self.assertEqual(len(calls), 2)

    def test_install_keeps_what_the_game_wrote(self):
        commit = {"sha": "a" * 40, "message": "first"}
        self.built({"modinfo.json": b"m", "darkestdiablo.mpq/data/a.txt": b"a1",
                    "d2rloader/plugins/darkestdiablo.dll": b"dll", "d2rloader/config/d2rloader.toml": b"cfg",
                    "d2rloader/logs/build-time.log": b"not shipped"})
        self.assertEqual(ddmain.install_build(self.game, self.ws, commit), (4, 0))
        mod = ddcore.mod_dir(self.game)
        self.assertFalse((mod / "d2rloader/logs/build-time.log").exists())
        (mod / "d2rloader/logs").mkdir(parents=True)
        (mod / "d2rloader/logs/darkestdiablo.log").write_text("the game's own log")
        (mod / "d2rloader/camp_state.txt").write_text("written while playing")

        self.built({"modinfo.json": b"m", "darkestdiablo.mpq/data/b.txt": b"b",
                    "d2rloader/plugins/darkestdiablo.dll": b"dll2", "d2rloader/config/d2rloader.toml": b"cfg"})
        changed, removed = ddmain.install_build(self.game, self.ws, {"sha": "b" * 40, "message": "second"})
        self.assertEqual((changed, removed), (2, 1))
        self.assertFalse((mod / "darkestdiablo.mpq/data/a.txt").exists())
        self.assertEqual((mod / "d2rloader/plugins/darkestdiablo.dll").read_bytes(), b"dll2")
        self.assertTrue((mod / "d2rloader/logs/darkestdiablo.log").is_file())
        self.assertTrue((mod / "d2rloader/camp_state.txt").is_file())
        rec = ddcore.read_install(self.game)
        self.assertEqual((rec["channel"], rec["commit"], rec["version"]), ("main", "b" * 40, "main bbbbbbb"))

    def test_install_needs_the_plugin(self):
        self.built({"modinfo.json": b"m"})
        with self.assertRaises(RuntimeError):
            ddmain.install_build(self.game, self.ws, {"sha": "c" * 40})


class FullPackageKeepsOtherFiles(Workspace):
    def test_full_package_does_not_wipe_the_mod_folder(self):
        self.write_build(V1)
        ddcore.update(self.game, self.make_release("0.1.0"))
        log = ddcore.mod_dir(self.game) / "d2rloader/logs/darkestdiablo.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text("kept")
        r = self.make_release_full_only("0.2.0", dict(V1, **{"modinfo.json": b"new"}))
        ddcore.update(self.game, r)
        self.assertEqual(log.read_text(), "kept")
        self.assertEqual(self.installed_files()["modinfo.json"], b"new")

    def make_release_full_only(self, version, files):
        self.write_build(files)
        self.published.clear()  # no earlier manifests, so no delta: a full package
        return self.make_release(version)


class VanillaListTests(unittest.TestCase):
    def test_fallback_matches_the_list_file(self):
        lines = [l.split("#", 1)[0].strip() for l in (ROOT / "tools" / "vanilla_files.txt").read_text().splitlines()]
        self.assertEqual([l for l in lines if l], ddmain.DEFAULT_VANILLA)


class ConsoleTests(unittest.TestCase):
    def config(self, game, text):
        path = Path(game) / "d2rloader" / "config" / "d2rloader.toml"
        path.parent.mkdir(parents=True)
        path.write_text(text, encoding="utf-8")
        return path

    def test_turns_the_console_on(self):
        with tempfile.TemporaryDirectory() as game:
            path = self.config(game, "[d2rloader.developer]\n\n# Enable it.\nenable_console = false\n\nassert_dialog = true\n")
            self.assertTrue(ddcore.enable_console(game))
            self.assertIn("enable_console = true\n", path.read_text(encoding="utf-8"))
            self.assertIn("assert_dialog = true", path.read_text(encoding="utf-8"))

    def test_adds_the_setting_when_missing(self):
        with tempfile.TemporaryDirectory() as game:
            path = self.config(game, "[d2rloader]\nmod = 1\n")
            ddcore.enable_console(game)
            self.assertTrue(path.read_text(encoding="utf-8").endswith("[d2rloader.developer]\nenable_console = true\n"))

    def test_no_config_yet(self):
        with tempfile.TemporaryDirectory() as game:
            self.assertFalse(ddcore.enable_console(game))


class ChildProgramTests(unittest.TestCase):
    """Programs the launcher starts must not use its temp unpack folder (see ddcore.clean_env)."""

    def test_clean_env_when_frozen(self):
        bundle = os.path.join(tempfile.gettempdir(), "_MEI12345")
        env = {"PATH": os.pathsep.join([bundle, os.path.join(bundle, "sub"), "/usr/bin"]),
               "_PYI_APPLICATION_HOME_DIR": bundle, "_PYI_PARENT_PROCESS_LEVEL": "1", "_MEIPASS2": bundle,
               "TCL_LIBRARY": os.path.join(bundle, "tcl"), "TK_LIBRARY": "/elsewhere/tk",
               "PYINSTALLER_RESET_ENVIRONMENT": "1", "APPDATA": "x"}
        with mock.patch.object(sys, "frozen", True, create=True), \
                mock.patch.object(sys, "_MEIPASS", bundle, create=True):
            out = ddcore.clean_env(env)
        self.assertEqual(out["PATH"], "/usr/bin")
        for k in ("_PYI_APPLICATION_HOME_DIR", "_PYI_PARENT_PROCESS_LEVEL", "_MEIPASS2", "TCL_LIBRARY"):
            self.assertNotIn(k, out)
        self.assertEqual(out["TK_LIBRARY"], "/elsewhere/tk")
        self.assertEqual(out["PYINSTALLER_RESET_ENVIRONMENT"], "1")
        self.assertEqual(out["APPDATA"], "x")

    def test_clean_env_from_source_changes_nothing(self):
        env = {"PATH": "/a", "_PYI_X": "1"}
        self.assertEqual(ddcore.clean_env(env), env)


if __name__ == "__main__":
    unittest.main()


import ddself  # noqa: E402


class SelfUpdateTests(unittest.TestCase):
    def test_newer_build(self):
        self.assertTrue(ddself.newer_build({"build": 12, "sha256": "ab"}, current=11))
        self.assertFalse(ddself.newer_build({"build": 11, "sha256": "ab"}, current=11))
        self.assertFalse(ddself.newer_build({"build": 10, "sha256": "ab"}, current=11))
        self.assertFalse(ddself.newer_build({"build": 99, "sha256": "ab"}, current=0))  # own builds never update
        self.assertFalse(ddself.newer_build({"build": 99}, current=11))  # no checksum, no update
        self.assertFalse(ddself.newer_build({"build": "x", "sha256": "ab"}, current=11))

    def test_never_from_source(self):
        self.assertFalse(ddself.can_update())
        self.assertIsNone(ddself.check())

    def test_swap_script(self):
        s = ddself.swap_script(r"C:\L\DarkestDiabloLauncher.exe.new", r"C:\L\DarkestDiabloLauncher.exe", [11, 22])
        self.assertIn('"PID eq 11"', s)
        self.assertIn('"PID eq 22"', s)
        self.assertIn(r'move /y "C:\L\DarkestDiabloLauncher.exe.new" "C:\L\DarkestDiabloLauncher.exe"', s)
        self.assertIn(r'start "" "C:\L\DarkestDiabloLauncher.exe"', s)
        self.assertLess(s.index(":wait"), s.index(":move"))
        self.assertLess(s.index("move /y"), s.index("start "))

    def test_download_checks_the_file(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "built.exe"
            src.write_bytes(b"new launcher")
            exe = Path(d) / "DarkestDiabloLauncher.exe"
            exe.write_bytes(b"old")
            good = {"url": src.as_uri(), "sha256": ddcore.sha256_file(src), "size": src.stat().st_size}
            with mock.patch.object(sys, "executable", str(exe)):
                new = ddself.download(good)
                self.assertEqual(new.read_bytes(), b"new launcher")
                new.unlink()
                with self.assertRaises(RuntimeError):
                    ddself.download(dict(good, sha256="00" * 32))
                self.assertFalse((Path(d) / "DarkestDiabloLauncher.exe.new").exists())
