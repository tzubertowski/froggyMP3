#ifndef FROGGY_UI_H
#define FROGGY_UI_H

#include <stdint.h>

#define SCREEN_W  320
#define SCREEN_H  240

/* RGB565 conversion */
#define RGB565(r, g, b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

/* Color palette - dark theme with accent */
#define COL_BG          RGB565(18, 18, 22)
#define COL_TEXT        RGB565(240, 240, 245)
#define COL_SELECT_BG   RGB565(70, 130, 220)
#define COL_SELECT_FG   RGB565(255, 255, 255)
#define COL_DIM         RGB565(120, 120, 130)
#define COL_PILL        RGB565(40, 40, 48)
#define COL_BAR_BG      RGB565(50, 50, 60)
#define COL_BAR_FG      RGB565(70, 130, 220)
#define COL_HEADER_BG   RGB565(28, 28, 34)
#define COL_PLAYING     RGB565(100, 200, 130)
#define COL_ROW_ALT     RGB565(24, 24, 30)

/* Global framebuffer - defined in libretro.c */
extern uint16_t pixels[];

/* Drawing primitives */
void ui_clear(uint16_t col);
void ui_fill(int x, int y, int w, int h, uint16_t col);
void ui_pill(int x, int y, int w, int h, int r, uint16_t col);
void ui_text_pill(int x, int y, const char *str, uint16_t bg, uint16_t fg, int pad);
void ui_legend_btn(int x, int y, const char *str);

#endif
