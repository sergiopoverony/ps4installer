#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "service.h"

// Плитка игры: одна на title_id. Патчи, дополнения и русификаторы считаются и показываются пометками.
struct Game {
    std::string title, title_id, path;
    uint64_t size = 0;
    int patches = 0, dlcs = 0, rus = 0;
};

// Собирает плитки из списка файлов службы, по алфавиту. Файлы без самой игры (только патч)
// плиток не дают. Копии одного файла (на диске и на флешке) учитываются один раз.
void build_games(const std::vector<PkgItem>& items, std::vector<Game>& out);

// Режет строку на строки не длиннее maxc символов (по пробелам, иначе жёстко), лишнее заменяет «…».
std::vector<std::string> wrap_lines(const std::string& s, int maxc, int maxLines);

// "5.4 ГБ" или "144 МБ".
std::string fmt_size(uint64_t bytes);
