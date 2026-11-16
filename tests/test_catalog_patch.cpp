// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
// Заплатки каталога: catalog_2.json (+ pak), catalog_3.json ...
#include "catalog.h"
#include <stdio.h>
#include <string>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(int argc, char** argv)
{
    CHECK(argc > 1);
    CHECK(catalog_load(argv[1]));
    CHECK(catalog_patch_count() == 2 && catalog_patch_last() == 3);
    CHECK(catalog_count() == 3);   // a, c (новая), d (новая); b удалена заплаткой 3

    const CatEntry* a = catalog_by_title_id("CUSA00001");
    CHECK(a && a->hash == std::string(40, 'b') && a->ru == 0 && a->voice == 1 && a->size_mb == 2048);
    CHECK(a->name_en == "Alpha");                 // остальные поля не тронуты
    CatImage im;
    CHECK(catalog_image(a->cover, 40, 40, im) && im.ok());   // обложка осталась в основном pak

    CHECK(catalog_by_title_id("CUSA00002") == nullptr);       // удалена

    const CatEntry* c = catalog_by_title_id("CUSA00003");
    CHECK(c && c->name_en == "Gamma v2" && c->cover.pak == 2);   // перезаписана заплаткой 3: обложка лежит в её pak (0 - основной, 1 - заплатка 2, 2 - заплатка 3)
    CHECK(catalog_image(c->cover, 40, 40, im) && im.ok());
    const CatEntry* d = catalog_by_title_id("CUSA00004");
    CHECK(d && d->cover.pak == 1 && catalog_image(d->cover, 40, 40, im) && im.ok());   // новая игра заплатки 2 - обложка в pak заплатки 2
    return 0;
}
