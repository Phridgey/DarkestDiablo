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


if __name__ == "__main__":
    unittest.main()
