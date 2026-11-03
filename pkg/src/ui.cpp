// Шаг 1: каркас интерфейса. Шапка (Logo + три вкладки), переключение вкладок L1/R1,
// нижняя строка подсказок. Содержимое вкладок пока заглушки: список игр, окна установки
// и связь со службой добавляются следующими шагами.
#include "ui.h"
#include "scene.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <string>

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

static int decode_utf8(const unsigned char*& p)
{
    unsigned c = *p++;
    if (c < 0x80) return (int)c;

    int extra;
    unsigned cp;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return '?';

    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    return (int)cp;
}

// Число символов (не байтов): кириллица занимает 2 байта.
static int count_cp(const std::string& s)
{
    const unsigned char* p = (const unsigned char*)s.c_str();
    int n = 0;
    while (*p) { decode_utf8(p); n++; }
    return n;
}

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

// Ширина текста в пикселях (шрифт моноширинный: все символы одной ширины).
static int text_w(const std::string& s) { return count_cp(s) * g_cellW; }

static void fill(int x, int y, int w, int h, Color c) { g_scene->FillRect(x, y, w, h, c); }

// ---------------------------------------------------------------- рисование

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

    // Тело вкладки (заглушка)
    std::string body = std::string("Здесь будет вкладка «") + TAB_NAMES[g_tab] + "»";
    draw_text(body, MARGIN_X, HEAD_H + 120, C_TEXT, C_BG);
    draw_text("Каркас приложения: переключение вкладок кнопками L1 и R1.", MARGIN_X, HEAD_H + 180, C_DIM, C_BG);

    // Нижняя строка
    draw_text("L1 / R1 - вкладки", MARGIN_X, FRAME_HEIGHT - 50, C_DIM, C_BG);
    std::string ver = std::string("версия ") + APP_VERSION;
    draw_text(ver, FRAME_WIDTH - MARGIN_X - text_w(ver), FRAME_HEIGHT - 50, C_DIM, C_BG);
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

static unsigned pad_read()
{
    if (g_pad < 0) return 0;
    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    scePadReadState(g_pad, &data);
    return (unsigned)data.buttons;
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

void ui_pump()
{
    if (!g_ready) return;

    unsigned b = pad_read();
    static unsigned prev = 0;
    unsigned pressed = b & ~prev;
    prev = b;
    if (pressed) logf_("pad: pressed 0x%08X", pressed);  // для отладки первых запусков на консоли

    int old = g_tab;
    if (pressed & ORBIS_PAD_BUTTON_L1) g_tab = (g_tab + TAB_COUNT - 1) % TAB_COUNT;
    if (pressed & ORBIS_PAD_BUTTON_R1) g_tab = (g_tab + 1) % TAB_COUNT;
    if (g_tab != old) g_redraw = 2;

    if (g_redraw > 0) {
        draw_all();
        g_scene->SubmitFlip(g_frameID);
        g_scene->FrameWait(g_frameID);
        g_scene->FrameBufferSwap();
        g_frameID++;
        g_redraw--;
        static bool hidden = false;
        if (!hidden) { hidden = true; int r = sceSystemServiceHideSplashScreen(); logf_("ui: HideSplashScreen = 0x%08X", (unsigned)r); }
    }
}
