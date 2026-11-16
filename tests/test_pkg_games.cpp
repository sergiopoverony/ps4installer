// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
// Тесты логики приложения без консоли: группировка игр, перенос строк, размеры.
// Сборка и запуск: tests/test_pkg_games.py
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "games.h"
#include "i18n.h"
#include "utf8.h"

static PkgItem mk(const char* title_id, const char* kind, const char* title, uint64_t size, const char* path,
                  bool patch = false, bool rus = false, bool dlc_name = false, const char* cid = nullptr)
{
    PkgItem it;
    it.title_id = title_id;
    it.kind = kind;
    it.title = title;
    it.size = size;
    it.path = path;
    it.patch = patch;
    it.rus = rus;
    it.dlc_name = dlc_name;
    it.content_id = cid ? cid : (std::string("CID-") + path);
    return it;
}

int main()
{
    // --- язык: русский файл, запасной английский, подстановки
    i18n_load(LANG_DIR, "ru");
    assert(tr("tab.catalog") == "Каталог игр");
    assert(trf("space.need", "5 ГБ") == "Нужно: 5 ГБ");
    assert(tr("нет.такого.ключа") == "нет.такого.ключа");
    i18n_load(LANG_DIR, "en");
    assert(tr("tab.catalog") == "Games catalog");
    assert(fmt_size(1048576ULL) == "1 MB");
    i18n_load(LANG_DIR, "xx");   // нет файла языка - остаётся английский
    assert(tr("tab.catalog") == "Games catalog");
    assert(i18n_code_for_system_lang(8) == "ru" && i18n_code_for_system_lang(1) == "en" && i18n_code_for_system_lang(18) == "en");
    i18n_load(LANG_DIR, "ru");

    // --- группировка
    {
        std::vector<PkgItem> in;
        in.push_back(mk("CUSA2", "game", "Zelda", 5000, "/d/z.pkg"));
        in.push_back(mk("CUSA1", "game", "Боевая игра", 9000, "/d/b.pkg"));
        in.push_back(mk("CUSA1", "patch", "Боевая игра", 100, "/d/b_patch.pkg", true));
        in.push_back(mk("CUSA1", "dlc", "Боевая игра DLC", 50, "/d/b_dlc.pkg"));
        in.push_back(mk("CUSA1", "patch", "Боевая игра", 80, "/d/b_rus.pkg", true, true));
        in.push_back(mk("CUSA3", "patch", "Только патч", 10, "/d/p.pkg", true));   // без игры - плитки нет
        // копия той же игры на флешке (тот же content_id, тип, размер)
        in.push_back(mk("CUSA2", "game", "Zelda", 5000, "/usb/z.pkg", false, false, false, "CID-/d/z.pkg"));
        std::vector<Game> g;
        build_games(in, g);
        assert(g.size() == 2);
        assert(g[0].title_id == "CUSA2" || g[0].title_id == "CUSA1");
        // сортировка по алфавиту без учёта регистра для латиницы: "Zelda" < "Боевая игра" по байтам UTF-8
        assert(g[0].title == "Zelda" && g[1].title == "Боевая игра");
        assert(g[1].patches == 1 && g[1].dlcs == 1 && g[1].rus == 1);
        assert(g[0].path == "/d/z.pkg" && g[0].patches == 0);
    }
    // --- перенос строк
    {
        auto l = wrap_lines("Warhammer 40,000: Boltgun", 17, 2);
        assert(l.size() == 2 && l[0] == "Warhammer 40,000:" && l[1] == "Boltgun");
        l = wrap_lines("Assassin's Creed Valhalla Complete Edition", 17, 2);
        assert(l.size() == 2 && count_cp(l[0]) <= 17 && count_cp(l[1]) <= 17);
        assert(l[1].size() >= 3 && l[1].substr(l[1].size() - 3) == "\xE2\x80\xA6");   // последняя строка с «…»
        l = wrap_lines("Русская игра про космических дальнобойщиков", 12, 2);
        assert(l.size() == 2);
        for (auto& s : l) assert(count_cp(s) <= 12);
        // кириллица не разрезается посреди символа
        for (auto& s : l) { const unsigned char* p = (const unsigned char*)s.c_str(); while (*p) assert(decode_utf8(p) != '?'); }
        l = wrap_lines("Сверхдлинноеназваниебезпробелов", 10, 2);
        assert(l.size() == 2 && count_cp(l[0]) == 10);
        l = wrap_lines("", 10, 2);
        assert(l.empty());
    }
    // --- размеры
    {
        assert(fmt_size(5782044672ULL) == "5.4 ГБ");
        assert(fmt_size(144965632ULL) == "138 МБ");
        assert(fmt_size(1048576ULL) == "1 МБ");
    }
    printf("ok\n");
    return 0;
}
