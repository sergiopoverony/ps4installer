#include "pkg.h"

#include "log.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
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
    fclose(f);
    out->path = path;
    out->file_size = (uint64_t)st.st_size;
    return pkg_parse_header(buf, n, out, err);
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

void sort_for_install(std::vector<PkgInfo>* list) {
    sort_by_size(list);
    // Патч и дополнения нельзя ставить раньше своей игры: сразу после игры (того же title_id)
    // идут её патчи и дополнения, тоже от большего к меньшему. Если игры в списке нет
    // (она уже установлена), они остаются на своих местах по размеру.
    auto is_base = [](const PkgInfo& e) { return e.kind == "game" && !e.is_patch; };
    std::vector<PkgInfo> out;
    std::vector<bool> used(list->size(), false);
    for (size_t i = 0; i < list->size(); i++) {
        if (used[i]) {
            continue;
        }
        const PkgInfo& e = (*list)[i];
        if (!is_base(e)) {
            bool base_in_list = false;
            for (size_t j = 0; j < list->size(); j++) {
                base_in_list = base_in_list || (is_base((*list)[j]) && (*list)[j].title_id == e.title_id);
            }
            if (base_in_list) {
                continue;  // будет выведен вместе со своей игрой
            }
        }
        used[i] = true;
        out.push_back(e);
        if (is_base(e)) {
            for (size_t j = 0; j < list->size(); j++) {
                if (!used[j] && !is_base((*list)[j]) && (*list)[j].title_id == e.title_id) {
                    used[j] = true;
                    out.push_back((*list)[j]);
                }
            }
        }
    }
    // На случай двух игр с одним title_id: ничего не потеряем.
    for (size_t i = 0; i < list->size(); i++) {
        if (!used[i]) {
            out.push_back((*list)[i]);
        }
    }
    *list = out;
}
