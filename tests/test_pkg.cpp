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

    auto h = make_header("UP0006-CUSA15080_00-STARWARSQUADRONS", 0x1A, 0);
    assert(pkg_parse_header(h.data(), h.size(), &info, &err));
    assert(info.title_id == "CUSA15080" && info.kind == "game" && !info.is_patch);

    h = make_header("UP0006-CUSA15080_00-STARWARSQUADRONS", 0x1A, 0x40000000);
    assert(pkg_parse_header(h.data(), h.size(), &info, &err) && info.kind == "patch" && info.is_patch);
    h = make_header("UP0006-CUSA15080_00-STARWARSQUADRONS", 0x1B, 0);
    assert(pkg_parse_header(h.data(), h.size(), &info, &err) && info.kind == "dlc");
    h = make_header("UP0006-CUSA15080_00-STARWARSQUADRONS", 0x1E, 0);
    assert(pkg_parse_header(h.data(), h.size(), &info, &err) && info.kind == "delta_patch");
    h = make_header("UP0006-CUSA15080_00-STARWARSQUADRONS", 0x55, 0);
    assert(!pkg_parse_header(h.data(), h.size(), &info, &err));          // неизвестный тип
    h = make_header("bad content id", 0x1A, 0);
    assert(!pkg_parse_header(h.data(), h.size(), &info, &err));          // плохой content id
    h = make_header("UP0006-CUSA15080_00-STARWARSQUADRONS", 0x1A, 0);
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
    return 0;
}
