#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Проверка службы на ПК: собирает src/*.cpp с -DHOST_TEST (заглушки вместо консоли),
запускает настоящий HTTP-сервер и гоняет его API на поддельных PKG.
Консольная часть (BGFT) здесь НЕ проверяется, только логика службы.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TMP = Path(tempfile.mkdtemp(prefix="ps4inst_test_"))
BIN = TMP / "ps4installerd_host"
UNIT = TMP / "test_pkg"
DATA_PKG = TMP / "data" / "pkg"
USB_PREFIX = str(TMP / "mnt" / "usb")
DATA_DIR = TMP / "svc"

DEFS = [
    '-DHOST_TEST',
    '-DDATA_DIR="%s"' % DATA_DIR,
    '-DDATA_PKG_DIR="%s"' % DATA_PKG,
    '-DUSB_PREFIX="%s"' % USB_PREFIX,
]
COMMON = ["g++", "-std=c++17", "-g", "-pthread", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-I" + str(ROOT / "src")]


def build():
    srcs = [str(p) for p in sorted((ROOT / "src").glob("*.cpp"))]
    subprocess.run(COMMON + DEFS + srcs + ["-o", str(BIN)], check=True)
    unit_srcs = [str(ROOT / "src" / n) for n in ("pkg.cpp", "json.cpp", "log.cpp")]
    subprocess.run(COMMON + DEFS + [str(ROOT / "tests" / "test_pkg.cpp")] + unit_srcs + ["-o", str(UNIT)], check=True)


def make_pkg(path, content_id, size, ptype=0x1A, flags=0):
    path.parent.mkdir(parents=True, exist_ok=True)
    b = bytearray(max(size, 0x1000))
    b[0:4] = bytes([0x7F, 0x43, 0x4E, 0x54])
    b[0x40:0x40 + len(content_id)] = content_id.encode()
    b[0x74:0x78] = ptype.to_bytes(4, "big")
    b[0x78:0x7C] = flags.to_bytes(4, "big")
    b[0x430:0x438] = len(b).to_bytes(8, "big")
    path.write_bytes(bytes(b))
    return str(path)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Server:
    def __init__(self, port=None, env=None, extra=()):
        self.port = port or free_port()
        e = dict(os.environ)
        e["ASAN_OPTIONS"] = "detect_leaks=0"
        e["PS4I_SCAN_CACHE_SEC"] = "0"   # без кэша /scan (кроме теста кэша)
        e.update(env or {})
        self.proc = subprocess.Popen([str(BIN), "--port", str(self.port), "--poll-ms", "50"] + list(extra),
                                     env=e, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                self.get("/health")
                return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError("сервер не поднялся")

    def req(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else (b"" if method == "POST" else None)
        r = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path), data=data, method=method)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, json.loads(resp.read().decode())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read().decode())

    def get(self, path):
        return self.req("GET", path)

    def post(self, path, body=None):
        return self.req("POST", path, body)

    def wait_idle(self, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            st = self.get("/status")[1]
            if st["state"] == "idle":
                return st
            time.sleep(0.05)
        raise RuntimeError("очередь не освободилась")

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.post("/quit")
            except Exception:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        err = self.proc.stderr.read().decode() if self.proc.stderr else ""
        self.proc.stderr.close()
        return err


def cid(name):
    # 36 символов: XXNNNN-CUSAnnnnn_00-XXXXXXXXXXXXXXXX
    n = (name.upper() + "0" * 16)[:16]
    return "UP0001-CUSA%05d_00-%s" % (hash(name) % 90000 + 10000, n)


class ServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(TMP, ignore_errors=True)

    def setUp(self):
        shutil.rmtree(TMP / "data", ignore_errors=True)
        shutil.rmtree(TMP / "mnt", ignore_errors=True)
        shutil.rmtree(DATA_DIR, ignore_errors=True)
        self.srv = None

    def tearDown(self):
        if self.srv:
            err = self.srv.stop()
            self.assertNotIn("runtime error", err)       # UBSan
            self.assertNotIn("AddressSanitizer", err)    # ASan

    def log_text(self):
        return (DATA_DIR / "log.txt").read_text(encoding="utf-8")

    def test_unit_pkg_and_json(self):
        r = subprocess.run([str(UNIT)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("test_pkg: ok", r.stdout)

    def test_health_and_log_created_first(self):
        self.srv = Server()
        code, j = self.srv.get("/health")
        self.assertEqual(code, 200)
        self.assertTrue(j["ok"])
        self.assertRegex(j["version"], r"^\d+\.\d+\.\d+\.\d+$")
        self.assertIn("запущен", self.log_text())

    def test_scan_sorted_and_filters(self):
        make_pkg(DATA_PKG / "small.pkg", "UP0001-CUSA00001_00-SMALL00000000000", 0x2000)
        make_pkg(DATA_PKG / "game" / "big.pkg", "UP0001-CUSA00002_00-BIG0000000000000", 0x10000)
        make_pkg(Path(USB_PREFIX + "0") / "mid.pkg", "UP0001-CUSA00003_00-MID0000000000000", 0x5000)
        make_pkg(Path(USB_PREFIX + "0") / "$RECYCLE.BIN" / "trash.pkg", "UP0001-CUSA00004_00-TRASH00000000000", 0x3000)
        (DATA_PKG / "junk.pkg").write_bytes(b"not a pkg" * 100)
        (DATA_PKG / "notes.txt").write_text("x")
        make_pkg(DATA_PKG / "patch.pkg", "UP0001-CUSA00001_00-PATCH00000000000", 0x3000, flags=0x40000000)
        self.srv = Server()
        code, j = self.srv.get("/scan")
        self.assertEqual(code, 200)
        names = [Path(p["path"]).name for p in j["packages"]]
        self.assertEqual(names, ["big.pkg", "mid.pkg", "patch.pkg", "small.pkg"])
        self.assertEqual(j["packages"][0]["title_id"], "CUSA00002")
        self.assertEqual([p["kind"] for p in j["packages"]], ["game", "game", "patch", "game"])

    def test_install_order_biggest_first(self):
        a = make_pkg(DATA_PKG / "a.pkg", "UP0001-CUSA00011_00-AAAAAAAAAAAAAAAA", 0x3000)
        b = make_pkg(DATA_PKG / "b.pkg", "UP0001-CUSA00012_00-BBBBBBBBBBBBBBBB", 0x9000)
        c = make_pkg(Path(USB_PREFIX + "1") / "c.pkg", "UP0001-CUSA00013_00-CCCCCCCCCCCCCCCC", 0x6000)
        self.srv = Server()
        code, j = self.srv.post("/install", {"paths": [a, b, c, b]})  # дубль b игнорируется
        self.assertEqual(code, 200, j)
        self.assertEqual(j["order"], [b, c, a])
        st = self.srv.wait_idle()
        self.assertEqual([x["status"] for x in st["jobs"]], ["done", "done", "done"])
        self.assertEqual([x["path"] for x in st["jobs"]], [b, c, a])
        reg = [ln for ln in self.log_text().splitlines() if "fake register" in ln]
        self.assertEqual(len(reg), 3)
        self.assertIn("BBBBBBBBBBBBBBBB", reg[0])
        self.assertIn("CCCCCCCCCCCCCCCC", reg[1])
        self.assertIn("AAAAAAAAAAAAAAAA", reg[2])

    def test_patch_after_its_game_and_same_package_once(self):
        # патч больше игры, но ставится после неё; копия игры на флешке пропускается
        game = make_pkg(DATA_PKG / "g.pkg", "UP0001-CUSA00021_00-GAMEGAMEGAMEGAME", 0x4000)
        patch = make_pkg(DATA_PKG / "p.pkg", "UP0001-CUSA00021_00-GAMEGAMEGAMEGAME", 0x8000, flags=0x00100000)
        other = make_pkg(DATA_PKG / "o.pkg", "UP0001-CUSA00022_00-OTHEROTHEROTHERO", 0x6000)
        copy = make_pkg(Path(USB_PREFIX + "0") / "g.pkg", "UP0001-CUSA00021_00-GAMEGAMEGAMEGAME", 0x4000)
        self.srv = Server()
        code, j = self.srv.post("/install", {"paths": [patch, other, game, copy]})
        self.assertEqual(code, 200, j)
        self.assertEqual(j["order"], [other, game, patch])

    def test_busy_register_is_retried(self):
        a = make_pkg(DATA_PKG / "busy.pkg", "UP0001-CUSA00031_00-BUSYBUSYBUSYBUSY", 0x3000)
        self.srv = Server()
        code, j = self.srv.post("/install", {"paths": [a]})
        self.assertEqual(code, 200, j)
        st = self.srv.wait_idle()
        self.assertEqual([x["status"] for x in st["jobs"]], ["done"])
        self.assertEqual(self.log_text().count("система занята"), 2)

    def test_backport_skipped_on_new_firmware(self):
        game = make_pkg(DATA_PKG / "Game" / "Game 1.00.pkg", "UP0001-CUSA00041_00-BPGAMEBPGAMEBPGA", 0x4000)
        bp = make_pkg(DATA_PKG / "Game" / "Game BackPort 6.72.pkg", "UP0001-CUSA00042_00-BPGAMEBPGAMEBPGB", 0x6000)
        self.srv = Server()  # прошивка по умолчанию 13.02
        code, j = self.srv.post("/install", {"paths": [game, bp]})
        self.assertEqual(code, 200, j)
        self.assertEqual(j["order"], [game])
        self.assertEqual([x["path"] for x in j["skipped"]], [bp])
        self.assertIn("backport", j["skipped"][0]["reason"])
        self.srv.wait_idle()

    def test_backport_installed_on_old_firmware(self):
        bp = make_pkg(DATA_PKG / "Game BP.pkg", "UP0001-CUSA00043_00-BPGAMEBPGAMEBPGC", 0x4000)
        self.srv = Server(env={"FAKE_FW": "9.00"})
        code, j = self.srv.post("/install", {"paths": [bp]})
        self.assertEqual(code, 200, j)
        self.assertEqual(j["order"], [bp])
        self.assertEqual(j["skipped"], [])
        self.srv.wait_idle()

    def test_survives_connection_flood(self):
        # много пустых соединений и быстрые запросы: служба не падает и потом отвечает
        self.srv = Server()
        idle = []
        for _ in range(40):
            c = socket.socket()
            c.connect(("127.0.0.1", self.srv.port))
            idle.append(c)
        for _ in range(60):
            try:
                self.srv.get("/status")
            except Exception:
                pass
        for c in idle:
            c.close()
        time.sleep(0.5)
        for _ in range(50):
            code, j = self.srv.get("/health")
            if code == 200:
                break
            time.sleep(0.2)
        self.assertEqual(code, 200)
        self.assertIsNone(self.srv.proc.poll())

    def test_bad_requests(self):
        ok = make_pkg(DATA_PKG / "ok.pkg", "UP0001-CUSA00021_00-OKOKOKOKOKOKOKOK", 0x2000)
        (DATA_PKG / "garbage.pkg").write_bytes(b"x" * 5000)
        self.srv = Server()
        s = self.srv
        self.assertEqual(s.post("/install", {"paths": []})[0], 400)
        self.assertEqual(s.post("/install", {"nope": 1})[0], 400)
        self.assertEqual(s.req("POST", "/install", None)[0], 400)
        for bad in ["/etc/passwd", str(DATA_PKG) + "/../x.pkg", str(DATA_PKG) + "/ok.txt", "relative.pkg"]:
            code, j = s.post("/install", {"paths": [ok, bad]})
            self.assertEqual((code, j["error"]), (400, "bad_path"), bad)
        code, j = s.post("/install", {"paths": [str(DATA_PKG / "garbage.pkg")]})
        self.assertEqual((code, j["error"]), (422, "bad_pkg"))
        code, j = s.post("/install", {"paths": [str(DATA_PKG / "missing.pkg")]})
        self.assertEqual((code, j["error"]), (422, "bad_pkg"))
        self.assertEqual(s.get("/install")[0], 405)
        self.assertEqual(s.get("/nope")[0], 404)
        # ничего из этого не должно попасть в очередь
        self.assertEqual(s.get("/status")[1]["jobs"], [])

    def test_no_space(self):
        p = make_pkg(DATA_PKG / "big.pkg", "UP0001-CUSA00031_00-BIGBIGBIGBIGBIGB", 0x10000)
        self.srv = Server(env={"FAKE_FREE_BYTES": "1000", "FAKE_TOTAL_BYTES": "5000"})
        code, j = self.srv.post("/install", {"paths": [p]})
        self.assertEqual((code, j["error"]), (409, "no_space"))
        self.assertEqual(j["need"], 0x10000)
        self.assertEqual(j["free"], 1000)
        self.assertEqual(self.srv.get("/free")[1]["free_bytes"], 1000)
        self.assertEqual(self.srv.get("/free")[1]["total_bytes"], 5000)
        self.assertEqual(self.srv.get("/installed")[1]["title_ids"], [])
        self.assertEqual(self.srv.get("/status")[1]["jobs"], [])

    def test_failure_does_not_stop_queue_and_already_installed(self):
        bad = make_pkg(DATA_PKG / "bad.pkg", "UP0001-CUSA00041_00-FAILFAILFAILFAIL", 0x9000)
        good = make_pkg(DATA_PKG / "good.pkg", "UP0001-CUSA00042_00-GOODGOODGOODGOOD", 0x4000)
        self.srv = Server()
        self.assertEqual(self.srv.post("/install", {"paths": [bad, good]})[0], 200)
        st = self.srv.wait_idle()
        self.assertEqual([x["status"] for x in st["jobs"]], ["failed", "done"])
        self.assertIn("0x80990001", st["jobs"][0]["message"])
        # повторная установка того же пакета - "уже установлено", а не ошибка
        code, _ = self.srv.post("/install", {"paths": [good]})
        self.assertEqual(code, 200)
        st = self.srv.wait_idle()
        self.assertEqual(st["jobs"][0]["status"], "already_installed")

    def test_stall_detected(self):
        p = make_pkg(DATA_PKG / "stall.pkg", "UP0001-CUSA00051_00-STALLSTALLSTALLS", 0x4000)
        self.srv = Server(extra=["--stall", "1"])
        self.srv.post("/install", {"paths": [p]})
        st = self.srv.wait_idle(timeout=15)
        self.assertEqual(st["jobs"][0]["status"], "failed")
        self.assertIn("не продвигается", st["jobs"][0]["message"])

    def test_busy_and_cancel(self):
        a = make_pkg(DATA_PKG / "a.pkg", "UP0001-CUSA00061_00-AAAAAAAAAAAAAAAA", 0x9000)
        b = make_pkg(DATA_PKG / "b.pkg", "UP0001-CUSA00062_00-BBBBBBBBBBBBBBBB", 0x5000)
        c = make_pkg(DATA_PKG / "c.pkg", "UP0001-CUSA00063_00-CCCCCCCCCCCCCCCC", 0x3000)
        self.srv = Server(extra=[])
        self.assertEqual(self.srv.post("/install", {"paths": [a, b, c]})[0], 200)
        # Повторный запрос тех же файлов во время установки их не дублирует.
        code, j = self.srv.post("/install", {"paths": [a]})
        self.assertEqual(code, 200)
        self.assertEqual(j["order"], [])
        self.assertEqual(len(self.srv.get("/status")[1]["jobs"]), 3)
        self.assertEqual(self.srv.post("/cancel")[0], 200)
        st = self.srv.wait_idle()
        statuses = [x["status"] for x in st["jobs"]]
        self.assertEqual(statuses[0], "done")
        self.assertIn("cancelled", statuses[1:])

    def test_torrent_preallocated_file_is_incomplete(self):
        # полный размер, но куски не пришли: внутри нули (как у недокачанного торрента)
        p = make_pkg(DATA_PKG / "torrent.pkg", "UP0001-CUSA00091_00-TORRENTTORRENTTO", 0x1000000)
        self.srv = Server(extra=[])
        item = [x for x in self.srv.get("/scan")[1]["packages"] if x["path"] == p][0]
        self.assertTrue(item["incomplete"])
        self.assertGreaterEqual(item["zero_blocks"], 6)
        code, j = self.srv.post("/install", {"paths": [p]})
        self.assertEqual(j["skipped"][0]["reason"], "incomplete")

    def test_complete_file_is_not_flagged(self):
        p = make_pkg(DATA_PKG / "full.pkg", "UP0001-CUSA00092_00-FULLFULLFULLFULL", 0x1000000)
        data = bytearray(open(p, "rb").read())
        for i in range(0x1000, len(data), 7):
            data[i] = 1 + (i % 250)
        open(p, "wb").write(bytes(data))
        self.srv = Server(extra=[])
        item = [x for x in self.srv.get("/scan")[1]["packages"] if x["path"] == p][0]
        self.assertFalse(item["incomplete"])

    def test_icon_from_pkg(self):
        p = make_pkg(DATA_PKG / "icon.pkg", "UP0001-CUSA00093_00-ICONICONICONICON", 0x6000)
        data = bytearray(open(p, "rb").read())
        png = b"\x89PNG\r\n\x1a\n" + bytes(range(64))
        data[0x10:0x14] = (1).to_bytes(4, "big")          # записей в таблице
        data[0x18:0x1C] = (0x2000).to_bytes(4, "big")     # смещение таблицы
        data[0x2000:0x2004] = (0x1200).to_bytes(4, "big")  # ICON0_PNG
        data[0x2010:0x2014] = (0x3000).to_bytes(4, "big")
        data[0x2014:0x2018] = len(png).to_bytes(4, "big")
        data[0x3000:0x3000 + len(png)] = png
        open(p, "wb").write(bytes(data))
        self.srv = Server(extra=[])
        import urllib.parse
        with urllib.request.urlopen("http://127.0.0.1:%d/icon?path=%s" % (self.srv.port, urllib.parse.quote(p))) as r:
            self.assertEqual(r.headers["Content-Type"], "image/png")
            self.assertEqual(r.read(), png)
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen("http://127.0.0.1:%d/icon?path=%s" % (self.srv.port, urllib.parse.quote("/etc/passwd")))
        self.assertEqual(cm.exception.code, 404)

    def test_installed_games_icon_and_launch(self):
        import tempfile
        meta = tempfile.mkdtemp()
        os.makedirs(meta + "/CUSA00001")
        png = b"\x89PNG\r\n\x1a\n" + bytes(range(32))
        open(meta + "/CUSA00001/icon0.png", "wb").write(png)
        log = meta + "/launch.log"
        self.srv = Server(env={"FAKE_INSTALLED": "CUSA00001:Game One,CUSA00002", "FAKE_APPMETA": meta,
                               "FAKE_LAUNCH_LOG": log, "FAKE_INSTALLED_EXT": "CUSA00002"})
        code, j = self.srv.get("/installed_games")
        self.assertEqual(code, 200)
        self.assertEqual(j["games"], [{"id": "CUSA00001", "title": "Game One", "loc": "HDD"}, {"id": "CUSA00002", "title": "", "loc": "EXT"}])
        self.assertEqual(self.srv.get("/installed")[1]["title_ids"], ["CUSA00001", "CUSA00002"])
        with urllib.request.urlopen("http://127.0.0.1:%d/appicon?id=CUSA00001" % self.srv.port) as r:
            self.assertEqual(r.read(), png)
        for bad in ("CUSA00002", "../../etc/passwd"):
            with self.assertRaises(urllib.error.HTTPError) as cm:
                urllib.request.urlopen("http://127.0.0.1:%d/appicon?id=%s" % (self.srv.port, bad))
            self.assertEqual(cm.exception.code, 404)
        self.assertEqual(self.srv.post("/launch", {"title_ids": ["CUSA00001"]})[0], 200)
        self.assertEqual(open(log).read().split(), ["CUSA00001"])
        self.assertEqual(self.srv.post("/launch", {"title_ids": ["CUSA99999"]})[0], 404)   # не установлена
        self.assertEqual(self.srv.post("/launch", {"title_ids": []})[0], 400)
        self.assertEqual(self.srv.get("/launch")[0], 405)

    def test_uninstall(self):
        import tempfile
        d = tempfile.mkdtemp()
        log = d + "/un.log"
        self.srv = Server(env={"FAKE_INSTALLED": "CUSA00001:A,CUSA00003FAIL", "FAKE_UNINSTALL_LOG": log})
        self.assertEqual(self.srv.post("/uninstall", {"title_ids": ["CUSA00001"]})[0], 200)
        self.assertEqual(open(log).read().split(), ["CUSA00001"])
        self.assertEqual(self.srv.post("/uninstall", {"title_ids": ["CUSA99999"]})[0], 404)   # не установлена
        self.assertEqual(self.srv.post("/uninstall", {"title_ids": ["CUSA00003FAIL"]})[0], 500)
        self.assertEqual(self.srv.post("/uninstall", {"title_ids": ["A", "B"]})[0], 400)
        self.assertEqual(self.srv.post("/uninstall", {"title_ids": ["OFST00001"]})[0], 403)   # себя удалять нельзя
        self.assertEqual(self.srv.get("/uninstall")[0], 405)

    def test_delete_files(self):
        a = make_pkg(DATA_PKG / "da.pkg", "UP0001-CUSA00201_00-DELETEDELETEDEL1", 0x2000)
        b = make_pkg(Path(USB_PREFIX + "0") / "db.pkg", "UP0001-CUSA00202_00-DELETEDELETEDEL2", 0x2000)
        keep = make_pkg(DATA_PKG / "keep.pkg", "UP0001-CUSA00203_00-DELETEDELETEDEL3", 0x2000)
        self.srv = Server()
        self.assertEqual(self.srv.post("/delete_files", {"paths": ["/etc/passwd.pkg"]})[0], 403)   # чужая папка
        self.assertEqual(self.srv.post("/delete_files", {"paths": [a, "/data/pkg/../x.pkg"]})[0], 403)
        self.assertTrue(os.path.exists(a))   # при отказе не удаляется ничего
        self.assertEqual(self.srv.post("/delete_files", {"paths": []})[0], 400)
        self.assertEqual(self.srv.get("/delete_files")[0], 405)
        self.assertEqual(self.srv.get("/scan")[1]["count"], 3)
        code, j = self.srv.post("/delete_files", {"paths": [a, b]})
        self.assertEqual(code, 200, j)
        self.assertEqual(j["deleted"], 2)
        self.assertFalse(os.path.exists(a) or os.path.exists(b))
        self.assertTrue(os.path.exists(keep))
        self.assertEqual(self.srv.get("/scan?fresh=1")[1]["count"], 1)
        code, j = self.srv.post("/delete_files", {"paths": [a]})   # уже нет файла
        self.assertFalse(j["ok"])

    def test_usb_and_write_magnet(self):
        usb1 = Path(USB_PREFIX + "1")
        shutil.rmtree(usb1, ignore_errors=True)
        usb1.mkdir(parents=True, exist_ok=True)
        self.srv = Server()
        self.assertEqual(self.srv.get("/usb")[1]["roots"].count(USB_PREFIX + "1"), 0)   # пустая точка монтирования без накопителя - не флешка
        (usb1 / "marker").write_text("x")
        code, j = self.srv.get("/usb")
        self.assertEqual(code, 200)
        self.assertIn(USB_PREFIX + "1", j["roots"])
        magnet = "magnet:?xt=urn:btih:" + "a" * 40 + "&dn=Game%20One"
        ok = {"args": [USB_PREFIX + "1", "Game_One_CUSA00001_magnet.txt", magnet]}
        code, j = self.srv.post("/write_magnet", ok)
        self.assertEqual(code, 200, j)
        self.assertEqual((usb1 / "Game_One_CUSA00001_magnet.txt").read_text(), magnet)
        self.assertEqual(self.srv.get("/write_magnet")[0], 405)
        for bad in (["/etc", "x_magnet.txt", magnet], [USB_PREFIX + "1", "../x_magnet.txt", magnet], [USB_PREFIX + "1", "a/b_magnet.txt", magnet],
                    [USB_PREFIX + "1", "evil.sh", magnet], [USB_PREFIX + "1", "x_magnet.txt", "rm -rf /"], [USB_PREFIX + "9", "x_magnet.txt", magnet]):
            self.assertEqual(self.srv.post("/write_magnet", {"args": bad})[0], 400, bad)
        self.assertEqual(self.srv.post("/write_magnet", {"args": ["a"]})[0], 400)
        shutil.rmtree(usb1, ignore_errors=True)

    def test_scan_is_cached(self):
        a = make_pkg(DATA_PKG / "ca.pkg", "UP0001-CUSA00101_00-CACHECACHECACHE1", 0x2000)
        self.srv = Server(env={"PS4I_SCAN_CACHE_SEC": "30"})
        n1 = self.srv.get("/scan")[1]["count"]
        make_pkg(DATA_PKG / "cb.pkg", "UP0001-CUSA00102_00-CACHECACHECACHE2", 0x2000)
        self.assertEqual(self.srv.get("/scan")[1]["count"], n1)   # из кэша
        time.sleep(3.2)
        self.assertEqual(self.srv.get("/scan?fresh=1")[1]["count"], n1 + 1)   # принудительное обновление

    def test_queue_appends_second_batch(self):
        a = make_pkg(DATA_PKG / "qa.pkg", "UP0001-CUSA00071_00-QAQAQAQAQAQAQAQA", 0x9000)
        b = make_pkg(DATA_PKG / "qb.pkg", "UP0001-CUSA00072_00-QBQBQBQBQBQBQBQB", 0x5000)
        self.srv = Server(extra=[])
        self.assertEqual(self.srv.post("/install", {"paths": [a]})[0], 200)
        code, j = self.srv.post("/install", {"paths": [b]})
        self.assertEqual(code, 200)
        self.assertEqual(j["order"], [b])
        st = self.srv.wait_idle()
        self.assertEqual([x["status"] for x in st["jobs"]], ["done", "done"])

    def test_second_instance_replaces_first(self):
        port = free_port()
        first = Server(port=port)
        second = Server(port=port)
        self.srv = second
        try:
            first.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.fail("старая копия не завершилась")
        # новая копия привязывается к порту с повторами, пока старая закрывается
        for _ in range(60):
            try:
                self.assertEqual(second.get("/health")[0], 200)
                break
            except urllib.error.URLError:
                time.sleep(0.1)
        else:
            self.fail("новая копия не начала отвечать")
        first.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
