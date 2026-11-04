#include "catalog.h"
#include "json.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_BMP
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

static std::vector<CatEntry> g_entries;
static std::string g_pakPath;

static bool read_file(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static CatRef ref_of(const JsonValue& v)
{
    CatRef r;
    if (v.type == JsonValue::ARR && v.arr.size() == 2) {
        r.off = (uint64_t)v.arr[0].asNum();
        r.len = (uint64_t)v.arr[1].asNum();
    }
    return r;
}

bool catalog_load(const std::string& dir)
{
    g_entries.clear();
    std::string text;
    if (!read_file(dir + "/catalog.json", text)) { logf_("catalog: нет %s/catalog.json", dir.c_str()); return false; }
    JsonValue j;
    if (!json_parse(text, j)) { logf_("catalog: catalog.json не разобран"); return false; }
    const JsonValue& games = j["games"];
    for (size_t i = 0; i < games.arr.size(); i++) {
        const JsonValue& g = games.arr[i];
        CatEntry e;
        e.id = g["id"].asStr();
        e.cid = g["cid"].asStr();
        // cid вида EP0001-CUSA18535_00-GAME000000000000: Title ID - между первым и вторым '-', до '_'
        size_t a = e.cid.find('-');
        if (a != std::string::npos && e.cid.size() >= a + 10) e.title_id = e.cid.substr(a + 1, 9);
        e.name_en = g["n"].asStr();
        e.name_ru = g["r"].asStr();
        e.cover = ref_of(g["c"]);
        e.desc = ref_of(g["d"]);
        const JsonValue& s = g["s"];
        for (size_t k = 0; k < s.arr.size(); k++) e.shots.push_back(ref_of(s.arr[k]));
        g_entries.push_back(e);
    }
    g_pakPath = dir + "/catalog.pak";
    logf_("catalog: загружено игр %d", (int)g_entries.size());
    return !g_entries.empty();
}

int catalog_count() { return (int)g_entries.size(); }
const CatEntry* catalog_at(int i) { return (i >= 0 && i < (int)g_entries.size()) ? &g_entries[(size_t)i] : nullptr; }

const CatEntry* catalog_by_title_id(const std::string& title_id)
{
    if (title_id.empty()) return nullptr;
    for (size_t i = 0; i < g_entries.size(); i++)
        if (g_entries[i].title_id == title_id) return &g_entries[i];
    return nullptr;
}

// Ключ названия: только буквы и цифры (латиница и кириллица в нижнем регистре); ™ ® ' : - и пробелы отбрасываются.
static std::string name_key(const std::string& s)
{
    std::string k;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { if (isalnum(c)) k += (char)tolower(c); }
        else if (c == 0xD0 || c == 0xD1) {          // кириллица: приводим заглавные к строчным
            if (i + 1 < s.size()) {
                unsigned char d = (unsigned char)s[i + 1];
                if (c == 0xD0 && d >= 0x90 && d <= 0x9F) { k += (char)0xD0; k += (char)(d + 0x20); }
                else if (c == 0xD0 && d >= 0xA0 && d <= 0xAF) { k += (char)0xD1; k += (char)(d - 0x20); }
                else { k += (char)c; k += (char)d; }
                i++;
            }
        } else if (c >= 0xC0) {                      // остальные многобайтовые символы (™ и т.п.) пропускаем целиком
            int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            i += (size_t)n;
        }
    }
    return k;
}

const CatEntry* catalog_by_name(const std::string& name)
{
    std::string k = name_key(name);
    if (k.size() < 3) return nullptr;
    for (size_t i = 0; i < g_entries.size(); i++)
        if (name_key(g_entries[i].name_en) == k || (!g_entries[i].name_ru.empty() && name_key(g_entries[i].name_ru) == k)) return &g_entries[i];
    // Не нашли точно: берём игру, название которой начинается с нашего (в каталоге часто дописано "- Game of the
    // Year Edition"), если наше не короче половины. Из нескольких подходящих - с самым коротким названием.
    const CatEntry* best = nullptr;
    size_t bestLen = 0;
    if (k.size() >= 8) {
        for (size_t i = 0; i < g_entries.size(); i++) {
            std::string e = name_key(g_entries[i].name_en);
            if (e.size() > k.size() && e.compare(0, k.size(), k) == 0 && k.size() * 2 >= e.size() &&
                (!best || e.size() < bestLen)) { best = &g_entries[i]; bestLen = e.size(); }
        }
    }
    return best;
}

const CatEntry* catalog_find(const std::string& title_id, const std::string& name, const char** how)
{
    const CatEntry* e = catalog_by_title_id(title_id);
    if (e) { if (how) *how = "id"; return e; }
    e = catalog_by_name(name);
    if (e && how) *how = "name";
    return e;
}

static bool read_blob(const CatRef& r, std::string& out)
{
    out.clear();
    if (r.len == 0 || r.len > (64u << 20)) return false;
    FILE* f = fopen(g_pakPath.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    if (fseeko(f, (off_t)r.off, SEEK_SET) == 0) {
        out.resize((size_t)r.len);
        ok = fread(&out[0], 1, (size_t)r.len, f) == (size_t)r.len;
    }
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

std::string catalog_text(const CatEntry& e)
{
    std::string s;
    if (!read_blob(e.desc, s)) return "";
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t') s[i] = ' ';
    return s;
}

bool catalog_image(const CatRef& ref, int w, int h, CatImage& out)
{
    out = CatImage();
    std::string blob;
    if (!read_blob(ref, blob)) return false;
    return catalog_image_from_blob(blob, w, h, out);
}

bool catalog_decode_rgba(const std::string& blob, int& w, int& h, std::vector<uint32_t>& argb)
{
    int comp = 0;
    unsigned char* src = stbi_load_from_memory((const unsigned char*)blob.data(), (int)blob.size(), &w, &h, &comp, 4);
    if (!src || w <= 0 || h <= 0) { if (src) stbi_image_free(src); return false; }
    argb.resize((size_t)w * h);
    for (size_t i = 0; i < argb.size(); i++)
        argb[i] = ((uint32_t)src[i * 4 + 3] << 24) | ((uint32_t)src[i * 4] << 16) | ((uint32_t)src[i * 4 + 1] << 8) | src[i * 4 + 2];
    stbi_image_free(src);
    return true;
}

bool catalog_image_from_blob(const std::string& blob, int w, int h, CatImage& out)
{
    out = CatImage();
    if (w <= 0 || h <= 0 || blob.empty()) return false;
    int sw = 0, sh = 0, comp = 0;
    unsigned char* src = stbi_load_from_memory((const unsigned char*)blob.data(), (int)blob.size(), &sw, &sh, &comp, 3);
    if (!src || sw <= 0 || sh <= 0) { if (src) stbi_image_free(src); return false; }

    // Рамка заполняется целиком: берём по центру кусок исходника с нужным соотношением сторон.
    double scale = (double)sw / w;
    if ((double)sh / h < scale) scale = (double)sh / h;
    double cw = w * scale, ch = h * scale;       // размер вырезаемой области в исходнике
    double x0 = (sw - cw) / 2, y0 = (sh - ch) / 2;
    out.w = w; out.h = h;
    out.px.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
        int sy0 = (int)(y0 + y * scale), sy1 = (int)(y0 + (y + 1) * scale);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sh) sy1 = sh;
        for (int x = 0; x < w; x++) {
            int sx0 = (int)(x0 + x * scale), sx1 = (int)(x0 + (x + 1) * scale);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > sw) sx1 = sw;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const unsigned char* p = src + ((size_t)yy * sw + sx0) * 3;
                for (int xx = sx0; xx < sx1; xx++, p += 3) { r += p[0]; g += p[1]; b += p[2]; n++; }
            }
            if (n == 0) n = 1;
            out.px[(size_t)y * w + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    stbi_image_free(src);
    return true;
}
