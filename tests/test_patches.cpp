// Загрузка заплаток: адрес и папка передаются аргументами. Сервер - python (tests/test_patches.py).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include "patches.h"
#include "sha256.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

int main(int argc, char** argv)
{
    if (argc < 5) return 2;
    std::string url = argv[1], dir = argv[2], base = argv[3], mode = argv[4];
    std::string info;
    int got = -1;
    // SHA-256 известного значения
    { Sha256 s; s.update("abc", 3); CHECK(s.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }
    patches_configure("", dir, base);
    CHECK(patches_check_online(0, info, &got) == PC_NOT_CONFIGURED);
    patches_configure(url, dir, base);
    if (mode == "ok") {
        CHECK(patches_check_online(0, info, &got) == PC_DOWNLOADED && got == 2);
        CHECK(exists(dir + "/catalog_2.json") && exists(dir + "/catalog_2.pak") && exists(dir + "/catalog_3.json"));
        CHECK(!exists(dir + "/catalog_2.json.tmp"));
        CHECK(patches_check_online(3, info, &got) == PC_UP_TO_DATE && got == 0);
        // уже применена вторая - качаем только третью
        unlink((dir + "/catalog_3.json").c_str());
        CHECK(patches_check_online(2, info, &got) == PC_DOWNLOADED && got == 1);
    } else if (mode == "badsum") {
        CHECK(patches_check_online(0, info, &got) == PC_ERROR);
        CHECK(!exists(dir + "/catalog_2.json") && !exists(dir + "/catalog_2.pak") && !exists(dir + "/catalog_2.pak.tmp"));
    } else if (mode == "otherbase") {
        CHECK(patches_check_online(0, info, &got) == PC_ERROR && !exists(dir + "/catalog_2.json"));
    } else if (mode == "badname") {
        CHECK(patches_check_online(0, info, &got) == PC_ERROR);
    } else if (mode == "down") {
        CHECK(patches_check_online(0, info, &got) == PC_OFFLINE);
    }
    printf("ok %s\n", info.c_str());
    return 0;
}
