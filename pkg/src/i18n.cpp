#include "i18n.h"

#include <stdio.h>
#include <map>

static std::map<std::string, std::string> g_texts;

// Читает файл «ключ=текст». Пустые строки и строки с # пропускаются; \n в тексте - перенос строки.
static void load_file(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    fclose(f);
    size_t i = 0;
    while (i < all.size()) {
        size_t e = all.find('\n', i);
        if (e == std::string::npos) e = all.size();
        std::string line = all.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string v = line.substr(eq + 1), out;
        for (size_t k = 0; k < v.size(); k++) {
            if (v[k] == '\\' && k + 1 < v.size() && v[k + 1] == 'n') { out += '\n'; k++; }
            else out += v[k];
        }
        g_texts[line.substr(0, eq)] = out;
    }
}

void i18n_load(const std::string& dir, const std::string& lang)
{
    g_texts.clear();
    load_file(dir + "/lang_en.txt");
    if (lang != "en") load_file(dir + "/lang_" + lang + ".txt");
}

const std::string& tr(const char* key)
{
    static std::string tmp;
    auto it = g_texts.find(key);
    if (it != g_texts.end()) return it->second;
    tmp = key;
    return tmp;
}

std::string trf(const char* key, const std::string& a, const std::string& b)
{
    std::string s = tr(key), out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{' && i + 2 < s.size() && s[i + 2] == '}' && (s[i + 1] == '1' || s[i + 1] == '2')) {
            out += s[i + 1] == '1' ? a : b;
            i += 2;
        } else out += s[i];
    }
    return out;
}

// Номера языков PS4: 0 ja, 1 en-US, 2 fr, 3 es, 4 de, 5 it, 6 nl, 7 pt-PT, 8 ru, 9 ko, 10 zh-Hant, 11 zh-Hans,
// 12 fi, 13 sv, 14 da, 15 no, 16 pl, 17 pt-BR, 18 en-GB, 19 tr, 20 es-LA, 21 ar, 22 fr-CA, 23 cs, 24 hu, 25 el, 26 ro, 27 th, 28 vi, 29 id.
// Сейчас переведены только русский и английский; остальные добавляются сюда вместе с файлом языка.
std::string i18n_code_for_system_lang(int sysLang)
{
    switch (sysLang) {
        case 8: return "ru";
        default: return "en";
    }
}
