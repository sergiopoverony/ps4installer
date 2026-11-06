#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "service.h"

// Плитка игры: одна на title_id. Патчи, дополнения и русификаторы считаются и показываются пометками.
struct Game {
    std::string title, title_id, path;
    std::string hash;             // вкладки «Каталог игр» и «Запрос на игры»: хэш раздачи (торрент), пусто - нет
    uint64_t size = 0;
    int patches = 0, dlcs = 0, rus = 0;
    bool incomplete = false;      // хотя бы один файл докачан не до конца
    bool installed = false;       // уже стоит на консоли
    bool listOnly = false;        // запись вкладки «Каталог игр»: игры нет ни на диске, ни на консоли
    bool usbOnly = false;         // вкладка «Избранное»: ссылка magnet сохранена на USB, загрузка не запускалась
    bool console = false;         // запись вкладки «Установленные игры»: берётся с консоли, а не из PKG
    std::vector<PkgItem> files;   // все файлы этой игры (сама игра, патчи, DLC, русификаторы)
};

// Что показывать и что отправлять на установку: если прошивка выше 9 (или неизвестна, fw_major < 0),
// файлы Backport не нужны (служба их всё равно пропустит).
std::vector<PkgItem> install_files(const Game& g, int fw_major);
uint64_t install_bytes(const Game& g, int fw_major);

// Порядок как у службы: игра, затем патчи (по возрастанию версии), DLC, русификаторы.
// Нужен только для показа списка в окне; настоящий порядок решает служба.
int file_rank(const PkgItem& it);
std::string file_label(const PkgItem& it);   // "Игра", "Патч 01.07", "DLC: имя", "Русификатор"

// Собирает плитки из списка файлов службы, по алфавиту. Файлы без самой игры (только патч)
// плиток не дают. Копии одного файла (на диске и на флешке) учитываются один раз.
void build_games(const std::vector<PkgItem>& items, std::vector<Game>& out);

// Режет строку на строки не длиннее maxc символов (по пробелам, иначе жёстко), лишнее заменяет «…».
std::vector<std::string> wrap_lines(const std::string& s, int maxc, int maxLines);

// "5.4 ГБ" или "144 МБ".
std::string fmt_size(uint64_t bytes);
