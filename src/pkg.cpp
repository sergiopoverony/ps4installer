#include "pkg.h"

#include "log.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <cstdio>
#include <cerrno>
#include <cstring>

#ifndef DATA_PKG_DIR
#define DATA_PKG_DIR "/data/pkg"
#endif
#ifndef USB_PREFIX
#define USB_PREFIX "/mnt/usb"
#endif

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t* p) {
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

bool pkg_parse_header(const uint8_t* buf, size_t len, PkgInfo* out, std::string* err) {
    if (len < 0x438) {
        *err = "файл слишком короткий для PKG";
        return false;
    }
    if (be32(buf) != 0x7F434E54) {  // "\x7FCNT"
        *err = "нет сигнатуры PKG";
        return false;
    }
    char cid[0x25];
    memcpy(cid, buf + 0x40, 0x24);
    cid[0x24] = 0;
    std::string content_id(cid);
    // формат: XXNNNN-CUSAnnnnn_00-XXXXXXXXXXXXXXXX (дефисы на позициях 6 и 19)
    if (content_id.size() != 36 || content_id[6] != '-' || content_id[19] != '-') {
        *err = "некорректный content id";
        return false;
    }
    for (char c : content_id) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            *err = "некорректный content id";
            return false;
        }
    }
    out->content_id = content_id;
    out->title_id = content_id.substr(7, 9);
    out->content_type = be32(buf + 0x74);
    out->flags = be32(buf + 0x78);
    out->pkg_size = be64(buf + 0x430);

    const uint32_t patch_mask = 0x00100000 | 0x40000000 | 0x41000000 | 0x60000000;
    out->is_patch = (out->flags & patch_mask) != 0;
    switch (out->content_type) {
        case PKG_TYPE_GD: out->kind = out->is_patch ? "patch" : "game"; break;
        case PKG_TYPE_AC: out->kind = "dlc"; break;
        case PKG_TYPE_AL: out->kind = "dlc_nodata"; break;
        case PKG_TYPE_DP: out->kind = "delta_patch"; break;
        default:
            *err = "неподдерживаемый тип пакета";
            return false;
    }
    return true;
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

// Разбор param.sfo (формат PSF): ищем TITLE, APP_VER/VERSION. Любая неувязка - просто выходим.
static void parse_sfo(const std::vector<uint8_t>& d, PkgInfo* out) {
    if (d.size() < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') {
        return;
    }
    uint32_t key_off = le32(&d[8]), data_off = le32(&d[12]), n = le32(&d[16]);
    if (n > 256) {
        return;
    }
    std::string version;
    for (uint32_t i = 0; i < n; i++) {
        size_t e = 20 + (size_t)i * 16;
        if (e + 16 > d.size()) {
            return;
        }
        size_t k = (size_t)key_off + le16(&d[e]);
        uint32_t len = le32(&d[e + 4]);
        size_t v = (size_t)data_off + le32(&d[e + 12]);
        if (k >= d.size() || v >= d.size() || len > 4096) {
            continue;
        }
        std::string key;
        for (size_t j = k; j < d.size() && d[j]; j++) {
            key += (char)d[j];
        }
        std::string val;
        for (size_t j = v; j < d.size() && j < v + len && d[j]; j++) {
            val += (char)d[j];
        }
        if (key == "TITLE") {
            out->title = val;
        } else if (key == "APP_VER") {
            out->app_ver = val;
        } else if (key == "VERSION") {
            version = val;
        }
    }
    if (out->app_ver.empty()) {
        out->app_ver = version;
    }
    int a = 0, b = 0;
    if (sscanf(out->app_ver.c_str(), "%d.%d", &a, &b) == 2) {
        out->ver_num = a * 100 + b;
    }
}

bool pkg_sfo_title_from_file(const std::string& path, std::string* title) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::vector<uint8_t> d(1 << 20);
    size_t n = fread(d.data(), 1, d.size(), f);
    fclose(f);
    d.resize(n);
    PkgInfo p;
    parse_sfo(d, &p);
    if (p.title.empty()) {
        return false;
    }
    *title = p.title;
    return true;
}

// Читает param.sfo из таблицы файлов PKG (запись с id 0x1000). Не проверено на настоящих PKG.
static void read_sfo(FILE* f, uint64_t file_size, const uint8_t* hdr, PkgInfo* out) {
    uint32_t count = be32(hdr + 0x10), table = be32(hdr + 0x18);
    if (count == 0 || count > 4096 || table == 0 || table >= file_size) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t e[32];
        if (fseeko(f, (off_t)table + (off_t)i * 32, SEEK_SET) != 0 || fread(e, 1, 32, f) != 32) {
            return;
        }
        if (be32(e) != 0x1000) {
            continue;
        }
        uint32_t off = be32(e + 16), size = be32(e + 20);
        if (size == 0 || size > 0x10000 || (uint64_t)off + size > file_size) {
            return;
        }
        std::vector<uint8_t> d(size);
        if (fseeko(f, (off_t)off, SEEK_SET) != 0 || fread(d.data(), 1, size, f) != size) {
            return;
        }
        parse_sfo(d, out);
        return;
    }
}

bool pkg_read_icon(const std::string& path, std::vector<uint8_t>* out) {
    out->clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    uint8_t hdr[0x20];
    struct stat st;
    bool ok = false;
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && fstat(fileno(f), &st) == 0 && be32(hdr) == 0x7F434E54) {
        uint32_t count = be32(hdr + 0x10), table = be32(hdr + 0x18);
        uint64_t fsz = (uint64_t)st.st_size;
        if (count > 0 && count <= 4096 && table != 0 && table < fsz) {
            for (uint32_t i = 0; i < count && !ok; i++) {
                uint8_t e[32];
                if (fseeko(f, (off_t)table + (off_t)i * 32, SEEK_SET) != 0 || fread(e, 1, 32, f) != 32) {
                    break;
                }
                if (be32(e) != 0x1200) {
                    continue;
                }
                uint32_t off = be32(e + 16), size = be32(e + 20);
                if (size == 0 || size > (4u << 20) || (uint64_t)off + size > fsz) {
                    break;
                }
                out->resize(size);
                ok = fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(out->data(), 1, size, f) == size;
            }
        }
    }
    fclose(f);
    if (!ok) {
        out->clear();
    }
    return ok;
}

// Слова из строки: буквы, цифры и байты UTF-8 (кириллица), остальное - разделители.
static std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> w;
    std::string cur;
    for (unsigned char c : s) {
        if (isalnum(c) || c >= 0x80) {
            cur += (char)(c < 0x80 ? tolower(c) : c);
        } else if (!cur.empty()) {
            w.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) {
        w.push_back(cur);
    }
    return w;
}

static bool has_word(const std::vector<std::string>& w, const char* a, const char* b = nullptr,
                     const char* c = nullptr, const char* d = nullptr) {
    for (const std::string& x : w) {
        if (x == a || (b && x == b) || (c && x == c) || (d && x == d)) {
            return true;
        }
    }
    return false;
}

void pkg_apply_name_tags(PkgInfo* p) {
    // имя файла без папок и расширения + имя ближайшей папки
    std::string path = p->path;
    size_t slash = path.rfind('/');
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string dir;
    if (slash != std::string::npos && slash > 0) {
        size_t s2 = path.rfind('/', slash - 1);
        dir = path.substr(s2 == std::string::npos ? 0 : s2 + 1, slash - (s2 == std::string::npos ? 0 : s2 + 1));
    }
    size_t dot = file.rfind('.');
    if (dot != std::string::npos) {
        file.resize(dot);
    }
    std::vector<std::string> w = words_of(file);
    std::vector<std::string> wd = words_of(dir);
    w.insert(w.end(), wd.begin(), wd.end());
    bool back_port = false;
    for (size_t i = 0; i + 1 < w.size(); i++) {
        back_port = back_port || (w[i] == "back" && w[i + 1] == "port");
    }
    p->backport = back_port || has_word(w, "backport", "backports", "bp");
    p->rus = has_word(w, "rus", "ru", "russian", "rusifikator") ||
             has_word(w, "русификатор", "рус", "русский", "русская") ||
             has_word(w, "Русификатор", "РУС", "Русский", "Русская");
    p->dlc_name = has_word(w, "dlc", "dlcs", "expansion", "addon");
}

int pkg_probe_zeros(const std::string& path, uint64_t size, long mtime) {
    // /scan может идти из нескольких потоков одновременно (двойное нажатие «обновить»): кэш под замком.
    static std::map<std::string, int> cache;
    static std::mutex mu;
    std::string key = path + "|" + std::to_string(size) + "|" + std::to_string(mtime);
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }
    int zeros = 0;
    const uint64_t BLK = 4096;
    const int N = 48;
    if (size >= 8ULL * 1024 * 1024) {
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            std::vector<uint8_t> b(BLK);
            for (int i = 0; i <= N; i++) {
                // N мест по всему файлу (без самого начала) и последняя проба - самый конец.
                uint64_t off = (i == N) ? size - BLK : ((size / (N + 1)) * (uint64_t)(i + 1)) / BLK * BLK;
                if (fseeko(f, (off_t)off, SEEK_SET) != 0 || fread(b.data(), 1, BLK, f) != BLK) {
                    continue;
                }
                bool zero = true;
                for (uint64_t k = 0; k < BLK && zero; k++) {
                    zero = b[k] == 0;
                }
                if (zero) {
                    zeros++;
                }
            }
            fclose(f);
        }
        if (zeros > 0) {
            log_printf("pkg: %d нулевых проб из %d: %s", zeros, N + 1, path.c_str());
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        cache[key] = zeros;
    }
    return zeros;
}

bool pkg_read_header(const std::string& path, PkgInfo* out, std::string* err) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        *err = "файл не найден";
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        *err = "это не обычный файл";
        return false;
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        *err = "не удалось открыть файл";
        return false;
    }
    uint8_t buf[0x1000];
    size_t n = fread(buf, 1, sizeof(buf), f);
    out->path = path;
    out->file_size = (uint64_t)st.st_size;
    bool ok = pkg_parse_header(buf, n, out, err);
    if (ok) {
        out->zero_blocks = pkg_probe_zeros(path, out->file_size, (long)st.st_mtime);
        read_sfo(f, out->file_size, buf, out);
        pkg_apply_name_tags(out);
    }
    fclose(f);
    return ok;
}

static bool has_pkg_ext(const std::string& p) {
    if (p.size() < 5) {
        return false;
    }
    std::string e = p.substr(p.size() - 4);
    for (char& c : e) {
        c = (char)tolower((unsigned char)c);
    }
    return e == ".pkg";
}

static bool starts_with(const std::string& s, const char* pre) {
    return s.compare(0, strlen(pre), pre) == 0;
}

static bool has_dotdot(const std::string& p) {
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) {
            j = p.size();
        }
        if (p.compare(i, j - i, "..") == 0 && j - i == 2) {
            return true;
        }
        i = j + 1;
    }
    return false;
}

bool path_allowed(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path.find('\0') != std::string::npos) {
        return false;
    }
    if (path[0] != '/' || has_dotdot(path) || !has_pkg_ext(path)) {
        return false;
    }
    if (starts_with(path, DATA_PKG_DIR "/")) {
        return true;
    }
    // /mnt/usbN/...  (N - одна или несколько цифр)
    if (starts_with(path, USB_PREFIX)) {
        size_t i = strlen(USB_PREFIX);
        size_t digits = 0;
        while (i < path.size() && path[i] >= '0' && path[i] <= '9') {
            i++;
            digits++;
        }
        return digits > 0 && i < path.size() && path[i] == '/';
    }
    return false;
}

std::string bgft_path(const std::string& path) {
    if (starts_with(path, "/data/")) {
        return "/user" + path;
    }
    return path;
}

std::vector<std::string> pkg_roots() {
    std::vector<std::string> roots;
    roots.push_back(DATA_PKG_DIR);
    for (int i = 0; i < 10; i++) {
        roots.push_back(std::string(USB_PREFIX) + std::to_string(i));
    }
    return roots;
}

static bool skip_dir(const char* name) {
    return name[0] == '.' || strcmp(name, "$RECYCLE.BIN") == 0 ||
           strcmp(name, "System Volume Information") == 0 || strcmp(name, "disc") == 0;
}

static void scan_dir(const std::string& dir, int depth, size_t max_files, std::vector<PkgInfo>* out) {
    if (depth > 6 || out->size() >= max_files) {
        return;
    }
    DIR* d = opendir(dir.c_str());
    if (!d) {
        if (depth > 0) {
            log_printf("scan: не открылась папка %s (errno=%d)", dir.c_str(), errno);
        }
        return;
    }
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    if (depth > 0) {
        log_printf("scan: папка %s, записей: %d", dir.c_str(), (int)names.size());
    }
    for (const std::string& name : names) {
        if (out->size() >= max_files) {
            return;
        }
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) {
            log_printf("scan: stat не удался для %s (errno=%d)", full.c_str(), errno);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!skip_dir(name.c_str())) {
                scan_dir(full, depth + 1, max_files, out);
            }
        } else if (S_ISREG(st.st_mode) && has_pkg_ext(name)) {
            PkgInfo info;
            std::string err;
            if (pkg_read_header(full, &info, &err)) {
                out->push_back(info);
            } else {
                log_printf("scan: файл отвергнут %s: %s", full.c_str(), err.c_str());
            }
        }
    }
}

std::vector<PkgInfo> scan_packages(size_t max_files) {
    std::vector<PkgInfo> out;
    for (const std::string& root : pkg_roots()) {
        scan_dir(root, 0, max_files, &out);
    }
    return out;
}

void sort_by_size(std::vector<PkgInfo>* list) {
    std::stable_sort(list->begin(), list->end(), [](const PkgInfo& a, const PkgInfo& b) {
        if (a.file_size != b.file_size) {
            return a.file_size > b.file_size;
        }
        return a.path < b.path;
    });
}

// base_titles - игры (без пометки русификатора), которые есть в этой же пачке: если у пакета с
// пометкой RUS есть такая игра, он точно не основа, даже если в заголовке записан как "game".
static int rank_of(const PkgInfo& p, const std::map<std::string, bool>& base_titles) {
    bool has_base = base_titles.count(p.title_id) > 0;
    bool extra = p.kind != "game" || has_base;
    if (p.rus && extra) {
        return 3;
    }
    if (p.kind == "dlc" || p.kind == "dlc_nodata" || p.dlc_name) {
        return 2;
    }
    if (p.is_patch || p.kind == "delta_patch") {
        return 1;
    }
    return 0;
}

std::vector<PkgInfo> plan_install(const std::vector<PkgInfo>& in, int fw_major, std::vector<SkippedPkg>* skipped) {
    std::map<std::string, bool> base_titles;
    for (const PkgInfo& p : in) {
        if (p.kind == "game" && !p.is_patch && !p.rus) {
            base_titles[p.title_id] = true;
        }
    }
    auto rank_of = [&](const PkgInfo& p) { return ::rank_of(p, base_titles); };
    std::vector<PkgInfo> keep;
    for (const PkgInfo& p : in) {
        if (p.backport && (fw_major < 0 || fw_major > 9)) {
            if (skipped) {
                skipped->push_back({p, fw_major < 0 ? "backport: версия прошивки неизвестна"
                                                    : "backport: прошивка " + std::to_string(fw_major) + " выше 9"});
            }
            continue;
        }
        keep.push_back(p);
    }
    // из обычных патчей одной игры оставляем самый новый (если версия известна)
    std::map<std::string, int> newest;
    for (const PkgInfo& p : keep) {
        if (rank_of(p) == 1 && p.kind == "patch" && p.ver_num >= 0) {
            int& v = newest[p.title_id];
            v = std::max(v, p.ver_num);
        }
    }
    std::vector<PkgInfo> kept2;
    for (const PkgInfo& p : keep) {
        if (rank_of(p) == 1 && p.kind == "patch" && p.ver_num >= 0 && p.ver_num < newest[p.title_id]) {
            if (skipped) {
                skipped->push_back({p, "есть более новый патч (версия " + std::to_string(newest[p.title_id] / 100) +
                                           "." + (newest[p.title_id] % 100 < 10 ? "0" : "") +
                                           std::to_string(newest[p.title_id] % 100) + ")"});
            }
            continue;
        }
        kept2.push_back(p);
    }
    // порядок игр: по размеру самой большой "основы" (если игры в списке нет - по самому большому файлу)
    std::map<std::string, uint64_t> anchor;
    for (const PkgInfo& p : kept2) {
        uint64_t& a = anchor[p.title_id];
        if (rank_of(p) == 0) {
            a = std::max<uint64_t>(a, p.file_size | (1ULL << 62));  // основа важнее любого другого файла
        } else {
            a = std::max<uint64_t>(a, p.file_size);
        }
    }
    std::vector<PkgInfo> out = kept2;
    std::stable_sort(out.begin(), out.end(), [&](const PkgInfo& a, const PkgInfo& b) {
        if (a.title_id != b.title_id) {
            uint64_t x = anchor[a.title_id], y = anchor[b.title_id];
            if (x != y) {
                return x > y;
            }
            return a.title_id < b.title_id;
        }
        int ra = rank_of(a), rb = rank_of(b);
        if (ra != rb) {
            return ra < rb;
        }
        if (ra == 1 && a.ver_num >= 0 && b.ver_num >= 0 && a.ver_num != b.ver_num) {
            return a.ver_num < b.ver_num;
        }
        if (a.file_size != b.file_size) {
            return a.file_size > b.file_size;
        }
        return a.path < b.path;
    });
    return out;
}
