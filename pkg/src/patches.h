// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once
// Заплатки каталога из интернета (catalog_N.json + catalog_N.pak) с GitHub Releases репозитория ps4catalog.
// Приложение читает файл index.json последнего выпуска, сравнивает номер с применённой заплаткой, скачивает недостающие
// (проверяя размер и SHA-256), кладёт их в папку каталога на консоли. Применяются они при следующем запуске приложения (см. catalog_load).
#include <string>

enum PatchCheck { PC_NOT_CONFIGURED = 0, PC_OFFLINE, PC_UP_TO_DATE, PC_DOWNLOADED, PC_ERROR };

// Откуда качать (адрес папки выпуска со слэшем на конце), куда класть, и где лежит основной catalog.json (его отпечаток
// сверяется с index.json: заплатки делались для другого основного каталога - не ставим). Пустой url - загрузка выключена.
void patches_configure(const std::string& baseUrl, const std::string& dir, const std::string& baseCatalogFile);

// Проверяет и скачивает заплатки новее lastApplied. Звать из фонового потока и только при подтверждённом интернете.
// PC_OFFLINE - не удалось получить index.json (нет связи/сервера), PC_ERROR - файл повреждён, не сошлась сумма и т.п.
// info - пояснение для журнала. downloaded (необязательно) - сколько заплаток скачано.
PatchCheck patches_check_online(int lastApplied, std::string& info, int* downloaded = nullptr);
