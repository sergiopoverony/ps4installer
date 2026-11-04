// Шаг 2: шапка (Logo + три вкладки, L1/R1) и вкладка «Доступные игры»: сетка игр по данным
// службы (/scan), навигация крестовиной и левым стиком, Квадрат - обновить список.
// Остальные вкладки пока заглушки; окно установки добавляется следующим шагом.
#include "ui.h"
#include "catalog.h"
#include "scene.h"
#include "log.h"
#include "service.h"
#include "games.h"
#include "utf8.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

extern "C" uint64_t sceKernelGetProcessTime(void);
extern "C" int sceSystemServiceHideSplashScreen(void);

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

#define FRAME_WIDTH   1920
#define FRAME_HEIGHT  1080
#define FRAME_DEPTH      4

static const int TITLE_PX = 32;   // название игры в карточке (если не влезает - уменьшается, но не ниже TITLE_MIN_PX)
static const int TITLE_MIN_PX = 16;
static const int DESC_PX = 26;    // описание игры в карточке
static const int FONT_PX  = 30;   // крупно, чтобы читалось с дивана; подберём по телевизору
static const int MARGIN_X = 40;
static const int HEAD_H   = 90;

static const Color C_BG     = { 22,  24,  30 };
static const Color C_HEAD   = { 32,  36,  46 };
static const Color C_TEXT   = { 230, 230, 230 };
static const Color C_DIM    = { 150, 155, 165 };
static const Color C_TAB_ON = { 70, 110, 200 };
static const Color C_TAB_OFF = { 44, 49, 62 };
static const Color C_LOGO   = { 60,  66,  84 };
static const Color C_TILE   = { 34,  38,  48 };
static const Color C_TILE_SEL = { 52, 60,  82 };
static const Color C_WARN   = { 235, 180,  80 };
static const Color C_EDGE   = { 90, 110, 170 };
static const Color C_PANEL  = { 40,  44,  56 };
static const Color C_RED_BG = { 170,  40,  40 };
static const Color C_GREEN  = { 110, 210, 130 };
static const Color C_BAR_BG = { 24,  26,  34 };

// Сетка игр: 5 колонок, 2 ряда на экране; ниже - прокрутка по рядам.
static const int COLS = 5, ROWS_VIS = 2;
static const int TILE_W = 340, IMG_H = 340, TEXT_H = 72;
static const int TILE_H = IMG_H + TEXT_H;
static const int GAP_Y = 20;
static const int GRID_TOP = HEAD_H + 20;

static Scene2D* g_scene = NULL;
static FT_Face  g_font;
static int      g_cellW = 16;
static bool     g_ready = false;
static int      g_redraw = 2;
static int      g_frameID = 0;

enum Tab { TAB_AVAILABLE = 0, TAB_INSTALLED = 1, TAB_CATALOG = 2, TAB_REQUESTS = 3, TAB_COUNT = 4 };
static int g_tab = TAB_AVAILABLE;

static const char* TAB_NAMES[TAB_COUNT] = {
    "Доступные игры", "Установленные игры", "Каталог игр", "Запрос на игры"
};

// ---------------------------------------------------------------- UTF-8 и текст

static void draw_text(const std::string& s, int x, int baseline, Color fg, Color bg)
{
    FT_GlyphSlot slot = g_font->glyph;
    const unsigned char* p = (const unsigned char*)s.c_str();

    while (*p) {
        int cp = decode_utf8(p);
        FT_UInt gi = FT_Get_Char_Index(g_font, (FT_ULong)cp);

        if (FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) continue;
        if (FT_Render_Glyph(slot, ft_render_mode_normal)) continue;

        for (unsigned yy = 0; yy < slot->bitmap.rows; yy++) {
            for (unsigned xx = 0; xx < slot->bitmap.width; xx++) {
                unsigned a = slot->bitmap.buffer[yy * slot->bitmap.pitch + xx];
                if (a == 0) continue;

                int px = x + (int)xx + slot->bitmap_left;
                int py = baseline + (int)yy - slot->bitmap_top;
                if (px < 0 || py < 0 || px >= (int)FRAME_WIDTH || py >= (int)FRAME_HEIGHT) continue;

                Color c;
                c.r = (uint8_t)((bg.r * (255 - a) + fg.r * a) / 255);
                c.g = (uint8_t)((bg.g * (255 - a) + fg.g * a) / 255);
                c.b = (uint8_t)((bg.b * (255 - a) + fg.b * a) / 255);
                g_scene->PutPixel(px, py, c);
            }
        }
        x += (int)(slot->advance.x >> 6);
    }
}

// Обрезает строку до maxChars символов (не байтов) и добавляет «…».
static std::string fit_chars(const std::string& s, int maxChars)
{
    if (count_cp(s) <= maxChars) return s;
    const unsigned char* start = (const unsigned char*)s.c_str();
    const unsigned char* p = start;
    for (int i = 0; i < maxChars - 1; i++) decode_utf8(p);
    return std::string((const char*)start, (size_t)(p - start)) + "\xE2\x80\xA6";
}

// Ширина текста в пикселях (шрифт моноширинный: все символы одной ширины).
static int text_w(const std::string& s) { return count_cp(s) * g_cellW; }

static void fill(int x, int y, int w, int h, Color c) { g_scene->FillRect(x, y, w, h, c); }

// ---- Значки кнопок геймпада. В тексте подсказок: {X} крест, {O} круг, {S} квадрат, {T} треугольник.
static const int ICON_S = 38;
static bool is_shoulder(char k) { return k == 'L' || k == 'R' || k == 'l' || k == 'r'; }   // L1 R1 (большие буквы) и L2 R2 (маленькие)
static int icon_w(char kind) { return is_shoulder(kind) ? ICON_S * 2 : ICON_S; }   // плечевые кнопки шире

// Попадает ли точка (fx, fy) внутрь рисунка значка (координаты в пикселях внутри квадрата ICON_S).
static bool icon_sample(char kind, double fx, double fy)
{
    const double R = ICON_S / 2.0;
    double dx = fx - R, dy = fy - R, d = sqrt(dx * dx + dy * dy);
    if (d > R) return false;
    if (d >= R - 3.0) return true;   // кольцо
    double u = dx / (R * 0.5), v = dy / (R * 0.5);
    const double t = 0.24;
    if (kind == 'X') return fabs(u) <= 1.0 && fabs(v) <= 1.0 && (fabs(u - v) < t * 1.3 || fabs(u + v) < t * 1.3);
    if (kind == 'O') { double r = sqrt(u * u + v * v); return r <= 1.0 && r >= 1.0 - 2 * t; }
    if (kind == 'S') return fabs(u) <= 0.95 && fabs(v) <= 0.95 && !(fabs(u) < 0.95 - 2 * t && fabs(v) < 0.95 - 2 * t);
    bool out = v >= -1.0 && v <= 0.8 && fabs(u) <= (v + 1.0) / 1.8;
    double u2 = u / 0.5, v2 = v / 0.5 + 0.35;   // уменьшенный треугольник для выреза
    bool in = v2 >= -1.0 && v2 <= 0.8 && fabs(u2) <= (v2 + 1.0) / 1.8;
    return out && !in;
}

static bool rr_sample(double px, double py, double x0, double y0, double w, double h, double r)
{
    if (px < x0 || py < y0 || px >= x0 + w || py >= y0 + h) return false;
    double cx = px < x0 + r ? x0 + r : (px > x0 + w - r ? x0 + w - r : px);
    double cy = py < y0 + r ? y0 + r : (py > y0 + h - r ? y0 + h - r : py);
    return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r;
}

static Color mix(Color a, Color b, int cov9)   // a - значок, b - фон; cov9 из 9
{
    Color c;
    c.r = (uint8_t)((a.r * cov9 + b.r * (9 - cov9)) / 9);
    c.g = (uint8_t)((a.g * cov9 + b.g * (9 - cov9)) / 9);
    c.b = (uint8_t)((a.b * cov9 + b.b * (9 - cov9)) / 9);
    return c;
}

// Готовые значки (pkg/assets/images/btn_*.png, их рисует tools/make_button_icons.py): гладкие края, прозрачность.
struct IconPng { int w = 0, h = 0; std::vector<uint32_t> px; bool tried = false; };
static IconPng g_iconPng[8];

static IconPng* icon_png(char kind)
{
    static const char kinds[] = "XOSTLRlr";
    static const char* names[] = { "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2" };
    const char* p = strchr(kinds, kind);
    if (!p || !kind) return nullptr;
    int i = (int)(p - kinds);
    IconPng& ic = g_iconPng[i];
    if (!ic.tried) {
        ic.tried = true;
        std::string path = std::string("/app0/assets/images/btn_") + names[i] + ".png", blob;
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) blob.append(buf, n);
            fclose(f);
        }
        if (blob.empty() || !catalog_decode_rgba(blob, ic.w, ic.h, ic.px)) { ic.w = ic.h = 0; ic.px.clear(); logf_("ui: нет значка %s", path.c_str()); }
    }
    return ic.w > 0 ? &ic : nullptr;
}

static void draw_pad_icon(char kind, int x, int top, Color bg = C_BG)
{
    if (IconPng* ic = icon_png(kind)) {
        int y0 = is_shoulder(kind) ? top + (ICON_S - ic->h) / 2 : top;
        for (int yy = 0; yy < ic->h; yy++)
            for (int xx = 0; xx < ic->w; xx++) {
                uint32_t v = ic->px[(size_t)yy * ic->w + xx];
                unsigned a = v >> 24;
                if (!a) continue;
                Color c;
                c.r = (uint8_t)((((v >> 16) & 255) * a + bg.r * (255 - a)) / 255);
                c.g = (uint8_t)((((v >> 8) & 255) * a + bg.g * (255 - a)) / 255);
                c.b = (uint8_t)(((v & 255) * a + bg.b * (255 - a)) / 255);
                g_scene->PutPixel(x + xx, y0 + yy, c);
            }
        return;
    }
    bool shoulder = is_shoulder(kind);
    Color col = kind == 'X' ? Color{ 110, 160, 255 } : kind == 'O' ? Color{ 255, 105, 105 } :
                kind == 'S' ? Color{ 235, 135, 205 } : kind == 'T' ? Color{ 95, 215, 150 } : Color{ 175, 180, 200 };
    const int w = icon_w(kind), h = shoulder ? ICON_S - 10 : ICON_S, y0 = shoulder ? top + 5 : top;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int cov = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    double fx = xx + (sx + 0.5) / 3.0, fy = yy + (sy + 0.5) / 3.0;
                    bool on = shoulder ? (rr_sample(fx, fy, 0, 0, w, h, 8) && !rr_sample(fx, fy, 2.5, 2.5, w - 5, h - 5, 5.5))
                                       : icon_sample(kind, fx, fy);
                    if (on) cov++;
                }
            if (cov) g_scene->PutPixel(x + xx, y0 + yy, mix(col, bg, cov));
        }
    }
    if (shoulder) {
        FT_Set_Pixel_Sizes(g_font, 0, 21);
        int cell = FT_Load_Char(g_font, 'M', FT_LOAD_DEFAULT) == 0 ? (int)(g_font->glyph->advance.x >> 6) : 12;
        draw_text(kind == 'L' ? "L1" : kind == 'R' ? "R1" : kind == 'l' ? "L2" : "R2", x + (w - 2 * cell) / 2, y0 + h - 7, col, bg);
        FT_Set_Pixel_Sizes(g_font, 0, FONT_PX);
    }
}

// Ширина строки подсказки в пикселях.
static int hint_w(const std::string& s)
{
    int w = 0;
    std::string seg;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '{' && i + 2 < s.size() && s[i + 2] == '}') { w += text_w(seg) + icon_w(s[i + 1]) + 4; seg.clear(); i += 3; }
        else { seg += s[i]; i++; }
    }
    return w + text_w(seg);
}

static void draw_hint(const std::string& s, int x, int baseline, Color fg, Color bg)
{
    std::string seg;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '{' && i + 2 < s.size() && s[i + 2] == '}') {
            draw_text(seg, x, baseline, fg, bg);
            x += text_w(seg);
            draw_pad_icon(s[i + 1], x, baseline - ICON_S + 6, bg);
            x += icon_w(s[i + 1]) + 4;
            seg.clear();
            i += 3;
        } else { seg += s[i]; i++; }
    }
    draw_text(seg, x, baseline, fg, bg);
}

// ---------------------------------------------------------------- игры

// Запуск службы: проверить, при необходимости отправить ELF из PKG в BinLoader GoldHEN и дождаться ответа.
enum SvcState { SVC_CHECK = 0, SVC_STARTING, SVC_OK, SVC_NO_LOADER, SVC_FAIL };
static SvcState g_svc = SVC_CHECK;
static bool g_svcDrawn = false;   // сообщение «Проверка/Запуск» уже показано на экране
static bool g_autorunTried = false;

enum LoadState { LS_NONE = 0, LS_LOADING, LS_OK, LS_ERROR };
static LoadState g_load = LS_NONE;
static std::string g_loadErr;
static std::vector<Game> g_games;
static int g_cursor = 0;
static int g_topRow = 0;
static std::vector<CatImage> g_tileImg;   // картинки плиток (см. tile_images_step)
static std::vector<char> g_tileTried;
static std::string g_status;   // строка под сеткой

// У каждой вкладки со списком своё состояние (игры, курсор, картинки). Активное лежит в g_games, g_cursor и т.д.;
// при смене вкладки оно откладывается в g_lists, а нужное достаётся оттуда.
struct ListState {
    std::vector<Game> games;
    int cursor = 0, topRow = 0;
    std::vector<CatImage> img;
    std::vector<char> tried;
    LoadState load = LS_NONE;
    std::string err;
};
static ListState g_lists[TAB_COUNT];
static int g_listTab = TAB_AVAILABLE;
static bool g_installedStale = true;
static bool g_catalogStale = true;     // список каталога надо собрать заново (первый раз, после установки/удаления)   // список установленных надо перечитать (первый раз, после установки)
static bool g_preloaded = false;       // после первой загрузки доступных игр установленные подгружены в фоне

static void switch_list(int newTab)
{
    if (newTab == g_listTab) return;
    ListState& s = g_lists[g_listTab];
    s.games.swap(g_games); s.img.swap(g_tileImg); s.tried.swap(g_tileTried);
    std::swap(s.cursor, g_cursor); std::swap(s.topRow, g_topRow); std::swap(s.load, g_load); s.err.swap(g_loadErr);
    ListState& n = g_lists[newTab];
    n.games.swap(g_games); n.img.swap(g_tileImg); n.tried.swap(g_tileTried);
    std::swap(n.cursor, g_cursor); std::swap(n.topRow, g_topRow); std::swap(n.load, g_load); n.err.swap(g_loadErr);
    g_listTab = newTab;
    if ((newTab == TAB_INSTALLED && g_installedStale) || (newTab == TAB_CATALOG && g_catalogStale) || newTab == TAB_REQUESTS) g_load = LS_NONE;   // перечитываем только если что-то могло измениться
}

// Окна: подтверждение установки и ход установки.
enum Mode { MODE_GRID = 0, MODE_CONFIRM, MODE_PROGRESS, MODE_DELETE };
static Mode g_mode = MODE_GRID;
static Mode g_deleteFrom = MODE_GRID;      // откуда открыли подтверждение удаления (сетка или окно игры)
static std::string g_deleteErr;
static Game g_sel;                         // выбранная игра
static std::vector<PkgItem> g_selFiles;    // что будет установлено (по порядку)
static uint64_t g_selNeed = 0, g_freeBytes = 0;
static bool g_freeOk = false;
static uint64_t g_totalBytes = 0;          // полный размер диска (0 - неизвестен)
static std::string g_confirmErr;           // отказ службы при запуске установки
static bool g_instActive = false;          // установка идёт (опрашиваем /status)
static bool g_instFinished = false;        // закончилась (успешно или с ошибкой), окно ещё не закрыто
static InstallStatus g_inst;
static std::string g_instTitle;
static std::vector<PkgItem> g_queuedFiles; // все файлы, отправленные на установку (для подписей)
static std::vector<std::string> g_queuedTitle; // название игры для каждого файла из g_queuedFiles
static int g_queuedGames = 0;

static bool job_active(const InstallJob& j)
{
    return j.status == "queued" || j.status == "registering" || j.status == "installing";
}

// Файлы игры стоят в очереди или ставятся прямо сейчас.
static bool game_queued(const Game& g)
{
    for (size_t j = 0; j < g_inst.jobs.size(); j++) {
        if (!job_active(g_inst.jobs[j])) continue;
        for (size_t f = 0; f < g.files.size(); f++)
            if (g.files[f].path == g_inst.jobs[j].path) return true;
    }
    return false;
}

static uint64_t pending_bytes()
{
    uint64_t n = 0;
    for (size_t j = 0; j < g_inst.jobs.size(); j++)
        if (job_active(g_inst.jobs[j])) n += g_inst.jobs[j].size;
    return n;
}
static uint64_t g_lastPoll = 0;
static bool g_freshScan = false;           // Квадрат: просим у службы свежее сканирование
static int g_cardReason = 0;               // 0 - можно ставить; иначе почему кнопка установки неактивна
enum { CARD_OK = 0, CARD_INSTALLED, CARD_INCOMPLETE, CARD_QUEUED, CARD_LAUNCH, CARD_REQUEST };
static bool g_confirmOk = false;          // сообщение под карточкой - не ошибка (рисуется зелёным)
static bool g_skipConfirmInput = false;   // то же нажатие Креста, что открыло окно, не считается «Установить»

static void mark_installed()
{
    std::vector<std::string> ids;
    if (!service_installed(ids)) return;
    std::vector<Game>& gs = (g_listTab == TAB_AVAILABLE) ? g_games : g_lists[TAB_AVAILABLE].games;
    for (size_t i = 0; i < gs.size(); i++) {
        gs[i].installed = false;
        for (size_t k = 0; k < ids.size(); k++)
            if (!gs[i].title_id.empty() && ids[k] == gs[i].title_id) gs[i].installed = true;
    }
}

static void load_games(bool fresh = false)
{
    std::vector<PkgItem> items;
    std::string err;
    if (service_scan(items, err, fresh)) {
        build_games(items, g_games);
        g_tileImg.clear(); g_tileTried.clear();
        for (size_t i = 0; i < g_games.size(); i++)
            for (size_t k = 0; k < g_games[i].files.size(); k++)
                if (g_games[i].files[k].incomplete) g_games[i].incomplete = true;
        mark_installed();
        if (g_cursor >= (int)g_games.size()) g_cursor = g_games.empty() ? 0 : (int)g_games.size() - 1;
        g_topRow = 0;
        g_load = LS_OK;
        g_loadErr.clear();
    } else {
        g_load = LS_ERROR;
        g_loadErr = err;
        g_games.clear();
    }
    g_status.clear();
}

static void load_installed();

// Подгружаем список установленных игр (и потом их картинки) в фоне, пока открыта другая вкладка.
static void preload_installed()
{
    if (g_listTab != TAB_AVAILABLE) return;
    switch_list(TAB_INSTALLED);
    load_installed();
    switch_list(TAB_AVAILABLE);
}

static void load_catalog_list();
static std::string lower_ascii(std::string s)
{
    for (size_t i = 0; i < s.size(); i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
    return s;
}

// Вкладка «Установленные игры»: список берём у службы, названия - из param.sfo, а если их нет - из каталога.
static void load_installed()
{
    std::vector<std::pair<std::string, std::string> > list;
    if (!service_installed_games(list)) {
        g_load = LS_ERROR;
        g_loadErr = "служба не отвечает";
        g_games.clear();
        return;
    }
    g_games.clear();
    for (size_t i = 0; i < list.size(); i++) {
        Game g;
        g.title_id = list[i].first;
        g.title = list[i].second;
        if (g.title.empty()) {
            const CatEntry* e = catalog_find(g.title_id, "");
            g.title = e && !e->name_en.empty() ? e->name_en : g.title_id;
        }
        g.installed = true;
        g.console = true;
        g_games.push_back(g);
    }
    g_catalogStale = true;   // каталог (без установленных) собирается заново
    for (size_t a = 1; a < g_games.size(); a++)   // по алфавиту (латиница; остальное по коду символа)
        for (size_t b = a; b > 0 && lower_ascii(g_games[b].title) < lower_ascii(g_games[b - 1].title); b--) std::swap(g_games[b], g_games[b - 1]);
    g_tileImg.clear(); g_tileTried.clear();
    if (g_cursor >= (int)g_games.size()) g_cursor = g_games.empty() ? 0 : (int)g_games.size() - 1;
    g_topRow = 0;
    g_load = LS_OK;
    g_installedStale = false;
    g_loadErr.clear();
    logf_("ui: установленных игр: %d", (int)g_games.size());
}

// Вкладка «Каталог игр»: все игры базы, кроме уже установленных (их ищем и по Title ID, и по названию).
static char letter_of(const std::string& t)
{
    if (t.empty()) return '#';
    unsigned char c = (unsigned char)t[0];
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
    return (c >= 'A' && c <= 'Z') ? (char)c : '#';
}

static bool g_letterHas[27];     // какие буквы есть в списке каталога: [0] - «#», [1..26] - A..Z
static bool g_skipDeleteInput = false; // кнопка, открывшая окно удаления, не должна сразу его закрыть
static bool g_stripFocus = false;  // фокус на алфавитной полосе (вкладка «Каталог игр»)
static int letter_index(char c) { return c == '#' ? 0 : (c - 'A' + 1); }

static const char* REQ_PATH = "/data/ps4installer_app/requests.txt";

static void load_catalog_list()
{
    std::vector<std::pair<std::string, std::string> > inst;
    service_installed_games(inst);   // не вышло - ничего страшного, просто покажем все игры
    std::vector<const CatEntry*> skip;
    for (size_t i = 0; i < inst.size(); i++) {
        const CatEntry* e = catalog_find(inst[i].first, inst[i].second);
        if (e) skip.push_back(e);
    }
    g_games.clear();
    int n = catalog_count();
    for (int i = 0; i < n; i++) {
        const CatEntry* e = catalog_at(i);
        if (!e || e->name_en.empty()) continue;
        if (std::find(skip.begin(), skip.end(), e) != skip.end()) continue;
        Game g;
        g.title = e->name_en;
        g.title_id = e->title_id;
        g.listOnly = true;
        g_games.push_back(g);
    }
    std::sort(g_games.begin(), g_games.end(), [](const Game& a, const Game& b) { return lower_ascii(a.title) < lower_ascii(b.title); });
    g_tileImg.clear(); g_tileTried.clear();
    if (g_cursor >= (int)g_games.size()) g_cursor = g_games.empty() ? 0 : (int)g_games.size() - 1;
    g_topRow = 0;
    memset(g_letterHas, 0, sizeof(g_letterHas));
    for (size_t i = 0; i < g_games.size(); i++) g_letterHas[letter_index(letter_of(g_games[i].title))] = true;
    g_stripFocus = false;
    g_load = LS_OK;
    g_catalogStale = false;
    g_loadErr.clear();
    logf_("ui: игр в каталоге для показа: %d (из %d)", (int)g_games.size(), n);
}

// Вкладка «Запрос на игры»: строки файла requests.txt (Title ID, табуляция, название).
static std::vector<std::string> req_read_lines()
{
    std::vector<std::string> out;
    std::string all;
    if (FILE* f = fopen(REQ_PATH, "rb")) {
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
        fclose(f);
    }
    size_t i = 0;
    while (i < all.size()) {
        size_t k = all.find('\n', i);
        if (k == std::string::npos) k = all.size();
        if (k > i) out.push_back(all.substr(i, k - i));
        i = k + 1;
    }
    return out;
}

static void load_requests()
{
    std::vector<std::string> lines = req_read_lines();
    g_games.clear();
    for (size_t i = 0; i < lines.size(); i++) {
        size_t t = lines[i].find('\t');
        Game g;
        g.title_id = t == std::string::npos ? "" : lines[i].substr(0, t);
        g.title = t == std::string::npos ? lines[i] : lines[i].substr(t + 1);
        g.listOnly = true;
        g_games.push_back(g);
    }
    g_tileImg.clear(); g_tileTried.clear();
    if (g_cursor >= (int)g_games.size()) g_cursor = g_games.empty() ? 0 : (int)g_games.size() - 1;
    g_topRow = 0;
    g_load = LS_OK;
    g_loadErr.clear();
    logf_("ui: запросов в списке: %d", (int)g_games.size());
}

// Убирает игру из списка запросов (по Title ID и названию).
static bool req_remove(const Game& g)
{
    std::vector<std::string> lines = req_read_lines();
    std::string drop = g.title_id + "\t" + g.title;
    FILE* f = fopen(REQ_PATH, "wb");
    if (!f) return false;
    for (size_t i = 0; i < lines.size(); i++)
        if (lines[i] != drop) { fputs(lines[i].c_str(), f); fputc('\n', f); }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- рисование

static void blit(const CatImage& im, int x, int y);

// Картинки плиток (обложка из каталога или иконка из PKG): грузятся лениво, держим только видимые ряды.

static bool load_game_image(const Game& g, int w, int h, CatImage& out)
{
    if (g.console) {   // у установленной игры берём её настоящую иконку (не проверено на консоли)
        std::string png;
        if (service_app_icon(g.title_id, png) && catalog_image_from_blob(png, w, h, out)) return true;
    }
    const CatEntry* e = catalog_find(g.title_id, g.title);
    if (e && catalog_image(e->cover, w, h, out)) return true;
    std::string png;
    if (!g.path.empty() && service_icon(g.path, png)) {
        if (catalog_image_from_blob(png, w, h, out)) return true;
        logf_("ui: иконка из PKG не разобрана (%d байт): %s", (int)png.size(), g.title.c_str());
    } else {
        logf_("ui: нет картинки для %s (каталог: %s, иконка из PKG: нет)", g.title.c_str(), e ? "обложка не читается" : "игры нет");
    }
    return false;
}

// ---- Кэш готовых плиток на диске: /data/ps4installer_app/cache. Плитка уже уменьшена и обрезана, читается
// почти мгновенно (без разбора большого JPEG). Название файла включает размер плитки, Title ID и число игр
// в каталоге (новый каталог - новые файлы). Чтобы сбросить кэш, достаточно удалить папку.
static const char* CACHE_DIR = "/data/ps4installer_app/cache";

static std::string tile_cache_path(const Game& g, int w, int h)
{
    if (g.title_id.empty()) return "";
    char b[64];
    snprintf(b, sizeof(b), "/t%dx%d_n%d_", w, h, catalog_count());
    return std::string(CACHE_DIR) + b + g.title_id + (g.console ? "_c" : "_a") + ".raw";
}

static bool tile_cache_read(const std::string& path, int w, int h, CatImage& out)
{
    if (path.empty()) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> rgb((size_t)w * h * 3);
    bool ok = fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
    unsigned char extra;
    ok = ok && fread(&extra, 1, 1, f) == 0;   // файл ровно нужного размера
    fclose(f);
    if (!ok) return false;
    out.w = w; out.h = h;
    out.px.resize((size_t)w * h);
    for (size_t i = 0; i < out.px.size(); i++)
        out.px[i] = ((uint32_t)rgb[i * 3] << 16) | ((uint32_t)rgb[i * 3 + 1] << 8) | rgb[i * 3 + 2];
    return true;
}

static void tile_cache_write(const std::string& path, const CatImage& im)
{
    if (path.empty() || !im.ok()) return;
    mkdir("/data/ps4installer_app", 0777);
    mkdir(CACHE_DIR, 0777);
    std::vector<unsigned char> rgb(im.px.size() * 3);
    for (size_t i = 0; i < im.px.size(); i++) {
        rgb[i * 3] = (unsigned char)(im.px[i] >> 16); rgb[i * 3 + 1] = (unsigned char)(im.px[i] >> 8); rgb[i * 3 + 2] = (unsigned char)im.px[i];
    }
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    fclose(f);
    if (ok) rename(tmp.c_str(), path.c_str()); else remove(tmp.c_str());
}

static void load_img(const Game& g, int W_, int H_, CatImage& out)
{
    static int logged = 0;   // время загрузки: в журнал попадают только первые плитки
    uint64_t t0 = sceKernelGetProcessTime();
    std::string p = tile_cache_path(g, W_, H_);
    if (tile_cache_read(p, W_, H_, out)) {
        if (logged++ < 40) logf_("ui: плитка %s из кэша за %d мс", g.title_id.c_str(), (int)((sceKernelGetProcessTime() - t0) / 1000));
        return;
    }
    if (load_game_image(g, W_, H_, out)) tile_cache_write(p, out);
    if (logged++ < 40) logf_("ui: плитка %s загружена за %d мс (с записью в кэш)", g.title_id.c_str(), (int)((sceKernelGetProcessTime() - t0) / 1000));
}

static void load_tile(const Game& g, CatImage& out) { load_img(g, TILE_W, IMG_H, out); }

// Список запросов: маленькие иконки (в 3 раза меньше обложки в карточке).
static const int REQ_ICON = 96, REQ_ROW_H = 108, REQ_ROWS_VIS = 8;

static bool request_images_step()
{
    int n = (int)g_games.size();
    if ((int)g_tileImg.size() != n) { g_tileImg.assign((size_t)n, CatImage()); g_tileTried.assign((size_t)n, 0); }
    uint64_t t0 = sceKernelGetProcessTime();
    bool any = false;
    for (int i = g_topRow; i < g_topRow + REQ_ROWS_VIS && i < n; i++) {
        if (g_tileTried[(size_t)i]) continue;
        g_tileTried[(size_t)i] = 1;
        load_img(g_games[(size_t)i], REQ_ICON, REQ_ICON, g_tileImg[(size_t)i]);
        any = true;
        if (sceKernelGetProcessTime() - t0 > 40000) break;
    }
    return any;
}

// За один проход грузим не больше одной картинки (чтобы кнопки не «залипали»). true - что-то загрузили.
static bool tile_images_step()
{
    if (g_tab == TAB_REQUESTS) return request_images_step();
    int n = (int)g_games.size();
    if ((int)g_tileImg.size() != n) { g_tileImg.assign((size_t)n, CatImage()); g_tileTried.assign((size_t)n, 0); }
    // Загруженные картинки хранятся до закрытия программы (прокрутка вверх-вниз не грузит их заново).
    // Чтобы не съесть память (одна плитка ~450 КБ), держим не больше MAX_TILE_IMAGES: лишние, самые далёкие
    // от текущего ряда, выбрасываем.
    const int MAX_TILE_IMAGES = 48;
    int loaded = 0;
    for (int i = 0; i < n; i++) if (g_tileImg[(size_t)i].ok()) loaded++;
    while (loaded > MAX_TILE_IMAGES) {
        int far = -1, farDist = -1, center = (g_topRow + 1) * COLS;
        for (int i = 0; i < n; i++) {
            if (!g_tileImg[(size_t)i].ok()) continue;
            int d = i > center ? i - center : center - i;
            if (d > farDist) { farDist = d; far = i; }
        }
        if (far < 0) break;
        g_tileImg[(size_t)far] = CatImage();
        g_tileTried[(size_t)far] = 0;
        loaded--;
    }
    // Грузим картинки видимых рядов, пока не истёк небольшой запас времени на кадр (кнопки не «залипают»):
    // из кэша на диске успевает несколько штук за проход, разбор большого JPEG - обычно одна.
    uint64_t t0 = sceKernelGetProcessTime();
    bool any = false;
    int vFirst = g_topRow * COLS, vLast = (g_topRow + ROWS_VIS) * COLS;
    for (int i = vFirst; i < vLast && i < n; i++) {
        if (g_tileTried[(size_t)i]) continue;
        g_tileTried[(size_t)i] = 1;
        load_tile(g_games[(size_t)i], g_tileImg[(size_t)i]);
        any = true;
        if (sceKernelGetProcessTime() - t0 > 40000) break;
    }
    return any;
}

// ---- Крутилка загрузки: 12 точек по кругу, самая яркая «бежит». Пока она на экране, кадр перерисовывается (анимация).
static bool g_spinnerSeen = false;
static uint64_t g_lastSpin = 0;

static Color mixf(Color fg, Color bg, double t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    Color c;
    c.r = (uint8_t)(bg.r + (fg.r - bg.r) * t);
    c.g = (uint8_t)(bg.g + (fg.g - bg.g) * t);
    c.b = (uint8_t)(bg.b + (fg.b - bg.b) * t);
    return c;
}

static void draw_spinner(int cx, int cy, int radius, Color bg)
{
    g_spinnerSeen = true;
    const int N = 12;
    const double PI = 3.14159265358979;
    int phase = (int)((sceKernelGetProcessTime() / 90000) % N);
    double dotR = radius * 0.15, ringR = radius * 0.8;
    Color fg = { 120, 165, 255 };
    for (int k = 0; k < N; k++) {
        double ang = 2 * PI * k / N - PI / 2;
        double px = cx + cos(ang) * ringR, py = cy + sin(ang) * ringR;
        int age = (phase - k + N) % N;                  // 0 - самая яркая точка, дальше тускнеют
        double bright = 1.0 - age * 0.075 - (age > 0 ? 0.08 : 0);
        for (int yy = (int)(py - dotR - 1); yy <= (int)(py + dotR + 1); yy++)
            for (int xx = (int)(px - dotR - 1); xx <= (int)(px + dotR + 1); xx++) {
                double d = sqrt((xx + 0.5 - px) * (xx + 0.5 - px) + (yy + 0.5 - py) * (yy + 0.5 - py));
                double cov = dotR + 0.5 - d;            // гладкий край
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                if (xx < 0 || yy < 0 || xx >= FRAME_WIDTH || yy >= FRAME_HEIGHT) continue;
                g_scene->PutPixel(xx, yy, mixf(fg, bg, bright * cov));
            }
    }
}

// Крутилка по центру области под шапкой, под ней надпись.
static void draw_loading(const std::string& text, const std::string& sub = "")
{
    int cx = FRAME_WIDTH / 2, cy = (HEAD_H + FRAME_HEIGHT - 100) / 2 - 30;
    draw_spinner(cx, cy, 55, C_BG);
    draw_text(text, cx - text_w(text) / 2, cy + 55 + 60, C_TEXT, C_BG);
    if (!sub.empty()) draw_text(sub, cx - text_w(sub) / 2, cy + 55 + 110, C_DIM, C_BG);
}

static void draw_tile(int x, int y, const Game& g, bool selected, const CatImage* img, bool pending = false)
{
    Color bg = selected ? C_TILE_SEL : C_TILE;
    if (selected) fill(x - 5, y - 5, TILE_W + 10, TILE_H + 10, C_TAB_ON);
    fill(x, y, TILE_W, TILE_H, bg);

    // Пока без обложки: цветной квадрат по title_id и первая буква названия.
    unsigned h = 0;
    for (size_t i = 0; i < g.title_id.size(); i++) h = h * 31 + (unsigned char)g.title_id[i];
    Color c = { (uint8_t)(50 + (h & 0x3F)), (uint8_t)(60 + ((h >> 6) & 0x3F)), (uint8_t)(80 + ((h >> 12) & 0x5F)) };
    fill(x, y, TILE_W, IMG_H, c);
    if (img && img->ok()) {
        blit(*img, x, y);
    } else if (pending) {   // картинка ещё грузится
        draw_spinner(x + TILE_W / 2, y + IMG_H / 2, 38, c);
    } else {
        std::string letter;
        const unsigned char* p = (const unsigned char*)g.title.c_str();
        const unsigned char* st = p;
        if (*p) { decode_utf8(p); letter = std::string((const char*)st, (size_t)(p - st)); }
        draw_text(letter, x + (TILE_W - g_cellW) / 2, y + IMG_H / 2 + FONT_PX / 3, C_TEXT, c);
    }

    // Размер игры в углу картинки
    if (!g.console && !g.listOnly) {
        std::string sz = fmt_size(g.size);
        int szw = text_w(sz) + 12;
        fill(x + TILE_W - szw, y + IMG_H - FONT_PX - 10, szw, FONT_PX + 10, C_BG);
        draw_text(sz, x + TILE_W - szw + 6, y + IMG_H - 9, C_DIM, C_BG);
    }

    // Уже установлена: полоса поверх картинки
    if (g.installed && !g.console && !g.listOnly) {
        std::string m = "УЖЕ УСТАНОВЛЕНО";
        int w = text_w(m) + 16;
        int mx = x + (TILE_W - w) / 2, my = y + IMG_H / 2 - (FONT_PX + 16) / 2;
        fill(mx, my, w, FONT_PX + 16, C_GREEN);
        draw_text(m, mx + 8, my + FONT_PX + 4, C_BG, C_GREEN);
    }

    if (g.incomplete && !g.installed) {
        std::string m = "НЕ ДОКАЧАНО";
        int w = text_w(m) + 16;
        int mx = x + (TILE_W - w) / 2, my = y + IMG_H / 2 - (FONT_PX + 16) / 2;
        fill(mx, my, w, FONT_PX + 16, C_RED_BG);
        draw_text(m, mx + 8, my + FONT_PX + 4, C_TEXT, C_RED_BG);
    } else if (!g.installed && game_queued(g)) {
        std::string m = "В ОЧЕРЕДИ";
        int w = text_w(m) + 16;
        int mx = x + (TILE_W - w) / 2, my = y + IMG_H / 2 - (FONT_PX + 16) / 2;
        fill(mx, my, w, FONT_PX + 16, C_WARN);
        draw_text(m, mx + 8, my + FONT_PX + 4, C_BG, C_WARN);
    }

    // Пометки: патч / DLC / русификатор
    std::string tags;
    if (g.patches) tags += "патч ";
    if (g.dlcs) tags += "DLC ";
    if (g.rus) tags += "рус ";
    if (!tags.empty() && !g.console && !g.listOnly) {
        int w = text_w(tags) + 12;
        fill(x, y, w, FONT_PX + 10, C_BG);
        draw_text(tags, x + 6, y + FONT_PX + 1, C_WARN, C_BG);
    }

    int maxc = (TILE_W - 16) / g_cellW;
    std::vector<std::string> lines = wrap_lines(g.title, maxc, 2);
    for (size_t i = 0; i < lines.size(); i++)
        draw_text(lines[i], x + 8, y + IMG_H + 30 + (int)i * (FONT_PX + 4), C_TEXT, bg);
}

static void draw_service_message()
{
    int y = GRID_TOP + 60;
    if (g_svc == SVC_CHECK) {
        draw_loading("Проверка службы установки...");
    } else if (g_svc == SVC_STARTING) {
        draw_loading("Запуск службы установки...", "Это занимает несколько секунд.");
    } else if (g_svc == SVC_NO_LOADER) {
        draw_text("Не удалось запустить службу: загрузчик payload'ов выключен.", MARGIN_X, y, C_WARN, C_BG);
        draw_text("Включите в GoldHEN: Servers settings - Enable Payloader Server.", MARGIN_X, y + 50, C_TEXT, C_BG);
        draw_hint("Затем нажмите {S}.", MARGIN_X, y + 100, C_DIM, C_BG);
    } else {
        draw_text("Служба установки не запустилась.", MARGIN_X, y, C_WARN, C_BG);
        draw_hint("Подождите немного и нажмите {S}. Если не помогает, перезапустите GoldHEN.", MARGIN_X, y + 50, C_DIM, C_BG);
    }
}

static int grid_top();
static void draw_letter_strip();

static void draw_available()
{
    if (g_load == LS_LOADING || g_load == LS_NONE) {
        draw_loading("Загрузка списка игр...");
        return;
    }
    if (g_load == LS_ERROR) {
        draw_text("Служба установки не отвечает.", MARGIN_X, GRID_TOP + 60, C_WARN, C_BG);
        draw_hint("Запустите ps4installerd (адрес 127.0.0.1, порт 8284) и нажмите {S}.", MARGIN_X, GRID_TOP + 110, C_DIM, C_BG);
        draw_text("Причина: " + g_loadErr, MARGIN_X, GRID_TOP + 160, C_DIM, C_BG);
        return;
    }
    if (g_games.empty()) {
        if (g_tab == TAB_INSTALLED) {
            draw_text("Установленных игр не найдено.", MARGIN_X, GRID_TOP + 60, C_TEXT, C_BG);
            draw_hint("Нажмите {S}, чтобы обновить список.", MARGIN_X, GRID_TOP + 110, C_DIM, C_BG);
        } else if (g_tab == TAB_CATALOG) {
            draw_text("Каталог игр не найден или пуст.", MARGIN_X, GRID_TOP + 60, C_TEXT, C_BG);
        } else {
            draw_text("Игр не найдено.", MARGIN_X, GRID_TOP + 60, C_TEXT, C_BG);
            draw_hint("Подключите флешку или диск с играми и нажмите {S}.", MARGIN_X, GRID_TOP + 110, C_DIM, C_BG);
        }
        return;
    }
    int gapX = (FRAME_WIDTH - 2 * MARGIN_X - COLS * TILE_W) / (COLS - 1);
    for (int r = 0; r < ROWS_VIS; r++) {
        for (int c = 0; c < COLS; c++) {
            int idx = (g_topRow + r) * COLS + c;
            if (idx >= (int)g_games.size()) break;
            draw_tile(MARGIN_X + c * (TILE_W + gapX), grid_top() + r * (TILE_H + GAP_Y), g_games[(size_t)idx], idx == g_cursor,
                      (size_t)idx < g_tileImg.size() ? &g_tileImg[(size_t)idx] : nullptr,
                      (size_t)idx >= g_tileTried.size() || !g_tileTried[(size_t)idx]);
        }
    }
    if (g_tab == TAB_CATALOG) draw_letter_strip();
}

static int grid_top() { return g_tab == TAB_CATALOG ? HEAD_H + 70 : GRID_TOP; }   // в каталоге под шапкой алфавитная полоса

static void draw_letter_strip()
{
    const int y = HEAD_H + 8, h = 54, gap = 6;
    const int cell = (FRAME_WIDTH - 2 * MARGIN_X - 26 * gap) / 27;   // на всю ширину экрана
    char cur = g_games.empty() ? '#' : letter_of(g_games[(size_t)g_cursor].title);
    for (int i = 0; i < 27; i++) {
        char ch = i == 0 ? '#' : (char)('A' + i - 1);
        bool sel = ch == cur;
        Color bg = sel ? (g_stripFocus ? C_BAR_NEED : C_TAB_ON) : C_TAB_OFF;
        int x = MARGIN_X + i * (cell + gap);
        if (sel) fill(x - 4, y - 4, cell + 8, h + 8, C_TEXT);   // белая рамка вокруг текущей буквы
        fill(x, y, cell, h, bg);
        std::string t(1, ch);
        Color fg = sel ? (g_stripFocus ? C_BG : C_TEXT) : (g_letterHas[i] ? C_TEXT : C_DIM);
        draw_text(t, x + (cell - g_cellW) / 2, y + 38, fg, bg);
    }
}

static void draw_requests()
{
    if (g_load == LS_LOADING || g_load == LS_NONE) { draw_loading("Загрузка списка запросов..."); return; }
    int top = HEAD_H + 30, w = FRAME_WIDTH - 2 * MARGIN_X;
    if (g_games.empty()) {
        draw_text("Запросов пока нет.", MARGIN_X, top + 40, C_TEXT, C_BG);
        draw_hint("Откройте игру во вкладке «Каталог игр» и нажмите {X} - запросить.", MARGIN_X, top + 90, C_DIM, C_BG);
        return;
    }
    for (int r = 0; r < REQ_ROWS_VIS; r++) {
        int idx = g_topRow + r;
        if (idx >= (int)g_games.size()) break;
        int y = top + r * REQ_ROW_H, rh = REQ_ROW_H - 10;
        bool sel = idx == g_cursor;
        Color bg = sel ? C_TILE_SEL : C_TILE;
        if (sel) fill(MARGIN_X - 4, y - 4, w + 8, rh + 8, C_TAB_ON);
        fill(MARGIN_X, y, w, rh, bg);
        int ix = MARGIN_X + 10, iy = y + (rh - REQ_ICON) / 2;
        const Game& g = g_games[(size_t)idx];
        if ((size_t)idx < g_tileImg.size() && g_tileImg[(size_t)idx].ok()) blit(g_tileImg[(size_t)idx], ix, iy);
        else {
            fill(ix, iy, REQ_ICON, REQ_ICON, C_PANEL);
            if ((size_t)idx >= g_tileTried.size() || !g_tileTried[(size_t)idx]) draw_spinner(ix + REQ_ICON / 2, iy + REQ_ICON / 2, 26, C_PANEL);
        }
        int tx = ix + REQ_ICON + 28;
        int idw = text_w(g.title_id) + 20;
        draw_text(fit_chars(g.title, (w - REQ_ICON - 28 - idw - 60) / g_cellW), tx, y + rh / 2 + FONT_PX / 3, C_TEXT, bg);
        draw_text(g.title_id, MARGIN_X + w - 20 - text_w(g.title_id), y + rh / 2 + FONT_PX / 3, C_DIM, bg);
    }
}

static void draw_panel(int x, int y, int w, int h)
{
    fill(x - 4, y - 4, w + 8, h + 8, C_EDGE);
    fill(x, y, w, h, C_PANEL);
}

static void draw_bar(int x, int y, int w, int h, int pct, Color c)
{
    fill(x, y, w, h, C_BAR_BG);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fill(x, y, w * pct / 100, h, c);
}

static std::string job_game(const InstallJob& j)
{
    for (size_t i = 0; i < g_queuedFiles.size() && i < g_queuedTitle.size(); i++)
        if (g_queuedFiles[i].path == j.path) return g_queuedTitle[i];
    return "";
}

static std::string job_label(const InstallJob& j)
{
    for (size_t i = 0; i < g_queuedFiles.size(); i++)
        if (g_queuedFiles[i].path == j.path) return file_label(g_queuedFiles[i]);
    return j.kind;
}

// ---- Информация об игре из каталога (обложка, 4 скриншота, описание) ----
static const int COVER_W = 350, COVER_H = 350;
static const int COMPACT_COVER = 420;   // обложка в узкой карточке без описания
static const int SHOT_W = 254, SHOT_H = 143, SHOT_GAP = 14, SHOT_N = 4;
static const CatEntry* g_selCat = nullptr;
static CatImage g_imgCover;
static CatImage g_imgShots[SHOT_N];
static std::string g_selDesc;

static void info_clear()
{
    g_selCat = nullptr;
    g_imgCover = CatImage();
    for (int i = 0; i < SHOT_N; i++) g_imgShots[i] = CatImage();
    g_selDesc.clear();
}

static void info_load(const Game& g)
{
    info_clear();
    const char* how = "";
    g_selCat = catalog_find(g.title_id, g.title, &how);
    if (!g_selCat) {
        logf_("ui: в каталоге нет %s (%s)", g.title_id.c_str(), g.title.c_str());
        load_game_image(g, COMPACT_COVER, COMPACT_COVER, g_imgCover);   // хотя бы иконка из самого PKG (описания нет - карточка узкая)
        return;
    }
    logf_("ui: найдено в каталоге по %s: %s", how, g_selCat->name_en.c_str());
    catalog_image(g_selCat->cover, COVER_W, COVER_H, g_imgCover);
    for (int i = 0; i < SHOT_N && i < (int)g_selCat->shots.size(); i++)
        catalog_image(g_selCat->shots[(size_t)i], SHOT_W, SHOT_H, g_imgShots[i]);
    if (!g_imgCover.ok()) load_game_image(g, COVER_W, COVER_H, g_imgCover);
    g_selDesc = catalog_text(*g_selCat);
    bool anyShot = false;
    for (int i = 0; i < SHOT_N; i++) anyShot = anyShot || g_imgShots[i].ok();
    if (g_selDesc.empty() && !anyShot) {   // карточка без описания: иконка крупнее
        g_imgCover = CatImage();
        if (!catalog_image(g_selCat->cover, COMPACT_COVER, COMPACT_COVER, g_imgCover)) load_game_image(g, COMPACT_COVER, COMPACT_COVER, g_imgCover);
    }
}

static void blit(const CatImage& im, int x, int y)
{
    if (!im.ok()) return;
    for (int yy = 0; yy < im.h; yy++) {
        int py = y + yy;
        if (py < 0 || py >= FRAME_HEIGHT) continue;
        for (int xx = 0; xx < im.w; xx++) {
            int px = x + xx;
            if (px < 0 || px >= FRAME_WIDTH) continue;
            uint32_t v = im.px[(size_t)yy * im.w + xx];
            Color c = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
            g_scene->PutPixel(px, py, c);
        }
    }
}

// Название игры размером TITLE_PX; если не влезает в maxw - обрезается с «…».
static void draw_title_fit(const std::string& en, int x, int baseline, int maxw, bool center, Color bg)
{
    FT_Set_Pixel_Sizes(g_font, 0, TITLE_PX);
    int cell = FT_Load_Char(g_font, 'M', FT_LOAD_DEFAULT) == 0 ? (int)(g_font->glyph->advance.x >> 6) : g_cellW * TITLE_PX / FONT_PX;
    if (cell < 1) cell = 1;
    std::string t = fit_chars(en, maxw / cell);   // не влезло - обрезаем и ставим «…», размер не меняем
    draw_text(t, center ? x + (maxw - count_cp(t) * cell) / 2 : x, baseline, C_TEXT, bg);
    FT_Set_Pixel_Sizes(g_font, 0, FONT_PX);
}

// Верхняя часть окна игры: слева обложка, справа названия, ряд скриншотов и описание.
static void draw_info_top(const Game& g, int x, int y, int W)
{
    int cx = x + 30, cy = y + 30;
    if (g_imgCover.ok()) blit(g_imgCover, cx, cy);
    else {
        fill(cx, cy, COVER_W, COVER_H, C_TILE);
        std::string l = g.title.empty() ? "?" : g.title.substr(0, 1);
        const unsigned char* p = (const unsigned char*)g.title.c_str();
        const unsigned char* st = p;
        if (*p) { decode_utf8(p); l = std::string((const char*)st, (size_t)(p - st)); }
        draw_text(l, cx + (COVER_W - g_cellW) / 2, cy + COVER_H / 2 + FONT_PX / 3, C_DIM, C_TILE);
    }
    int rx = x + 30 + COVER_W + 30, rw = W - (rx - x) - 30;
    int maxc = rw / g_cellW;
    std::string en = g_selCat && !g_selCat->name_en.empty() ? g_selCat->name_en : g.title;
    // Название игры крупнее остального текста (шрифт моноширинный: ширина буквы растёт вместе с размером).
    draw_title_fit(en, rx, cy + TITLE_PX, rw, false, C_PANEL);
    bool hasRu = g_selCat && !g_selCat->name_ru.empty() && g_selCat->name_ru != en;
    if (hasRu) draw_text(fit_chars(g_selCat->name_ru, maxc), rx, cy + TITLE_PX + FONT_PX + 10, C_DIM, C_PANEL);
    // Без второй строки скриншоты ближе к названию (расстояние до них примерно как от названия до верха карточки).
    int sy = hasRu ? cy + TITLE_PX + FONT_PX + 26 : cy + TITLE_PX + 45;
    for (int i = 0; i < SHOT_N; i++) {
        int sx = rx + i * (SHOT_W + SHOT_GAP);
        if (g_imgShots[i].ok()) blit(g_imgShots[i], sx, sy);
        else if (g_selCat) fill(sx, sy, SHOT_W, SHOT_H, C_TILE);
    }
    // Описание мельче основного текста (26 вместо 30), влезает больше строк.
    FT_Set_Pixel_Sizes(g_font, 0, DESC_PX);
    int dcell = FT_Load_Char(g_font, 'M', FT_LOAD_DEFAULT) == 0 ? (int)(g_font->glyph->advance.x >> 6) : g_cellW * DESC_PX / FONT_PX;
    if (dcell < 1) dcell = 1;
    int dy = sy + SHOT_H + 14 + DESC_PX;
    std::vector<std::string> d = wrap_lines(g_selDesc, rw / dcell, 4);
    for (size_t i = 0; i < d.size(); i++) draw_text(d[i], rx, dy + (int)i * (DESC_PX + 6), C_TEXT, C_PANEL);
    FT_Set_Pixel_Sizes(g_font, 0, FONT_PX);
}

// Полоска занятого места шириной w: серая основа, занято - синее, нужно для игры - оранжевое и толще, дальше свободно -
// зеленоватое. Под ней подписи. Возвращает занятую высоту.
static const Color C_BAR_TRACK = { 95, 100, 112 };
static const Color C_BAR_USED  = { 70, 130, 230 };
static const Color C_BAR_NEED  = { 255, 150, 40 };
static const Color C_BAR_FREE  = { 120, 200, 150 };

static int draw_space_info(int x, int y, int w, bool center)
{
    const int needH = 24, barH = 12;
    int bt = y + (needH - barH) / 2;
    if (g_freeOk && g_totalBytes > 0) {
        double total = (double)g_totalBytes;
        double usedF = total - (double)g_freeBytes;
        if (usedF < 0) usedF = 0;
        int usedW = (int)(w * usedF / total);
        double needAvail = (double)g_selNeed < (double)g_freeBytes ? (double)g_selNeed : (double)g_freeBytes;
        int needW = needAvail > 0 ? (int)(w * needAvail / total) : 0;
        if (g_selNeed > 0 && needW < 4) needW = 4;
        if (usedW + needW > w) usedW = w - needW;
        fill(x, bt, w, barH, C_BAR_TRACK);
        fill(x, bt, usedW, barH, C_BAR_USED);
        fill(x + usedW + needW, bt, w - usedW - needW, barH, C_BAR_FREE);
        fill(x + usedW, y, needW, needH, C_BAR_NEED);
    }
    FT_Set_Pixel_Sizes(g_font, 0, DESC_PX);
    int cell = FT_Load_Char(g_font, 'M', FT_LOAD_DEFAULT) == 0 ? (int)(g_font->glyph->advance.x >> 6) : g_cellW * DESC_PX / FONT_PX;
    int ly = y + needH + 10 + DESC_PX;
    std::string a = "Нужно: " + fmt_size(g_selNeed);
    std::string b = g_freeOk ? "Свободно: " + fmt_size(g_freeBytes) : "Свободное место узнать не удалось";
    draw_text(a, center ? x + (w - count_cp(a) * cell) / 2 : x, ly, C_BAR_NEED, C_PANEL);
    draw_text(b, center ? x + (w - count_cp(b) * cell) / 2 : x, ly + DESC_PX + 8, g_freeOk ? C_BAR_FREE : C_WARN, C_PANEL);
    FT_Set_Pixel_Sizes(g_font, 0, FONT_PX);
    return needH + 10 + 2 * (DESC_PX + 8);
}

// Нет ни описания, ни скриншотов: окно почти квадратное. Название сверху, под ним иконка, ниже сведения и кнопки столбиком.
static bool card_is_compact()
{
    if (!g_selDesc.empty()) return false;
    for (int i = 0; i < SHOT_N; i++) if (g_imgShots[i].ok()) return false;
    return true;
}

static void draw_confirm_compact()
{
    const int W = 1180;
    bool launch = g_cardReason == CARD_LAUNCH, request = g_cardReason == CARD_REQUEST;
    bool low = g_cardReason == CARD_OK && g_freeOk && g_selNeed > g_freeBytes;
    bool can = g_cardReason == CARD_OK && !low;
    std::string hint;   // все кнопки в одну строку
    if (launch) hint = "{X} - запустить игру    {T} - удалить    {O} - закрыть";
    else if (request) hint = "{X} - запросить    {O} - закрыть";
    else {
        hint = can ? "{X} - установить    {O} - закрыть" : "{X} - установить (недоступно)    {O} - закрыть";
        if (g_cardReason == CARD_QUEUED) hint += "    {T} - ход установки";
    }
    int H = 0;
    for (int pass = 0; pass < 2; pass++) {
        bool draw = pass == 1;
        int x = (FRAME_WIDTH - W) / 2, y = HEAD_H + (FRAME_HEIGHT - 100 - HEAD_H - H) / 2;
        int cur = y + 28;
        if (draw) { draw_panel(x, y, W, H); draw_title_fit(g_selCat && !g_selCat->name_en.empty() ? g_selCat->name_en : g_sel.title, x + 30, cur + TITLE_PX, W - 60, true, C_PANEL); }
        cur += TITLE_PX + 26;
        int cw = g_imgCover.ok() ? g_imgCover.w : COMPACT_COVER, ch = g_imgCover.ok() ? g_imgCover.h : COMPACT_COVER;
        int cx = x + (W - cw) / 2;
        if (draw) {
            if (g_imgCover.ok()) blit(g_imgCover, cx, cur);
            else {
                fill(cx, cur, cw, ch, C_TILE);
                std::string l = g_sel.title.empty() ? "?" : g_sel.title.substr(0, 1);
                const unsigned char* p = (const unsigned char*)g_sel.title.c_str();
                const unsigned char* st = p;
                if (*p) { decode_utf8(p); l = std::string((const char*)st, (size_t)(p - st)); }
                draw_text(l, cx + (cw - g_cellW) / 2, cur + ch / 2 + FONT_PX / 3, C_DIM, C_TILE);
            }
        }
        cur += ch + 26;
        if (!launch && !request) {
            if (draw) draw_space_info(cx, cur, cw, true);
            cur += 24 + 10 + 2 * (DESC_PX + 8) + 18;
            const char* msg = g_cardReason == CARD_INSTALLED ? "УЖЕ УСТАНОВЛЕНО" : g_cardReason == CARD_INCOMPLETE ? "НЕ ДОКАЧАНО" :
                              g_cardReason == CARD_QUEUED ? "УЖЕ В ОЧЕРЕДИ" : low ? "МАЛО МЕСТА - УДАЛИТЕ НЕНУЖНУЮ ИГРУ" : "";
            if (msg[0]) {
                Color bg = g_cardReason == CARD_INCOMPLETE || low ? C_RED_BG : g_cardReason == CARD_INSTALLED ? C_GREEN : C_WARN;
                Color fg = (g_cardReason == CARD_INCOMPLETE || low) ? C_TEXT : C_BG;
                std::string m = fit_chars(msg, (W - 80) / g_cellW);
                if (draw) { fill(x + 30, cur, W - 60, 52, bg); draw_text(m, x + (W - text_w(m)) / 2, cur + 36, fg, bg); }
                cur += 52 + 18;
            }
        }
        if (!g_confirmErr.empty()) {
            std::string m = fit_chars(g_confirmErr, (W - 60) / g_cellW);
            if (draw) draw_text(m, x + (W - text_w(m)) / 2, cur + FONT_PX, g_confirmOk ? C_GREEN : C_WARN, C_PANEL);
            cur += FONT_PX + 22;
        }
        if (draw) draw_hint(hint, x + (W - hint_w(hint)) / 2, cur + ICON_S - 6, (launch || request || can) ? C_TEXT : C_DIM, C_PANEL);
        cur += ICON_S + 26;
        if (!draw) H = cur - y;
    }
}

static void draw_confirm()
{
    if (card_is_compact()) { draw_confirm_compact(); return; }
    const int W = 1500;   // 30 + обложка 350 + 30 + 4 скриншота с промежутками (4*254+3*14) + 30 = 1478
    const int H = (g_cardReason == CARD_LAUNCH || g_cardReason == CARD_REQUEST) ? 520 : 590;
    // по центру между шапкой и нижней строкой подсказок
    int x = (FRAME_WIDTH - W) / 2, y = HEAD_H + (FRAME_HEIGHT - 100 - HEAD_H - H) / 2;
    draw_panel(x, y, W, H);
    draw_info_top(g_sel, x, y, W);
    int tx = x + 40, ty = y + 30 + COVER_H + 60;
    int maxc = (W - 80) / g_cellW;

    if (g_cardReason == CARD_LAUNCH) {   // окно установленной игры: только запуск
        if (!g_confirmErr.empty()) draw_text(fit_chars(g_confirmErr, maxc), tx, y + H - 130 + 36, C_WARN, C_PANEL);
        std::string h = "{X} - запустить игру      {T} - удалить      {O} - закрыть";
        draw_hint(h, x + (W - hint_w(h)) / 2, y + H - 36, C_TEXT, C_PANEL);
        return;
    }

    if (g_cardReason == CARD_REQUEST) {   // игра из каталога: запросить
        if (!g_confirmErr.empty()) draw_text(fit_chars(g_confirmErr, maxc), tx, y + H - 130 + 36, g_confirmOk ? C_GREEN : C_WARN, C_PANEL);
        std::string h = "{X} - запросить      {O} - закрыть";
        draw_hint(h, x + (W - hint_w(h)) / 2, y + H - 36, C_TEXT, C_PANEL);
        return;
    }

    // Под обложкой: полоска места и подписи; справа от неё - причина, почему нельзя ставить, или предупреждение.
    int sx = x + 30, sy = y + 30 + COVER_H + 24;
    draw_space_info(sx, sy, COVER_W, false);
    bool low = g_cardReason == CARD_OK && g_freeOk && g_selNeed > g_freeBytes;
    int bx = x + 30 + COVER_W + 30, bw = W - (bx - x) - 30, by = sy;
    if (g_cardReason != CARD_OK) {
        const char* msg = g_cardReason == CARD_INSTALLED ? "УЖЕ УСТАНОВЛЕНО" :
                          g_cardReason == CARD_INCOMPLETE ? "НЕ ДОКАЧАНО - ДОЖДИТЕСЬ ЗАГРУЗКИ ФАЙЛОВ" : "УЖЕ В ОЧЕРЕДИ НА УСТАНОВКУ";
        Color bg = g_cardReason == CARD_INCOMPLETE ? C_RED_BG : g_cardReason == CARD_INSTALLED ? C_GREEN : C_WARN;
        Color fg = g_cardReason == CARD_INCOMPLETE ? C_TEXT : C_BG;
        fill(bx, by, bw, 52, bg);
        std::string m = msg;
        draw_text(m, bx + (bw - text_w(m)) / 2, by + 36, fg, bg);
    } else if (low) {
        fill(bx, by, bw, 52, C_RED_BG);
        std::string msg = "МАЛО МЕСТА - УДАЛИТЕ НЕНУЖНУЮ ИГРУ";
        draw_text(msg, bx + (bw - text_w(msg)) / 2, by + 36, C_TEXT, C_RED_BG);
    } else if (!g_confirmErr.empty()) {
        draw_text(fit_chars(g_confirmErr, bw / g_cellW), bx, by + 36, C_WARN, C_PANEL);
    }
    bool can = g_cardReason == CARD_OK && !low;
    std::string hint = can ? "{X} - установить      {O} - закрыть" : "{X} - установить (недоступно)      {O} - закрыть";
    if (g_cardReason == CARD_QUEUED) hint += "      {T} - ход установки";
    draw_hint(hint, x + (W - hint_w(hint)) / 2, y + H - 36, can ? C_TEXT : C_DIM, C_PANEL);
}

static void draw_delete()
{
    const int W = 1200, H = 360;
    int x = (FRAME_WIDTH - W) / 2, y = (FRAME_HEIGHT - H) / 2;
    draw_panel(x, y, W, H);
    int maxc = (W - 80) / g_cellW;
    std::string q = g_tab == TAB_REQUESTS ? "ТОЧНО УДАЛИТЬ ИГРУ ИЗ СПИСКА ЗАПРОСОВ?" : "ТОЧНО УДАЛИТЬ ВЫБРАННУЮ ИГРУ?";
    draw_text(q, x + (W - text_w(q)) / 2, y + 80, C_TEXT, C_PANEL);
    std::string t = fit_chars(g_sel.title, maxc);
    fill(x + 40, y + 120, W - 80, FONT_PX + 24, C_RED_BG);
    draw_text(t, x + (W - text_w(t)) / 2, y + 120 + FONT_PX + 8, C_TEXT, C_RED_BG);
    if (!g_deleteErr.empty()) draw_text(fit_chars(g_deleteErr, maxc), x + 40, y + 240, C_WARN, C_PANEL);
    std::string h = "{X} - да      {O} - НЕТ";
    draw_hint(h, x + (W - hint_w(h)) / 2, y + H - 50, C_TEXT, C_PANEL);
}

static int overall_pct()
{
    uint64_t total = 0, done = 0;
    for (size_t i = 0; i < g_inst.jobs.size(); i++) {
        const InstallJob& j = g_inst.jobs[i];
        total += j.size;
        if (j.status == "done" || j.status == "already_installed") done += j.size;
        else if (j.status == "installing" && j.length > 0) done += (uint64_t)((double)j.size * (double)j.transferred / (double)j.length);
    }
    return total ? (int)(done * 100 / total) : 0;
}

static void draw_progress()
{
    const int W = 1300, H = 700;
    int x = (FRAME_WIDTH - W) / 2, y = HEAD_H + 40;
    draw_panel(x, y, W, H);
    int tx = x + 40, ty = y + 56;
    int maxc = (W - 80) / g_cellW;
    draw_text(fit_chars(g_instTitle, maxc), tx, ty, C_TEXT, C_PANEL);
    ty += FONT_PX + 24;

    bool failed = false;
    for (size_t i = 0; i < g_inst.jobs.size(); i++)
        if (g_inst.jobs[i].status == "failed") failed = true;
    int pct = overall_pct();
    if (g_instFinished) pct = failed ? pct : 100;
    draw_bar(tx, ty, W - 80, 36, pct, failed ? C_WARN : C_GREEN);
    char pc[16];
    snprintf(pc, sizeof(pc), "%d%%", pct);
    draw_text(pc, x + W / 2 - text_w(pc) / 2, ty + 28, C_TEXT, C_BAR_BG);
    ty += 36 + 40;

    std::string lastGame = "\x01";
    size_t shown = 0;
    for (size_t i = 0; i < g_inst.jobs.size(); i++) {
        const InstallJob& j = g_inst.jobs[i];
        std::string gt = job_game(j);
        bool header = gt != lastGame;
        if (ty + (header ? 2 : 1) * (FONT_PX + 10) > y + H - 140) {
            draw_text("... и ещё файлов: " + std::to_string(g_inst.jobs.size() - shown), tx + 20, ty, C_DIM, C_PANEL);
            break;
        }
        if (header) {
            if (!lastGame.empty() && lastGame != "\x01") ty += 14;   // просто пустой промежуток между играми
            draw_text(fit_chars(gt, maxc - 4), tx, ty, C_WARN, C_PANEL);
            ty += FONT_PX + 10;
            lastGame = gt;
        }
        shown++;
        std::string st, label = job_label(j);
        Color col = C_DIM;
        bool dimLabel = true;
        if (j.status == "done") { st = "готово"; col = C_GREEN; }
        else if (j.status == "already_installed") { st = "уже установлено"; col = C_GREEN; }
        else if (j.status == "installing") {
            int p = j.length > 0 ? (int)(j.transferred * 100 / j.length) : 0;
            st = std::to_string(p) + "%";
            col = C_TEXT; dimLabel = false;
        }
        else if (j.status == "registering") { st = "подготовка"; col = C_TEXT; dimLabel = false; }
        else if (j.status == "failed") { st = "ошибка"; col = C_WARN; }
        else if (j.status == "cancelled") { st = "отменено"; }
        else st = "ожидает";
        draw_text(fit_chars(label, maxc - 30), tx + 40, ty, dimLabel ? C_DIM : C_TEXT, C_PANEL);
        draw_text(st, x + W - 40 - text_w(st), ty, col, C_PANEL);
        ty += FONT_PX + 10;
    }
    int by = y + H - 130;
    if (g_instFinished && failed) {
        std::string m;
        for (size_t i = 0; i < g_inst.jobs.size(); i++)
            if (g_inst.jobs[i].status == "failed") { m = g_inst.jobs[i].message; break; }
        draw_text(fit_chars("Ошибка: " + m, maxc), tx, by + 36, C_WARN, C_PANEL);
    } else if (g_instFinished) {
        draw_text("Готово! Игра установлена, ищите её на главном экране консоли.", tx, by + 36, C_GREEN, C_PANEL);
    }
    std::string hint = g_instFinished ? "{O} - закрыть" : "{O} - скрыть окно (установка продолжится)";
    draw_hint(hint, x + (W - hint_w(hint)) / 2, y + H - 36, C_TEXT, C_PANEL);
}

static void draw_all()
{
    g_spinnerSeen = false;
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, C_BG);
    fill(0, 0, FRAME_WIDTH, HEAD_H, C_HEAD);

    // Место под логотип (картинку добавим позже).
    int x = MARGIN_X;
    fill(x, 14, 160, HEAD_H - 28, C_LOGO);
    draw_text("Logo", x + (160 - text_w("Logo")) / 2, 14 + (HEAD_H - 28) / 2 + FONT_PX / 3, C_DIM, C_LOGO);
    x += 160 + 40;

    // Вкладки. Ширина по тексту, активная выделена.
    draw_pad_icon('L', x, HEAD_H / 2 - ICON_S / 2, C_HEAD);
    x += icon_w('L') + 20;
    for (int i = 0; i < TAB_COUNT; i++) {
        std::string name = TAB_NAMES[i];
        int w = text_w(name) + 40;
        Color bg = (i == g_tab) ? C_TAB_ON : C_TAB_OFF;
        fill(x, 14, w, HEAD_H - 28, bg);
        draw_text(name, x + 20, 14 + (HEAD_H - 28) / 2 + FONT_PX / 3, (i == g_tab) ? C_TEXT : C_DIM, bg);
        x += w + 16;
    }
    x += 8;
    draw_pad_icon('R', x, HEAD_H / 2 - ICON_S / 2, C_HEAD);

    if (g_svc != SVC_OK) {
        draw_service_message();
    } else if (g_tab == TAB_REQUESTS) {
        draw_requests();
    } else if (g_tab == TAB_AVAILABLE || g_tab == TAB_INSTALLED || g_tab == TAB_CATALOG) {
        draw_available();
    } else {
        std::string body = std::string("Здесь будет вкладка «") + TAB_NAMES[g_tab] + "»";
        draw_text(body, MARGIN_X, HEAD_H + 120, C_TEXT, C_BG);
    }

    if (g_svc == SVC_OK && (g_mode == MODE_CONFIRM || (g_mode == MODE_DELETE && g_deleteFrom == MODE_CONFIRM))) draw_confirm();
    if (g_svc == SVC_OK && g_mode == MODE_DELETE) draw_delete();
    if (g_svc == SVC_OK && g_mode == MODE_PROGRESS) draw_progress();

    // Идёт установка, а окно скрыто: короткая строка под сеткой
    if (g_instActive && g_mode == MODE_GRID) {
        char b[160];
        snprintf(b, sizeof(b), "Идёт установка: %d%%", overall_pct());
        draw_text(fit_chars(g_instTitle, 40) + " - " + b, MARGIN_X + 480, FRAME_HEIGHT - 90, C_GREEN, C_BG);
    }

    // Нижняя строка
    const char* hint = "";
    if (g_tab == TAB_INSTALLED && g_mode == MODE_GRID)
        hint = "{X} - открыть    {T} - удалить    {S} - обновить";
    else if (g_tab == TAB_CATALOG && g_mode == MODE_GRID)
        hint = g_stripFocus ? "влево / вправо - буква    вниз - к играм    {T} - к играм" : "{X} - информация    {T} - алфавит    {l} / {r} - по буквам";
    else if (g_tab == TAB_REQUESTS && g_mode == MODE_GRID)
        hint = g_games.empty() ? "" : "{O} - удалить из списка";
    else if (g_tab == TAB_AVAILABLE && g_mode == MODE_GRID)
        hint = g_instActive ? "{X} - в очередь    {T} - ход установки    {S} - обновить"
                            : "{X} - установить    {S} - обновить";
    else if (g_mode != MODE_GRID) hint = "";
    draw_hint(hint, (FRAME_WIDTH - hint_w(hint)) / 2, FRAME_HEIGHT - 40, C_DIM, C_BG);
    std::string ver = std::string("версия ") + APP_VERSION;
    draw_text(ver, FRAME_WIDTH - MARGIN_X - text_w(ver), FRAME_HEIGHT - 90, C_DIM, C_BG);
}

// ---------------------------------------------------------------- геймпад

static int g_pad = -1;

static bool pad_init()
{
    if (scePadInit() != 0) return false;
    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    int userID = 0;
    sceUserServiceGetInitialUser(&userID);
    g_pad = scePadOpen(userID, 0, 0, NULL);
    return g_pad >= 0;
}

// Кнопки с учётом левого стика: стик превращается в «нажатия» крестовины.
static unsigned pad_read()
{
    if (g_pad < 0) return 0;
    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    scePadReadState(g_pad, &data);
    unsigned b = (unsigned)data.buttons;
    if (data.leftStick.x < 60) b |= ORBIS_PAD_BUTTON_LEFT;
    if (data.leftStick.x > 196) b |= ORBIS_PAD_BUTTON_RIGHT;
    if (data.leftStick.y < 60) b |= ORBIS_PAD_BUTTON_UP;
    if (data.leftStick.y > 196) b |= ORBIS_PAD_BUTTON_DOWN;
    return b;
}

// ---------------------------------------------------------------- вход

bool ui_init()
{
    // Для проверок: каталог можно положить на консоль в /data/ps4installer_app/catalog/ (тогда PKG собирают без него).
    if (!catalog_load("/data/ps4installer_app/catalog")) catalog_load("/app0/assets/misc");
    g_scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    if (!g_scene->Init(0xC000000, 2)) { logf_("ui: scene init FAILED"); return false; }
    if (!g_scene->InitFont(&g_font, "/app0/assets/fonts/DejaVuSansMono.ttf", FONT_PX)) { logf_("ui: font init FAILED"); return false; }

    FT_UInt gi = FT_Get_Char_Index(g_font, 'M');
    if (!FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) {
        int w = (int)(g_font->glyph->advance.x >> 6);
        if (w > 0) g_cellW = w;
    }
    bool padOk = pad_init();
    logf_("ui: ready, cell width %d, pad %s", g_cellW, padOk ? "ok" : "FAILED");
    g_ready = true;
    return true;
}

static void cursor_move(int dx, int dy)
{
    if (g_games.empty()) return;
    int n = (int)g_games.size();
    int c = g_cursor;
    if (dx < 0 && c % COLS > 0) c--;
    if (dx > 0 && c % COLS < COLS - 1 && c + 1 < n) c++;
    if (dy < 0 && c - COLS >= 0) c -= COLS;
    if (dy > 0) { if (c + COLS < n) c += COLS; else if ((c / COLS) < (n - 1) / COLS) c = n - 1; }
    if (c != g_cursor) {
        g_cursor = c;
        int row = c / COLS;
        if (row < g_topRow) g_topRow = row;
        if (row >= g_topRow + ROWS_VIS) g_topRow = row - ROWS_VIS + 1;
        g_redraw = 2;
    }
}

// Перескок к началу следующей (dir > 0) или предыдущей буквы алфавита (вкладка «Каталог игр»).
static void jump_letter(int dir)
{
    int n = (int)g_games.size();
    if (n == 0) return;
    int i = g_cursor;
    char cur = letter_of(g_games[(size_t)i].title);
    if (dir > 0) {
        while (i < n && letter_of(g_games[(size_t)i].title) == cur) i++;
        if (i >= n) return;
    } else {
        while (i > 0 && letter_of(g_games[(size_t)i - 1].title) == cur) i--;
        if (i == g_cursor) {   // уже в начале буквы: идём к началу предыдущей
            if (i == 0) return;
            char prev = letter_of(g_games[(size_t)i - 1].title);
            while (i > 0 && letter_of(g_games[(size_t)i - 1].title) == prev) i--;
        }
    }
    g_cursor = i;
    g_topRow = i / COLS;   // строка с началом буквы - сверху
    int rows = (n + COLS - 1) / COLS;
    if (g_topRow > rows - ROWS_VIS) g_topRow = rows > ROWS_VIS ? rows - ROWS_VIS : 0;
    g_redraw = 2;
}

// Переход к первой игре на букву с номером li (0 - «#», 1..26 - A..Z).
static void goto_letter(int li)
{
    for (size_t i = 0; i < g_games.size(); i++)
        if (letter_index(letter_of(g_games[i].title)) == li) {
            g_cursor = (int)i;
            int rows = ((int)g_games.size() + COLS - 1) / COLS;
            g_topRow = (int)i / COLS;
            if (g_topRow > rows - ROWS_VIS) g_topRow = rows > ROWS_VIS ? rows - ROWS_VIS : 0;
            g_redraw = 2;
            return;
        }
}

// Полоса букв: на соседнюю букву, в которой есть игры.
static void strip_step(int dir)
{
    if (g_games.empty()) return;
    int cur = letter_index(letter_of(g_games[(size_t)g_cursor].title));
    for (int k = cur + dir; k >= 0 && k < 27; k += dir)
        if (g_letterHas[k]) { goto_letter(k); return; }
}

// Запрос игры: пока просто записываем в /data/ps4installer_app/requests.txt (Title ID и название), без повторов.
static bool request_game(const Game& g, std::string& msg)
{
    const char* path = REQ_PATH;
    std::string line = g.title_id + "\t" + g.title;
    std::string all;
    if (FILE* f = fopen(path, "rb")) {
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
        fclose(f);
    }
    if (all.find(line + "\n") != std::string::npos) { msg = "Уже запрошено ранее"; return true; }
    mkdir("/data/ps4installer_app", 0777);
    FILE* f = fopen(path, "ab");
    if (!f) { msg = "Не удалось записать запрос"; return false; }
    fputs((line + "\n").c_str(), f);
    fclose(f);
    msg = "Запрос записан";
    return true;
}

void ui_pump()
{
    if (!g_ready) return;
    uint64_t now = sceKernelGetProcessTime();

    unsigned b = pad_read();
    static unsigned prev = 0;
    unsigned pressed = b & ~prev;
    prev = b;
    if (pressed) logf_("pad: pressed 0x%08X", pressed);  // для отладки первых запусков на консоли

    // Автоповтор направлений при удержании: первая задержка 350 мс, затем каждые 110 мс.
    const unsigned DIRS = ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT;
    static uint64_t nextRepeat = 0;
    unsigned dirs = pressed & DIRS;
    if (dirs) nextRepeat = now + 350000;
    else if ((b & DIRS) && now >= nextRepeat) { dirs = b & DIRS; nextRepeat = now + 110000; }

    int old = g_tab;
    if (g_mode == MODE_GRID) {
        if (pressed & ORBIS_PAD_BUTTON_L1) g_tab = (g_tab + TAB_COUNT - 1) % TAB_COUNT;
        if (pressed & ORBIS_PAD_BUTTON_R1) g_tab = (g_tab + 1) % TAB_COUNT;
    }
    if (g_tab != old) { g_stripFocus = false; switch_list(g_tab); g_redraw = 2; }

    // Опрос хода установки (раз в секунду), пока она идёт.
    if (g_instActive && now - g_lastPoll >= 1000000) {
        g_lastPoll = now;
        InstallStatus st;
        if (service_status(st)) {
            g_inst = st;
            if (!st.running && !st.jobs.empty()) {
                g_instActive = false;
                g_instFinished = true;
                mark_installed();
                g_installedStale = true;
                g_catalogStale = true;
                logf_("ui: установка завершена (%d файлов)", (int)st.jobs.size());
            }
            g_redraw = 2;
        }
    }

    static bool first = true;
    if ((g_svc == SVC_NO_LOADER || g_svc == SVC_FAIL) && (pressed & ORBIS_PAD_BUTTON_SQUARE)) {
        g_svc = SVC_CHECK;
        g_svcDrawn = false;
        g_redraw = 2;
    }
    if (g_svc == SVC_OK && (g_tab == TAB_AVAILABLE || g_tab == TAB_INSTALLED || g_tab == TAB_CATALOG)) {
        bool isAvail = g_tab == TAB_AVAILABLE;
        bool wantLoad = (isAvail ? first : g_load == LS_NONE) || (g_mode == MODE_GRID && (pressed & ORBIS_PAD_BUTTON_SQUARE));
        if (wantLoad && g_load != LS_LOADING) {
            g_freshScan = isAvail && !first;
            if (isAvail) first = false;
            g_load = LS_LOADING;
            g_redraw = 2;
        }
        if (g_load == LS_OK) {
            int dx = (dirs & ORBIS_PAD_BUTTON_RIGHT ? 1 : 0) - (dirs & ORBIS_PAD_BUTTON_LEFT ? 1 : 0);
            int dy = (dirs & ORBIS_PAD_BUTTON_DOWN ? 1 : 0) - (dirs & ORBIS_PAD_BUTTON_UP ? 1 : 0);
            if ((dx || dy) && g_mode == MODE_GRID) {
                if (g_tab == TAB_CATALOG && g_stripFocus) {
                    if (dy > 0) { g_stripFocus = false; g_redraw = 2; }
                    else if (dx) strip_step(dx);
                }
                else cursor_move(dx, dy);
            }
            if (g_mode == MODE_GRID && g_tab == TAB_INSTALLED && (pressed & ORBIS_PAD_BUTTON_TRIANGLE) && !g_games.empty()) {
                g_sel = g_games[(size_t)g_cursor];
                g_deleteErr.clear();
                g_deleteFrom = MODE_GRID;
                g_mode = MODE_DELETE; g_skipDeleteInput = true;
                g_redraw = 2;
            } else if (g_mode == MODE_GRID && g_tab == TAB_AVAILABLE && (pressed & ORBIS_PAD_BUTTON_TRIANGLE) && (g_instActive || g_instFinished)) {
                g_mode = MODE_PROGRESS;
                g_redraw = 2;
            }
            if (g_mode == MODE_GRID && g_tab == TAB_CATALOG && (pressed & ORBIS_PAD_BUTTON_TRIANGLE)) { g_stripFocus = !g_stripFocus; g_redraw = 2; }
            if (g_mode == MODE_GRID && g_tab == TAB_CATALOG && (pressed & ORBIS_PAD_BUTTON_L2)) jump_letter(-1);
            if (g_mode == MODE_GRID && g_tab == TAB_CATALOG && (pressed & ORBIS_PAD_BUTTON_R2)) jump_letter(1);
            if (g_mode == MODE_GRID && g_tab == TAB_CATALOG && !g_stripFocus && (pressed & ORBIS_PAD_BUTTON_CROSS) && !g_games.empty()) {
                g_sel = g_games[(size_t)g_cursor];
                g_cardReason = CARD_REQUEST;
                g_confirmErr.clear();
                g_confirmOk = false;
                info_load(g_sel);
                g_mode = MODE_CONFIRM;
                g_skipConfirmInput = true;
                logf_("ui: информация об игре из каталога: %s (%s)", g_sel.title.c_str(), g_sel.title_id.c_str());
                g_redraw = 2;
            } else if (g_mode == MODE_GRID && g_tab == TAB_INSTALLED && (pressed & ORBIS_PAD_BUTTON_CROSS) && !g_games.empty()) {
                g_sel = g_games[(size_t)g_cursor];
                g_cardReason = CARD_LAUNCH;
                g_confirmErr.clear();
                info_load(g_sel);
                g_mode = MODE_CONFIRM;
                g_skipConfirmInput = true;
                logf_("ui: окно установленной игры: %s (%s)", g_sel.title.c_str(), g_sel.title_id.c_str());
                g_redraw = 2;
            } else if (g_mode == MODE_GRID && (pressed & ORBIS_PAD_BUTTON_CROSS) && !g_games.empty()) {
                // Карточка открывается для любой игры; если ставить нельзя, неактивна только кнопка установки.
                g_sel = g_games[(size_t)g_cursor];
                g_cardReason = g_sel.installed ? CARD_INSTALLED : g_sel.incomplete ? CARD_INCOMPLETE
                             : (g_instActive && game_queued(g_sel)) ? CARD_QUEUED : CARD_OK;
                g_selFiles = install_files(g_sel, service_fw_major());
                g_selNeed = install_bytes(g_sel, service_fw_major()) + (g_instActive ? pending_bytes() : 0);
                g_freeOk = service_free(g_freeBytes, &g_totalBytes);
                if (!g_freeOk) g_freeOk = service_free(g_freeBytes, &g_totalBytes);   // одна повторная попытка
                if (!g_freeOk) {   // служба пропала (упала или закрыта): запускаем заново, как при старте приложения
                    logf_("ui: служба не отвечает, перезапускаем");
                    g_svc = SVC_CHECK;
                    g_svcDrawn = false;
                    g_redraw = 2;
                } else {
                g_confirmErr.clear();
                info_load(g_sel);
                g_mode = MODE_CONFIRM;
                g_skipConfirmInput = true;
                logf_("ui: карточка: %s, причина=%d, нужно %llu, свободно %llu (ok=%d)", g_sel.title.c_str(), g_cardReason,
                      (unsigned long long)g_selNeed, (unsigned long long)g_freeBytes, g_freeOk ? 1 : 0);
                g_redraw = 2;
                }
            }
        }
    }

    if (g_svc == SVC_OK && g_tab == TAB_REQUESTS && g_mode == MODE_GRID) {
        if (g_load == LS_NONE || (pressed & ORBIS_PAD_BUTTON_SQUARE)) { if (g_load != LS_LOADING) { g_load = LS_LOADING; g_redraw = 2; } }
        if (g_load == LS_OK && !g_games.empty()) {
            int dy = (dirs & ORBIS_PAD_BUTTON_DOWN ? 1 : 0) - (dirs & ORBIS_PAD_BUTTON_UP ? 1 : 0);
            int n = (int)g_games.size();
            if (dy && g_cursor + dy >= 0 && g_cursor + dy < n) {
                g_cursor += dy;
                if (g_cursor < g_topRow) g_topRow = g_cursor;
                if (g_cursor >= g_topRow + REQ_ROWS_VIS) g_topRow = g_cursor - REQ_ROWS_VIS + 1;
                g_redraw = 2;
            }
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                g_sel = g_games[(size_t)g_cursor];
                g_deleteErr.clear();
                g_deleteFrom = MODE_GRID;
                g_mode = MODE_DELETE; g_skipDeleteInput = true;
                g_redraw = 2;
            }
        }
    }

    if (g_svc == SVC_OK && g_mode == MODE_CONFIRM) {
        bool low = g_cardReason == CARD_OK && g_freeOk && g_selNeed > g_freeBytes;
        unsigned cp = g_skipConfirmInput ? 0 : pressed;
        g_skipConfirmInput = false;
        if (cp & ORBIS_PAD_BUTTON_CIRCLE) { info_clear(); g_mode = MODE_GRID; g_redraw = 2; }
        else if ((cp & ORBIS_PAD_BUTTON_TRIANGLE) && g_cardReason == CARD_LAUNCH) {
            g_deleteErr.clear();
            g_deleteFrom = MODE_CONFIRM;
            g_mode = MODE_DELETE; g_skipDeleteInput = true;
            g_redraw = 2;
        }
        else if ((cp & ORBIS_PAD_BUTTON_CROSS) && g_cardReason == CARD_REQUEST) {
            g_confirmOk = request_game(g_sel, g_confirmErr);
            logf_("ui: запрос игры %s: %s", g_sel.title_id.c_str(), g_confirmErr.c_str());
            g_redraw = 2;
        }
        else if ((cp & ORBIS_PAD_BUTTON_CROSS) && g_cardReason == CARD_LAUNCH) {
            std::string err;
            logf_("ui: запуск игры %s", g_sel.title_id.c_str());
            if (service_launch(g_sel.title_id, err)) g_confirmErr = "Запускаю игру...";
            else { g_confirmErr = "Не удалось запустить: " + err; logf_("ui: запуск не удался: %s", err.c_str()); }
            g_redraw = 2;
        }
        else if ((cp & ORBIS_PAD_BUTTON_TRIANGLE) && g_cardReason == CARD_QUEUED) { info_clear(); g_mode = MODE_PROGRESS; g_redraw = 2; }
        else if ((cp & ORBIS_PAD_BUTTON_CROSS) && !low && g_cardReason == CARD_OK) {
            std::vector<std::string> paths;
            for (size_t i = 0; i < g_selFiles.size(); i++) paths.push_back(g_selFiles[i].path);
            std::string err;
            std::vector<std::string> skipped;
            if (service_install(paths, err, &skipped)) {
                for (size_t i = 0; i < skipped.size(); i++) logf_("ui: служба пропустила: %s", skipped[i].c_str());
                if (!g_instActive) { g_queuedFiles.clear(); g_queuedTitle.clear(); g_queuedGames = 0; g_inst = InstallStatus(); }
                for (size_t i = 0; i < g_selFiles.size(); i++) { g_queuedFiles.push_back(g_selFiles[i]); g_queuedTitle.push_back(g_sel.title); }
                g_queuedGames++;
                g_instTitle = g_queuedGames > 1 ? "Очередь установки (игр: " + std::to_string(g_queuedGames) + ")" : g_sel.title;
                g_instActive = true;
                g_instFinished = false;
                g_lastPoll = 0;
                info_clear();
                g_mode = MODE_PROGRESS;
                logf_("ui: установка запущена: %s (%d файлов)", g_sel.title.c_str(), (int)paths.size());
            } else {
                g_confirmErr = "Не удалось начать установку: " + err;
                logf_("ui: install error: %s", err.c_str());
            }
            g_redraw = 2;
        }
    } else if (g_svc == SVC_OK && g_mode == MODE_DELETE) {
        unsigned dp = g_skipDeleteInput ? 0 : pressed;
        g_skipDeleteInput = false;
        if (dp & ORBIS_PAD_BUTTON_CIRCLE) { g_mode = g_deleteFrom; g_redraw = 2; }
        else if ((dp & ORBIS_PAD_BUTTON_CROSS) && g_tab == TAB_REQUESTS) {
            if (req_remove(g_sel)) {
                logf_("ui: запрос удалён из списка: %s", g_sel.title.c_str());
                g_mode = MODE_GRID;
                g_load = LS_LOADING;   // перечитать список
            } else g_deleteErr = "Не удалось изменить список запросов";
            g_redraw = 2;
        }
        else if (dp & ORBIS_PAD_BUTTON_CROSS) {
            std::string err;
            logf_("ui: удаление игры %s (%s)", g_sel.title.c_str(), g_sel.title_id.c_str());
            if (service_uninstall(g_sel.title_id, err)) {
                logf_("ui: игра удалена");
                info_clear();
                g_mode = MODE_GRID;
                g_load = LS_LOADING;   // перечитать список
                mark_installed();
            } else {
                g_deleteErr = "Не удалось удалить: " + err;
                logf_("ui: удаление не удалось: %s", err.c_str());
            }
            g_redraw = 2;
        }
    } else if (g_svc == SVC_OK && g_mode == MODE_PROGRESS) {
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
            g_mode = MODE_GRID;
            if (g_instFinished) { g_instFinished = false; g_inst = InstallStatus(); }
            g_redraw = 2;
        }
    }

    if (g_redraw == 0 && g_svc == SVC_OK && (g_tab == TAB_AVAILABLE || g_tab == TAB_INSTALLED || g_tab == TAB_CATALOG || g_tab == TAB_REQUESTS) && g_mode == MODE_GRID && g_load == LS_OK &&
        tile_images_step())
        g_redraw = 2;
    else if (g_redraw == 0 && g_svc == SVC_OK && g_tab == TAB_AVAILABLE && g_mode == MODE_GRID && g_load == LS_OK &&
             !g_installedStale && g_lists[TAB_INSTALLED].load == LS_OK && !g_lists[TAB_INSTALLED].games.empty()) {
        switch_list(TAB_INSTALLED);   // картинки установленных игр - по одной за проход, пока доступные ничего не просят
        tile_images_step();
        switch_list(TAB_AVAILABLE);
    }

    // Крутилка на экране: перерисовываем примерно каждые 90 мс, чтобы она вращалась.
    if (g_spinnerSeen && g_redraw == 0 && now - g_lastSpin >= 90000) g_redraw = 1;
    bool drew = false;
    if (g_redraw > 0) {
        g_lastSpin = now;
        draw_all();
        g_scene->SubmitFlip(g_frameID);
        g_scene->FrameWait(g_frameID);
        g_scene->FrameBufferSwap();
        g_frameID++;
        g_redraw--;
        drew = true;
        static bool hidden = false;
        if (!hidden) { hidden = true; int r = sceSystemServiceHideSplashScreen(); logf_("ui: HideSplashScreen = 0x%08X", (unsigned)r); }
    }

    // Запуск службы: сообщение показываем до блокирующих действий.
    if (g_svc == SVC_CHECK || g_svc == SVC_STARTING) {
        if (drew) g_svcDrawn = true;
        if (g_svcDrawn) {
            if (g_svc == SVC_CHECK) {
                std::string ver;
                if (service_query(ver) && ver == APP_VERSION) {
                    logf_("service: уже запущена, версия %s", ver.c_str());
                    g_svc = SVC_OK;
                } else {
                    logf_("service: нужна (повторная) отправка, ответ=%s ожидали=%s", ver.empty() ? "нет" : ver.c_str(), APP_VERSION);
                    g_svc = SVC_STARTING;
                    g_svcDrawn = false;
                }
                g_redraw = 2;
            } else {
                std::string err;
                bool noLoader = false;
                if (!service_send_payload(err, &noLoader)) {
                    g_svc = noLoader ? SVC_NO_LOADER : SVC_FAIL;
                } else {
                    g_svc = SVC_FAIL;
                    for (int i = 0; i < 30; i++) {   // ждём ответа до ~15 секунд
                        sceKernelUsleep(500000);
                        std::string ver;
                        if (service_query(ver) && ver == APP_VERSION) { g_svc = SVC_OK; break; }
                    }
                }
                if (g_svc == SVC_OK && !g_autorunTried) {   // один раз за запуск приложения: автозапуск службы
                    g_autorunTried = true;
                    std::string aerr;
                    service_install_autorun(aerr);
                }
                logf_("service: итог запуска = %d", (int)g_svc);
                g_redraw = 2;
            }
        }
    }

    // Загрузка списка: сначала на экране показывается «Загрузка...», потом (после показа кадра) идёт запрос.
    if (drew && g_load == LS_LOADING && g_redraw == 1) {
        if (g_tab == TAB_INSTALLED) load_installed();
        else if (g_tab == TAB_CATALOG) load_catalog_list();
        else if (g_tab == TAB_REQUESTS) load_requests();
        else {
            load_games(g_freshScan);
            if (!g_preloaded && g_load == LS_OK) {
                g_preloaded = true;
                preload_installed();
                if (g_games.empty()) {   // доступных игр нет: сразу показываем установленные
                    g_tab = TAB_INSTALLED;
                    switch_list(TAB_INSTALLED);
                    logf_("ui: доступных игр нет, переключаемся на «Установленные игры»");
                }
            }
        }
        g_freshScan = false;
        g_redraw = 2;
    }
}
