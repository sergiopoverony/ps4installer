// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
// Юнит-тесты разбора PKG, путей и JSON (без консоли). Сборка и запуск: tests/test_service.py
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "json.h"
#include "pkg.h"

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static std::vector<uint8_t> make_header(const char* cid, uint32_t type, uint32_t flags) {
    std::vector<uint8_t> b(0x1000, 0);
    put_be32(&b[0], 0x7F434E54);
    memcpy(&b[0x40], cid, strlen(cid));
    put_be32(&b[0x74], type);
    put_be32(&b[0x78], flags);
    return b;
}

int main() {
    PkgInfo info;
    std::string err;

    auto h = make_header("UP0000-CUSA00000_00-EXAMPLEGAME00000", 0x1A, 0);
    assert(pkg_parse_header(h.data(), h.size(), &info, &err));
    assert(info.title_id == "CUSA00000" && info.kind == "game" && !info.is_patch);

    h = make_header("UP0000-CUSA00000_00-EXAMPLEGAME00000", 0x1A, 0x40000000);
    assert(pkg_parse_header(h.data(), h.size(), &info, &err) && info.kind == "patch" && info.is_patch);
    h = make_header("UP0000-CUSA00000_00-EXAMPLEGAME00000", 0x1B, 0);
    assert(pkg_parse_header(h.data(), h.size(), &info, &err) && info.kind == "dlc");
    h = make_header("UP0000-CUSA00000_00-EXAMPLEGAME00000", 0x1E, 0);
    assert(pkg_parse_header(h.data(), h.size(), &info, &err) && info.kind == "delta_patch");
    h = make_header("UP0000-CUSA00000_00-EXAMPLEGAME00000", 0x55, 0);
    assert(!pkg_parse_header(h.data(), h.size(), &info, &err));          // неизвестный тип
    h = make_header("bad content id", 0x1A, 0);
    assert(!pkg_parse_header(h.data(), h.size(), &info, &err));          // плохой content id
    h = make_header("UP0000-CUSA00000_00-EXAMPLEGAME00000", 0x1A, 0);
    h[0] = 0;
    assert(!pkg_parse_header(h.data(), h.size(), &info, &err));          // нет сигнатуры
    assert(!pkg_parse_header(h.data(), 100, &info, &err));               // слишком коротко

    // пути (в тестовой сборке DATA_PKG_DIR и USB_PREFIX заданы макросами)
    assert(path_allowed(DATA_PKG_DIR "/a.pkg"));
    assert(path_allowed(DATA_PKG_DIR "/dir/b.PKG"));
    assert(!path_allowed(DATA_PKG_DIR "/a.txt"));
    assert(!path_allowed(DATA_PKG_DIR "/../etc/a.pkg"));
    assert(!path_allowed(DATA_PKG_DIR "/dir/../../a.pkg"));
    assert(!path_allowed("/etc/passwd.pkg"));
    assert(!path_allowed(DATA_PKG_DIR "x/a.pkg"));
    assert(path_allowed(USB_PREFIX "0/g/a.pkg"));
    assert(path_allowed(USB_PREFIX "12/a.pkg"));
    assert(!path_allowed(USB_PREFIX "/a.pkg"));       // нет номера
    assert(!path_allowed(USB_PREFIX "x/a.pkg"));
    assert(!path_allowed(""));
    assert(path_allowed(DATA_PKG_DIR "/..a/ok.pkg")); // "..a" не считается обходом

    assert(bgft_path("/data/pkg/a.pkg") == "/user/data/pkg/a.pkg");
    assert(bgft_path("/mnt/usb0/a.pkg") == "/mnt/usb0/a.pkg");

    // JSON
    std::vector<std::string> v;
    assert(json_get_string_array("{\"paths\":[\"/a\", \"/b\\\"c\",\"\\u041f\"]}", "paths", &v));
    assert(v.size() == 3 && v[0] == "/a" && v[1] == "/b\"c" && v[2] == "\xD0\x9F");
    assert(json_get_string_array("{ \"paths\" : [ ] }", "paths", &v) && v.empty());
    assert(!json_get_string_array("{\"x\":1}", "paths", &v));
    assert(!json_get_string_array("{\"paths\":[\"unterminated", "paths", &v));
    assert(!json_get_string_array("{\"paths\":[1,2]}", "paths", &v));
    assert(json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\n");

    printf("test_pkg: ok\n");

    // --- признаки по имени
    {
        PkgInfo t;
        t.path = "/data/pkg/Game/Game [BP] 1.00.pkg";
        pkg_apply_name_tags(&t);
        assert(t.backport && !t.rus && !t.dlc_name);
        t.path = "/data/pkg/Game/Game Back Port.pkg";
        pkg_apply_name_tags(&t);
        assert(t.backport);
        t.path = "/data/pkg/Game/Game_BackPort_6.72.pkg";
        pkg_apply_name_tags(&t);
        assert(t.backport);
        t.path = "/data/pkg/Game/Game-RUS-patch.pkg";
        pkg_apply_name_tags(&t);
        assert(t.rus && !t.backport);
        t.path = "/data/pkg/Game/Game Russian.pkg";
        pkg_apply_name_tags(&t);
        assert(t.rus);
        t.path = "/data/pkg/Игра/Русификатор.pkg";
        pkg_apply_name_tags(&t);
        assert(t.rus);
        t.path = "/data/pkg/Hollow Knight/Hollow Knight 1.00 Game.pkg";  // "Knight", "Hollow" не должны сработать
        pkg_apply_name_tags(&t);
        assert(!t.backport && !t.rus && !t.dlc_name);
        t.path = "/data/pkg/Game DLC/Pack1.pkg";  // DLC в имени папки
        pkg_apply_name_tags(&t);
        assert(t.dlc_name);
        t.path = "/data/pkg/Bpm/Prussia.pkg";  // слова только целиком
        pkg_apply_name_tags(&t);
        assert(!t.backport && !t.rus);
    }

    // --- план установки
    {
        auto mk = [](const char* name, const char* title, const char* kind, bool patch, uint64_t size, int ver) {
            PkgInfo p;
            p.path = std::string("/data/pkg/") + name + ".pkg";
            p.title_id = title;
            p.kind = kind;
            p.is_patch = patch;
            p.file_size = size;
            p.ver_num = ver;
            pkg_apply_name_tags(&p);
            return p;
        };
        std::vector<PkgInfo> in = {
            mk("g RUS", "CUSA1", "patch", true, 90, 100),
            mk("g dlc1", "CUSA1", "dlc", false, 50, -1),
            mk("g 1.02", "CUSA1", "patch", true, 80, 102),
            mk("g 1.01", "CUSA1", "patch", true, 99, 101),
            mk("g", "CUSA1", "game", false, 10, 100),
            mk("g BP", "CUSA1", "game", false, 500, 100),
            mk("other", "CUSA2", "game", false, 20, 100),
        };
        std::vector<SkippedPkg> sk;
        auto out = plan_install(in, 13, &sk);
        std::vector<std::string> names;
        for (auto& p : out) names.push_back(p.path.substr(10));
        // other (20) больше g (10): сначала other, потом g с цепочкой: патч 1.02, dlc, русификатор
        assert((names == std::vector<std::string>{"other.pkg", "g.pkg", "g 1.02.pkg", "g dlc1.pkg", "g RUS.pkg"}));
        assert(sk.size() == 2);  // backport (прошивка 13) и старый патч 1.01
        sk.clear();
        out = plan_install(in, 9, &sk);  // прошивка 9: backport ставится, он самый большой
        assert(out[0].path == "/data/pkg/g BP.pkg");
        sk.clear();
        out = plan_install(in, -1, &sk);  // неизвестная прошивка = как новая
        assert(sk.size() == 2);
    }

    // --- реальные имена: игра, Fix, русификатор (в заголовке записан как "game") и дополнение
    {
        auto mk = [](const char* name, const char* kind, bool patch, uint64_t size) {
            PkgInfo p;
            p.path = std::string("/data/pkg/") + name;
            p.title_id = "CUSA33170";
            p.kind = kind;
            p.is_patch = patch;
            p.file_size = size;
            pkg_apply_name_tags(&p);
            return p;
        };
        std::vector<PkgInfo> in = {
            mk("Warhammer 40,000_ Boltgun (CUSA33170) - 1.07 Rus_V_Cool-Games.pkg", "game", false, 900),
            mk("Warhammer.40.000.Boltgun_CUSA33170_Forges.of.Corruption.Expansion.pkg", "dlc", false, 300),
            mk("Warhammer 40,000_ Boltgun (CUSA33170) - 1.07 Fix Game Info.pkg", "patch", true, 20),
            mk("Warhammer 40,000_ Boltgun (CUSA33170) - 1.00 Game.pkg", "game", false, 5000),
        };
        assert(in[0].rus && in[1].dlc_name && !in[2].rus && !in[3].rus && !in[3].backport);
        std::vector<SkippedPkg> sk;
        auto out = plan_install(in, 13, &sk);
        assert(sk.empty() && out.size() == 4);
        assert(out[0].path.find("1.00 Game") != std::string::npos);
        assert(out[1].path.find("Fix Game Info") != std::string::npos);
        assert(out[2].path.find("Expansion") != std::string::npos);
        assert(out[3].path.find("Rus_V_Cool") != std::string::npos);
    }

    // --- param.sfo внутри PKG читается из файла
    {
        const char* path = "/tmp/ps4inst_sfo_test.pkg";
        std::vector<uint8_t> f(0x3000, 0);
        auto h2 = make_header("UP0000-CUSA00000_00-EXAMPLEGAME00000", 0x1A, 0x40000000);
        memcpy(f.data(), h2.data(), h2.size());
        put_be32(&f[0x10], 1);       // записей в таблице
        put_be32(&f[0x18], 0x2000);  // смещение таблицы
        // запись param.sfo
        put_be32(&f[0x2000], 0x1000);
        put_be32(&f[0x2000 + 16], 0x2400);
        put_be32(&f[0x2000 + 20], 0x80);
        // PSF: ключи TITLE, APP_VER
        uint8_t* sfo = &f[0x2400];
        sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
        auto le32w = [](uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; };
        le32w(sfo + 8, 0x40);   // key table
        le32w(sfo + 12, 0x50);  // data table
        le32w(sfo + 16, 2);
        // индекс 0: TITLE
        sfo[20] = 0; sfo[22] = 0x04; sfo[23] = 0x02; le32w(sfo + 24, 6); le32w(sfo + 28, 16); le32w(sfo + 32, 0);
        // индекс 1: APP_VER
        sfo[36] = 6; sfo[38] = 0x04; sfo[39] = 0x02; le32w(sfo + 40, 6); le32w(sfo + 44, 8); le32w(sfo + 48, 16);
        memcpy(sfo + 0x40, "TITLE\0APP_VER\0", 15);
        memcpy(sfo + 0x50, "Hello\0", 6);
        memcpy(sfo + 0x60, "01.04\0", 6);
        FILE* fp = fopen(path, "wb");
        fwrite(f.data(), 1, f.size(), fp);
        fclose(fp);
        PkgInfo r;
        assert(pkg_read_header(path, &r, &err));
        assert(r.title == "Hello" && r.app_ver == "01.04" && r.ver_num == 104);
        remove(path);
    }
    return 0;
}
