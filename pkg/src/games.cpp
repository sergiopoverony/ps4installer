// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#include "games.h"
#include "i18n.h"
#include "utf8.h"

#include <stdio.h>
#include <algorithm>
#include <map>

static std::string lower_ascii(std::string s)
{
    for (size_t i = 0; i < s.size(); i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
    return s;
}

// Собирает плитки: одна на игру (по title_id). Патчи, дополнения и русификаторы считаются
// и показываются как пометки на плитке. Файлы без самой игры (только патч) плиток не дают.
// Копии одного и того же файла (на диске и на флешке) учитываются один раз.
void build_games(const std::vector<PkgItem>& items, std::vector<Game>& out)
{
    std::map<std::string, Game> by;
    std::vector<std::string> seen;
    for (size_t i = 0; i < items.size(); i++) {
        const PkgItem& it = items[i];
        std::string key = it.content_id + "|" + it.kind + "|" + std::to_string((unsigned long long)it.size);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        bool extra = it.kind != "game" || it.patch;
        Game& g = by[it.title_id];
        if (g.title_id.empty()) g.title_id = it.title_id;
        g.files.push_back(it);
        if (extra) {
            if (it.rus) g.rus++;
            else if (it.kind == "dlc" || it.kind == "dlc_nodata" || it.dlc_name) g.dlcs++;
            else g.patches++;
        } else if (it.rus && !g.path.empty()) {
            g.rus++;
        } else if (g.path.empty() || it.size > g.size) {
            g.path = it.path;
            g.size = it.size;
            g.title = it.title.empty() ? it.title_id : it.title;
        }
    }
    out.clear();
    for (std::map<std::string, Game>::iterator m = by.begin(); m != by.end(); ++m)
        if (!m->second.path.empty()) out.push_back(m->second);
    std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) {
        return lower_ascii(a.title) < lower_ascii(b.title);
    });
}

// Режет строку на строки не длиннее maxc символов (по пробелам, иначе жёстко), последнюю обрезает «…».
std::vector<std::string> wrap_lines(const std::string& s, int maxc, int maxLines)
{
    std::vector<std::string> out;
    std::vector<std::string> words;
    {
        std::string cur;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
            else cur += s[i];
        }
        if (!cur.empty()) words.push_back(cur);
    }
    std::string line;
    for (size_t w = 0; w < words.size(); w++) {
        std::string cand = line.empty() ? words[w] : line + " " + words[w];
        if (count_cp(cand) <= maxc) { line = cand; continue; }
        if (!line.empty()) { out.push_back(line); line.clear(); }
        std::string rest = words[w];
        while (count_cp(rest) > maxc) {  // слово длиннее строки: рубим по символам
            const unsigned char* p = (const unsigned char*)rest.c_str();
            for (int k = 0; k < maxc; k++) decode_utf8(p);
            out.push_back(rest.substr(0, (size_t)(p - (const unsigned char*)rest.c_str())));
            rest = std::string((const char*)p);
        }
        line = rest;
    }
    if (!line.empty()) out.push_back(line);
    if ((int)out.size() > maxLines) {
        out.resize((size_t)maxLines);
        std::string& last = out[(size_t)maxLines - 1];
        const unsigned char* p = (const unsigned char*)last.c_str();
        int n = std::min(count_cp(last), maxc - 1);
        for (int k = 0; k < n; k++) decode_utf8(p);
        last = std::string((const char*)last.c_str(), (size_t)(p - (const unsigned char*)last.c_str())) + "\xE2\x80\xA6";
    }
    return out;
}

std::string fmt_size(uint64_t b)
{
    char buf[48];
    double gb = (double)b / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) snprintf(buf, sizeof(buf), "%.1f %s", gb, tr("unit.gb").c_str());
    else snprintf(buf, sizeof(buf), "%.0f %s", (double)b / (1024.0 * 1024.0), tr("unit.mb").c_str());
    return buf;
}


int file_rank(const PkgItem& it)
{
    bool extra = it.kind != "game" || it.patch;
    if (it.rus && extra) return 3;
    if (it.kind == "dlc" || it.kind == "dlc_nodata" || it.dlc_name) return 2;
    if (extra) return 1;
    return 0;
}

std::string file_label(const PkgItem& it)
{
    switch (file_rank(it)) {
        case 0: return tr("file.main");
        case 1: return it.ver.empty() ? tr("file.patch") : trf("file.patch_ver", it.ver);
        case 2: return it.title.empty() ? tr("file.dlc") : trf("file.dlc_name", it.title);
        default: return tr("file.rus");
    }
}

std::vector<PkgItem> install_files(const Game& g, int fw_major)
{
    std::vector<PkgItem> out;
    for (size_t i = 0; i < g.files.size(); i++) {
        if (g.files[i].backport && (fw_major < 0 || fw_major > 9)) continue;
        out.push_back(g.files[i]);
    }
    std::stable_sort(out.begin(), out.end(), [](const PkgItem& a, const PkgItem& b) {
        int ra = file_rank(a), rb = file_rank(b);
        if (ra != rb) return ra < rb;
        return a.size > b.size;
    });
    return out;
}

uint64_t install_bytes(const Game& g, int fw_major)
{
    uint64_t n = 0;
    std::vector<PkgItem> f = install_files(g, fw_major);
    for (size_t i = 0; i < f.size(); i++) n += f[i].size;
    return n;
}
