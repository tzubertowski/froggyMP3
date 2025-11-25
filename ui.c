#include "ui.h"
#include "font.h"

void ui_clear(uint16_t col)
{
    int i;
    for (i = 0; i < SCREEN_W * SCREEN_H; i++)
        pixels[i] = col;
}

void ui_fill(int x, int y, int w, int h, uint16_t col)
{
    int px, py;
    for (py = y; py < y + h && py < SCREEN_H; py++) {
        if (py < 0) continue;
        for (px = x; px < x + w && px < SCREEN_W; px++) {
            if (px < 0) continue;
            pixels[py * SCREEN_W + px] = col;
        }
    }
}

void ui_pill(int x, int y, int w, int h, int r, uint16_t col)
{
    int cx, cy, dx, dy;

    /* main body minus corners */
    ui_fill(x + r, y, w - 2 * r, h, col);
    ui_fill(x, y + r, w, h - 2 * r, col);

    /* corners */
    for (cy = 0; cy < r; cy++) {
        for (cx = 0; cx < r; cx++) {
            dx = r - cx;
            dy = r - cy;
            if (dx * dx + dy * dy > r * r)
                continue;

            /* top-left */
            if (x + cx >= 0 && y + cy >= 0)
                pixels[(y + cy) * SCREEN_W + x + cx] = col;

            /* top-right */
            if (x + w - 1 - cx < SCREEN_W && y + cy >= 0)
                pixels[(y + cy) * SCREEN_W + x + w - 1 - cx] = col;

            /* bottom-left */
            if (x + cx >= 0 && y + h - 1 - cy < SCREEN_H)
                pixels[(y + h - 1 - cy) * SCREEN_W + x + cx] = col;

            /* bottom-right */
            if (x + w - 1 - cx < SCREEN_W && y + h - 1 - cy < SCREEN_H)
                pixels[(y + h - 1 - cy) * SCREEN_W + x + w - 1 - cx] = col;
        }
    }
}

void ui_text_pill(int x, int y, const char *str, uint16_t bg, uint16_t fg, int pad)
{
    int tw = font_measure_text(str);
    int th = FONT_CHAR_HEIGHT;
    int lpad = 6;
    int pw = tw + lpad + pad;
    int ph = th + pad;
    int px = x - lpad;
    int py = y - pad / 2;

    ui_pill(px, py, pw, ph, 8, bg);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, x, y, str, fg);
}

void ui_legend_btn(int x, int y, const char *str)
{
    int tw = font_measure_text(str);
    ui_pill(x - 4, y - 2, tw + 8, 20, 10, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, x, y, str, COL_TEXT);
}
