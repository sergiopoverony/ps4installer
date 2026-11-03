#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Разбор заголовка PKG и работа с путями. Чистый C++, без вызовов консоли
// (проверяется тестами на ПК). Смещения полей заголовка взяты из открытых
// установщиков (flatz/ps4_remote_pkg_installer, cy33hc/ps4-webdav-client).

enum PkgContentType : uint32_t {
    PKG_TYPE_GD = 0x1A,  // игра, патч, ремастер
    PKG_TYPE_AC = 0x1B,  // дополнение с данными
    PKG_TYPE_AL = 0x1C,  // дополнение без данных
    PKG_TYPE_DP = 0x1E,  // дельта-патч
};

struct PkgInfo {
    std::string path;        // путь, как его видит служба (/data/pkg/... или /mnt/usbN/...)
    uint64_t file_size = 0;  // размер файла на диске
    uint64_t pkg_size = 0;   // размер из заголовка
    std::string content_id;  // 36 символов, например UP0006-CUSA15080_00-STARWARSQUADRONS
    std::string title_id;    // CUSA15080
    uint32_t content_type = 0;
    uint32_t flags = 0;
    std::string kind;        // game | patch | dlc | dlc_nodata | delta_patch
    bool is_patch = false;
};

// Читает и проверяет заголовок. При ошибке возвращает false и текст причины.
bool pkg_read_header(const std::string& path, PkgInfo* out, std::string* err);

// Разбор уже прочитанных первых байт файла (для тестов и pkg_read_header).
bool pkg_parse_header(const uint8_t* buf, size_t len, PkgInfo* out, std::string* err);

// Путь допустим для установки: только /data/pkg/... и /mnt/usbN/..., без "..", расширение .pkg.
bool path_allowed(const std::string& path);

// Путь, который понимает системная установка: /data/... -> /user/data/...
std::string bgft_path(const std::string& path);

// Ищет .pkg в /data/pkg и /mnt/usb0../mnt/usb9, читает заголовки.
std::vector<PkgInfo> scan_packages(size_t max_files);

// Просто по размеру, от большего к меньшему (для списка /scan).
void sort_by_size(std::vector<PkgInfo>* list);

// Порядок установки: от самого большого файла к самому маленькому, но патчи и дополнения
// идут сразу после своей игры (по title_id), даже если они больше неё.
void sort_for_install(std::vector<PkgInfo>* list);

// Корневые папки, которые просматривает scan_packages().
std::vector<std::string> pkg_roots();
