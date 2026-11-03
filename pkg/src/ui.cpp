// Шаг 2: шапка (Logo + три вкладки, L1/R1) и вкладка «Доступные игры»: сетка игр по данным
// службы (/scan), навигация крестовиной и левым стиком, Квадрат - обновить список.
// Остальные вкладки пока заглушки; окно установки добавляется следующим шагом.
#include "ui.h"
#include "scene.h"
#include "log.h"
#include "service.h"
#include "games.h"
#include "utf8.h"

#include <stdio.h>
#include <string.h>
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

enum Tab { TAB_AVAILABLE = 0, TAB_INSTALLED = 1, TAB_CATALOG = 2, TAB_COUNT = 3 };
static int g_tab = TAB_AVAILABLE;

static const char* TAB_NAMES[TAB_COUNT] = {
    "Доступные игры", "Установленные игры", "Каталог игр"
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
static std::string g_status;   // строка под сеткой

// Окна: подтверждение установки и ход установки.
enum Mode { MODE_GRID = 0, MODE_CONFIRM, MODE_PROGRESS };
static Mode g_mode = MODE_GRID;
static Game g_sel;                         // выбранная игра
static std::vector<PkgItem> g_selFiles;    // что будет установлено (по порядку)
static uint64_t g_selNeed = 0, g_freeBytes = 0;
static bool g_freeOk = false;
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

static void mark_installed()
{
    std::vector<std::string> ids;
    if (!service_installed(ids)) return;
    for (size_t i = 0; i < g_games.size(); i++) {
        g_games[i].installed = false;
        for (size_t k = 0; k < ids.size(); k++)
            if (!g_games[i].title_id.empty() && ids[k] == g_games[i].title_id) g_games[i].installed = true;
    }
}

static void load_games()
{
    std::vector<PkgItem> items;
    std::string err;
    if (service_scan(items, err)) {
        build_games(items, g_games);
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

// ---------------------------------------------------------------- рисование

static void draw_tile(int x, int y, const Game& g, bool selected)
{
    Color bg = selected ? C_TILE_SEL : C_TILE;
    if (selected) fill(x - 5, y - 5, TILE_W + 10, TILE_H + 10, C_TAB_ON);
    fill(x, y, TILE_W, TILE_H, bg);

    // Пока без обложки: цветной квадрат по title_id и первая буква названия.
    unsigned h = 0;
    for (size_t i = 0; i < g.title_id.size(); i++) h = h * 31 + (unsigned char)g.title_id[i];
    Color c = { (uint8_t)(50 + (h & 0x3F)), (uint8_t)(60 + ((h >> 6) & 0x3F)), (uint8_t)(80 + ((h >> 12) & 0x5F)) };
    fill(x, y, TILE_W, IMG_H, c);
    std::string letter;
    {
        const unsigned char* p = (const unsigned char*)g.title.c_str();
        const unsigned char* st = p;
        if (*p) { decode_utf8(p); letter = std::string((const char*)st, (size_t)(p - st)); }
    }
    draw_text(letter, x + (TILE_W - g_cellW) / 2, y + IMG_H / 2 + FONT_PX / 3, C_TEXT, c);

    // Размер игры в углу картинки
    std::string sz = fmt_size(g.size);
    int szw = text_w(sz) + 12;
    fill(x + TILE_W - szw, y + IMG_H - FONT_PX - 10, szw, FONT_PX + 10, C_BG);
    draw_text(sz, x + TILE_W - szw + 6, y + IMG_H - 9, C_DIM, C_BG);

    // Уже установлена: полоса поверх картинки
    if (g.installed) {
        std::string m = "УЖЕ УСТАНОВЛЕНА";
        int w = text_w(m) + 16;
        int mx = x + (TILE_W - w) / 2, my = y + IMG_H / 2 - (FONT_PX + 16) / 2;
        fill(mx, my, w, FONT_PX + 16, C_GREEN);
        draw_text(m, mx + 8, my + FONT_PX + 4, C_BG, C_GREEN);
    }

    if (g.incomplete && !g.installed) {
        std::string m = "НЕ ДОКАЧАНА";
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
    if (!tags.empty()) {
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
        draw_text("Проверка службы установки...", MARGIN_X, y, C_TEXT, C_BG);
    } else if (g_svc == SVC_STARTING) {
        draw_text("Запуск службы установки...", MARGIN_X, y, C_TEXT, C_BG);
        draw_text("Это занимает несколько секунд.", MARGIN_X, y + 50, C_DIM, C_BG);
    } else if (g_svc == SVC_NO_LOADER) {
        draw_text("Не удалось запустить службу: загрузчик payload'ов выключен.", MARGIN_X, y, C_WARN, C_BG);
        draw_text("Включите в GoldHEN: Servers settings - Enable Payloader Server.", MARGIN_X, y + 50, C_TEXT, C_BG);
        draw_text("Затем нажмите Квадрат.", MARGIN_X, y + 100, C_DIM, C_BG);
    } else {
        draw_text("Служба установки не запустилась.", MARGIN_X, y, C_WARN, C_BG);
        draw_text("Подождите немного и нажмите Квадрат. Если не помогает, перезапустите GoldHEN.", MARGIN_X, y + 50, C_DIM, C_BG);
    }
}

static void draw_available()
{
    if (g_load == LS_LOADING || g_load == LS_NONE) {
        draw_text("Загрузка списка игр...", MARGIN_X, GRID_TOP + 60, C_TEXT, C_BG);
        return;
    }
    if (g_load == LS_ERROR) {
        draw_text("Служба установки не отвечает.", MARGIN_X, GRID_TOP + 60, C_WARN, C_BG);
        draw_text("Запустите ps4installerd (адрес 127.0.0.1, порт 8284) и нажмите Квадрат.", MARGIN_X, GRID_TOP + 110, C_DIM, C_BG);
        draw_text("Причина: " + g_loadErr, MARGIN_X, GRID_TOP + 160, C_DIM, C_BG);
        return;
    }
    if (g_games.empty()) {
        draw_text("Игр не найдено.", MARGIN_X, GRID_TOP + 60, C_TEXT, C_BG);
        draw_text("Подключите флешку или диск с играми и нажмите Квадрат.", MARGIN_X, GRID_TOP + 110, C_DIM, C_BG);
        return;
    }
    int gapX = (FRAME_WIDTH - 2 * MARGIN_X - COLS * TILE_W) / (COLS - 1);
    for (int r = 0; r < ROWS_VIS; r++) {
        for (int c = 0; c < COLS; c++) {
            int idx = (g_topRow + r) * COLS + c;
            if (idx >= (int)g_games.size()) break;
            draw_tile(MARGIN_X + c * (TILE_W + gapX), GRID_TOP + r * (TILE_H + GAP_Y), g_games[(size_t)idx], idx == g_cursor);
        }
    }
    int rows = ((int)g_games.size() + COLS - 1) / COLS;
    char pg[96];
    snprintf(pg, sizeof(pg), "Игра %d из %d   ряд %d из %d", g_cursor + 1, (int)g_games.size(), g_cursor / COLS + 1, rows);
    draw_text(pg, MARGIN_X, FRAME_HEIGHT - 90, C_DIM, C_BG);
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

static void draw_confirm()
{
    const int W = 1300, H = 700;
    int x = (FRAME_WIDTH - W) / 2, y = HEAD_H + 40;
    draw_panel(x, y, W, H);
    int tx = x + 40, ty = y + 56;
    int maxc = (W - 80) / g_cellW;
    std::vector<std::string> t = wrap_lines(g_sel.title, maxc, 2);
    for (size_t i = 0; i < t.size(); i++) draw_text(t[i], tx, ty + (int)i * (FONT_PX + 6), C_TEXT, C_PANEL);
    ty += (int)t.size() * (FONT_PX + 6) + 20;

    draw_text("Будет установлено:", tx, ty, C_DIM, C_PANEL);
    ty += FONT_PX + 12;
    int maxRows = 8;
    for (size_t i = 0; i < g_selFiles.size() && (int)i < maxRows; i++) {
        std::string lab = fit_chars(file_label(g_selFiles[i]), maxc - 14);
        std::string sz = fmt_size(g_selFiles[i].size);
        draw_text(lab, tx + 20, ty, C_TEXT, C_PANEL);
        draw_text(sz, x + W - 40 - text_w(sz), ty, C_DIM, C_PANEL);
        ty += FONT_PX + 8;
    }
    if ((int)g_selFiles.size() > maxRows) {
        draw_text("... и ещё " + std::to_string(g_selFiles.size() - maxRows), tx + 20, ty, C_DIM, C_PANEL);
        ty += FONT_PX + 8;
    }
    ty += 14;
    draw_text("Всего нужно: " + fmt_size(g_selNeed), tx, ty, C_TEXT, C_PANEL);
    ty += FONT_PX + 10;
    if (g_freeOk) draw_text("Свободно на консоли: " + fmt_size(g_freeBytes), tx, ty, C_TEXT, C_PANEL);
    else draw_text("Свободное место узнать не удалось", tx, ty, C_WARN, C_PANEL);

    bool low = g_freeOk && g_selNeed > g_freeBytes;
    int by = y + H - 130;
    if (low) {
        fill(tx, by, W - 80, 52, C_RED_BG);
        std::string msg = "МАЛО МЕСТА - УДАЛИТЕ НЕНУЖНУЮ ИГРУ";
        draw_text(msg, x + (W - text_w(msg)) / 2, by + 36, C_TEXT, C_RED_BG);
    } else if (!g_confirmErr.empty()) {
        draw_text(fit_chars(g_confirmErr, maxc), tx, by + 36, C_WARN, C_PANEL);
    }
    std::string hint = low ? "Круг - закрыть" : "Крест - установить      Круг - закрыть";
    draw_text(hint, x + (W - text_w(hint)) / 2, y + H - 36, low ? C_DIM : C_TEXT, C_PANEL);
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
    std::string hint = g_instFinished ? "Круг - закрыть" : "Круг - скрыть окно (установка продолжится)";
    draw_text(hint, x + (W - text_w(hint)) / 2, y + H - 36, C_TEXT, C_PANEL);
}

static void draw_all()
{
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, C_BG);
    fill(0, 0, FRAME_WIDTH, HEAD_H, C_HEAD);

    // Место под логотип (картинку добавим позже).
    int x = MARGIN_X;
    fill(x, 14, 160, HEAD_H - 28, C_LOGO);
    draw_text("Logo", x + (160 - text_w("Logo")) / 2, 14 + (HEAD_H - 28) / 2 + FONT_PX / 3, C_DIM, C_LOGO);
    x += 160 + 40;

    // Вкладки. Ширина по тексту, активная выделена.
    draw_text("L1", x, HEAD_H / 2 + FONT_PX / 3, C_DIM, C_HEAD);
    x += text_w("L1") + 24;
    for (int i = 0; i < TAB_COUNT; i++) {
        std::string name = TAB_NAMES[i];
        int w = text_w(name) + 40;
        Color bg = (i == g_tab) ? C_TAB_ON : C_TAB_OFF;
        fill(x, 14, w, HEAD_H - 28, bg);
        draw_text(name, x + 20, 14 + (HEAD_H - 28) / 2 + FONT_PX / 3, (i == g_tab) ? C_TEXT : C_DIM, bg);
        x += w + 16;
    }
    x += 8;
    draw_text("R1", x, HEAD_H / 2 + FONT_PX / 3, C_DIM, C_HEAD);

    if (g_svc != SVC_OK) {
        draw_service_message();
    } else if (g_tab == TAB_AVAILABLE) {
        draw_available();
    } else {
        std::string body = std::string("Здесь будет вкладка «") + TAB_NAMES[g_tab] + "»";
        draw_text(body, MARGIN_X, HEAD_H + 120, C_TEXT, C_BG);
    }

    if (g_svc == SVC_OK && g_mode == MODE_CONFIRM) draw_confirm();
    if (g_svc == SVC_OK && g_mode == MODE_PROGRESS) draw_progress();

    // Идёт установка, а окно скрыто: короткая строка под сеткой
    if (g_instActive && g_mode == MODE_GRID) {
        char b[160];
        snprintf(b, sizeof(b), "Идёт установка: %d%%", overall_pct());
        draw_text(fit_chars(g_instTitle, 40) + " - " + b, MARGIN_X + 480, FRAME_HEIGHT - 90, C_GREEN, C_BG);
    }

    // Нижняя строка
    const char* hint = "L1 / R1 - вкладки";
    if (g_tab == TAB_AVAILABLE && g_mode == MODE_GRID)
        hint = g_instActive ? "Стрелки / стик - выбор    Крест - в очередь    Треугольник - ход установки    Квадрат - обновить    L1 / R1 - вкладки"
                            : "Стрелки / стик - выбор    Крест - установить    Квадрат - обновить    L1 / R1 - вкладки";
    else if (g_mode != MODE_GRID) hint = "";
    draw_text(hint, MARGIN_X, FRAME_HEIGHT - 40, C_DIM, C_BG);
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
    if (g_tab != old) g_redraw = 2;

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
    if (g_svc == SVC_OK && g_tab == TAB_AVAILABLE) {
        if ((first || (pressed & ORBIS_PAD_BUTTON_SQUARE)) && g_load != LS_LOADING) {
            first = false;
            g_load = LS_LOADING;
            g_redraw = 2;
        }
        if (g_load == LS_OK) {
            int dx = (dirs & ORBIS_PAD_BUTTON_RIGHT ? 1 : 0) - (dirs & ORBIS_PAD_BUTTON_LEFT ? 1 : 0);
            int dy = (dirs & ORBIS_PAD_BUTTON_DOWN ? 1 : 0) - (dirs & ORBIS_PAD_BUTTON_UP ? 1 : 0);
            if ((dx || dy) && g_mode == MODE_GRID) cursor_move(dx, dy);
            if (g_mode == MODE_GRID && (pressed & ORBIS_PAD_BUTTON_TRIANGLE) && (g_instActive || g_instFinished)) {
                g_mode = MODE_PROGRESS;
                g_redraw = 2;
            }
            if (g_mode == MODE_GRID && (pressed & ORBIS_PAD_BUTTON_CROSS) && !g_games.empty()) {
                if (g_games[(size_t)g_cursor].installed || g_games[(size_t)g_cursor].incomplete) {
                    // уже установлена или докачана не до конца: не ставим
                } else if (g_instActive && game_queued(g_games[(size_t)g_cursor])) {
                    g_mode = MODE_PROGRESS;                       // она уже в очереди: показываем ход
                } else {
                    g_sel = g_games[(size_t)g_cursor];
                    g_selFiles = install_files(g_sel, service_fw_major());
                    g_selNeed = install_bytes(g_sel, service_fw_major()) + (g_instActive ? pending_bytes() : 0);
                    g_freeOk = service_free(g_freeBytes);
                    g_confirmErr.clear();
                    g_mode = MODE_CONFIRM;
                    logf_("ui: окно установки: %s, нужно %llu, свободно %llu (ok=%d)", g_sel.title.c_str(),
                          (unsigned long long)g_selNeed, (unsigned long long)g_freeBytes, g_freeOk ? 1 : 0);
                }
                g_redraw = 2;
            }
        }
    }

    if (g_svc == SVC_OK && g_mode == MODE_CONFIRM) {
        bool low = g_freeOk && g_selNeed > g_freeBytes;
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE) { g_mode = MODE_GRID; g_redraw = 2; }
        else if ((pressed & ORBIS_PAD_BUTTON_CROSS) && !low) {
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
                g_mode = MODE_PROGRESS;
                logf_("ui: установка запущена: %s (%d файлов)", g_sel.title.c_str(), (int)paths.size());
            } else {
                g_confirmErr = "Не удалось начать установку: " + err;
                logf_("ui: install error: %s", err.c_str());
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

    bool drew = false;
    if (g_redraw > 0) {
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
        load_games();
        g_redraw = 2;
    }
}
