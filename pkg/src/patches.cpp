// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#include "patches.h"
#include "https.h"
#include "json.h"
#include "log.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static std::string g_url = "https://github.com/sergiopoverony/ps4catalog/releases/latest/download/";
static std::string g_dir = "/data/ps4installer_app/catalog";
static std::string g_baseFile;

void patches_configure(const std::string& baseUrl, const std::string& dir, const std::string& baseCatalogFile)
{
    g_url = baseUrl; g_dir = dir; g_baseFile = baseCatalogFile;
}

// Имя файла из index.json может быть только catalog_<число>.json / .pak: ничего, что вело бы в другую папку.
static bool name_ok(const std::string& n)
{
    if (n.compare(0, 8, "catalog_") != 0) return false;
    size_t i = 8, d = 0;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') { i++; d++; }
    if (d == 0 || d > 3) return false;
    std::string ext = n.substr(i);
    return ext == ".json" || ext == ".pak";
}

static bool write_file(const std::string& path, const std::string& data)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    return ok;
}

PatchCheck patches_check_online(int lastApplied, std::string& info, int* downloaded)
{
    if (downloaded) *downloaded = 0;
    if (g_url.empty()) { info = "адрес заплаток не задан"; return PC_NOT_CONFIGURED; }
    if (lastApplied < 1) lastApplied = 1;   // заплатки нумеруются с 2: основной каталог - это «первая»
    std::string body, err;
    if (!https_get(g_url + "index.json", body, 1 << 20, err)) { info = "index.json не получен: " + err; return PC_OFFLINE; }
    JsonValue idx;
    if (!json_parse(body, idx) || idx["patches"].type != JsonValue::ARR) { info = "index.json повреждён"; return PC_ERROR; }
    int latest = (int)idx["patch"].asInt();
    if (latest <= lastApplied) { info = "новых заплаток нет (применено " + std::to_string(lastApplied) + ")"; return PC_UP_TO_DATE; }

    std::string wantBase = idx["base"].asStr();   // отпечаток основного каталога, для которого сделаны заплатки (пусто - не проверяем)
    if (!wantBase.empty() && !g_baseFile.empty()) {
        std::string have = sha256_file(g_baseFile);
        if (have != wantBase) { info = "заплатки сделаны для другого основного каталога - пропускаем"; return PC_ERROR; }
    }

    mkdir(g_dir.c_str(), 0777);
    int got = 0;
    const JsonValue& list = idx["patches"];
    for (size_t i = 0; i < list.arr.size(); i++) {
        const JsonValue& p = list.arr[i];
        int n = (int)p["n"].asInt();
        if (n <= lastApplied) continue;
        if (n != lastApplied + 1 + got) { info = "в index.json пропущена заплатка " + std::to_string(lastApplied + 1 + got); return PC_ERROR; }
        const JsonValue& files = p["files"];
        std::vector<std::string> tmps, finals;
        bool hasJson = false;
        for (size_t k = 0; k < files.arr.size(); k++) {
            std::string name = files.arr[k]["name"].asStr(), sum = files.arr[k]["sha256"].asStr();
            long long size = files.arr[k]["size"].asInt(-1);
            if (!name_ok(name) || sum.size() != 64) { info = "неверная запись в index.json: " + name; return PC_ERROR; }
            std::string data;
            if (!https_get(g_url + name, data, (size_t)200 << 20, err)) { info = name + ": " + err; for (size_t t = 0; t < tmps.size(); t++) unlink(tmps[t].c_str()); return PC_OFFLINE; }
            Sha256 h; h.update(data.data(), data.size());
            if ((size >= 0 && (long long)data.size() != size) || h.hex() != sum) {
                info = name + ": не сошлись размер или SHA-256";
                for (size_t t = 0; t < tmps.size(); t++) unlink(tmps[t].c_str());
                return PC_ERROR;
            }
            std::string tmp = g_dir + "/" + name + ".tmp";
            if (!write_file(tmp, data)) { info = name + ": не удалось записать"; unlink(tmp.c_str()); return PC_ERROR; }
            tmps.push_back(tmp); finals.push_back(g_dir + "/" + name);
            if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) hasJson = true;
        }
        if (!hasJson) { info = "в заплатке " + std::to_string(n) + " нет json"; for (size_t t = 0; t < tmps.size(); t++) unlink(tmps[t].c_str()); return PC_ERROR; }
        // сначала .pak, json последним: заплатка считается поставленной, когда лежит её json
        for (int pass = 0; pass < 2; pass++)
            for (size_t t = 0; t < finals.size(); t++) {
                bool isJson = finals[t].size() > 5 && finals[t].compare(finals[t].size() - 5, 5, ".json") == 0;
                if (isJson != (pass == 1)) continue;
                if (rename(tmps[t].c_str(), finals[t].c_str()) != 0) { info = "не удалось переименовать " + finals[t]; return PC_ERROR; }
            }
        logf_("patches: заплатка %d скачана и сохранена", n);
        got++;
    }
    if (downloaded) *downloaded = got;
    info = "скачано заплаток: " + std::to_string(got) + " (будут применены при следующем запуске)";
    return got > 0 ? PC_DOWNLOADED : PC_UP_TO_DATE;
}
