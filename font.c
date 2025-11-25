#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static stbtt_fontinfo font_info;
static unsigned char *font_buffer = NULL;
static float font_scale;
static int font_loaded = 0;

#define FONT_SIZE 20.0f

// Glyph cache - covers ASCII 32-127 (printable characters)
#define CACHE_START 32
#define CACHE_END 128
#define CACHE_SIZE (CACHE_END - CACHE_START)
#define MAX_GLYPH_WIDTH 24
#define MAX_GLYPH_HEIGHT 24

typedef struct {
    unsigned char bitmap[MAX_GLYPH_WIDTH * MAX_GLYPH_HEIGHT];
    int width;
    int height;
    int xoff;
    int yoff;
    int advance;
    int valid;
} CachedGlyph;

static CachedGlyph glyph_cache[CACHE_SIZE];
static int cache_initialized = 0;
static int cached_baseline = 0;

// Pre-render all printable ASCII glyphs to cache
static void init_glyph_cache(void) {
    if (!font_loaded) return;

    // Get baseline
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
    cached_baseline = (int)(ascent * font_scale);

    memset(glyph_cache, 0, sizeof(glyph_cache));

    for (int i = 0; i < CACHE_SIZE; i++) {
        char c = (char)(i + CACHE_START);

        // Convert lowercase to uppercase for caching
        if (c >= 'a' && c <= 'z') {
            // Lowercase letters point to uppercase cache entry
            glyph_cache[i].valid = 0; // Will use uppercase version
            continue;
        }

        int glyph_index = stbtt_FindGlyphIndex(&font_info, c);
        if (glyph_index == 0) {
            glyph_cache[i].valid = 0;
            continue;
        }

        // Get advance width
        int advance_width, left_side_bearing;
        stbtt_GetGlyphHMetrics(&font_info, glyph_index, &advance_width, &left_side_bearing);
        glyph_cache[i].advance = (int)(advance_width * font_scale);

        // Get glyph bitmap
        int width, height, xoff, yoff;
        unsigned char *bitmap = stbtt_GetGlyphBitmap(&font_info, 0, font_scale,
                                                      glyph_index, &width, &height, &xoff, &yoff);

        if (bitmap && width <= MAX_GLYPH_WIDTH && height <= MAX_GLYPH_HEIGHT) {
            glyph_cache[i].width = width;
            glyph_cache[i].height = height;
            glyph_cache[i].xoff = xoff;
            glyph_cache[i].yoff = yoff;
            glyph_cache[i].valid = 1;

            // Copy bitmap to cache
            memcpy(glyph_cache[i].bitmap, bitmap, width * height);
        }

        if (bitmap) {
            stbtt_FreeBitmap(bitmap, NULL);
        }
    }

    cache_initialized = 1;
}

// Internal function to load a font file
static int load_font_file(const char *font_filename) {
    // Free previous font if loaded
    if (font_buffer) {
        free(font_buffer);
        font_buffer = NULL;
        font_loaded = 0;
        cache_initialized = 0;
    }

    // Build search paths for the font
    char font_paths[2][256];
    snprintf(font_paths[0], sizeof(font_paths[0]), "/mnt/sda1/frogui/fonts/%s", font_filename);
    snprintf(font_paths[1], sizeof(font_paths[1]), "fonts/%s", font_filename);

    FILE *fp = NULL;
    for (int i = 0; i < 2; i++) {
        fp = fopen(font_paths[i], "rb");
        if (fp) break;
    }

    if (!fp) {
        return 0;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long font_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Allocate buffer and read font
    font_buffer = (unsigned char*)malloc(font_size);
    if (!font_buffer) {
        fclose(fp);
        return 0;
    }

    fread(font_buffer, 1, font_size, fp);
    fclose(fp);

    // Initialize font
    if (!stbtt_InitFont(&font_info, font_buffer, stbtt_GetFontOffsetForIndex(font_buffer, 0))) {
        free(font_buffer);
        font_buffer = NULL;
        return 0;
    }

    // Calculate scale for desired pixel height
    font_scale = stbtt_ScaleForPixelHeight(&font_info, FONT_SIZE);
    font_loaded = 1;

    // Pre-render all glyphs to cache
    init_glyph_cache();

    return 1;
}

void font_load_from_settings(const char *font_name) {
    const char *font_filename = NULL;
    float custom_size = FONT_SIZE;

    // Map font names to font files
    if (strcmp(font_name, "GamePocket") == 0) {
        font_filename = "GamePocket-Regular-ZeroKern.ttf";
        custom_size = 18.0f; // GamePocket at 18px
    } else if (strcmp(font_name, "Monogram") == 0) {
        font_filename = "monogram.ttf";
        custom_size = 16.0f; // Monogram works best at 16px
    } else {
        // Default to GamePocket
        font_filename = "GamePocket-Regular-ZeroKern.ttf";
        custom_size = 18.0f; // GamePocket at 18px
    }

    load_font_file(font_filename);

    // Recalculate scale if custom size is different
    if (custom_size != FONT_SIZE && font_loaded) {
        font_scale = stbtt_ScaleForPixelHeight(&font_info, custom_size);
        // Re-cache with new scale
        init_glyph_cache();
    }
}

void font_init(void) {
    // Load default font initially
    font_load_from_settings("GamePocket");
}

// Get cached glyph for a character (converts lowercase to uppercase)
static CachedGlyph* get_cached_glyph(char c) {
    // Convert to uppercase
    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }

    if (c < CACHE_START || c >= CACHE_END) {
        return NULL;
    }

    CachedGlyph *g = &glyph_cache[c - CACHE_START];
    return g->valid ? g : NULL;
}

void font_draw_char(uint16_t *framebuffer, int screen_width, int screen_height,
                   int x, int y, char c, uint16_t color) {
    if (!cache_initialized || !framebuffer) return;

    CachedGlyph *g = get_cached_glyph(c);
    if (!g) return;

    // Draw from cache - pure integer math
    for (int row = 0; row < g->height; row++) {
        int py = y + cached_baseline + g->yoff + row;
        if (py < 0 || py >= screen_height) continue;

        for (int col = 0; col < g->width; col++) {
            unsigned char alpha = g->bitmap[row * g->width + col];
            if (alpha > 127) {
                int px = x + g->xoff + col;
                if (px >= 0 && px < screen_width) {
                    framebuffer[py * screen_width + px] = color;
                }
            }
        }
    }
}

void font_draw_text(uint16_t *framebuffer, int screen_width, int screen_height,
                   int x, int y, const char *text, uint16_t color) {
    if (!cache_initialized || !framebuffer || !text) return;

    int start_x = x;

    while (*text) {
        if (*text == '\n') {
            y += FONT_SIZE + 4;  // Line spacing
            x = start_x;
            text++;
            continue;
        }

        char c = *text;

        // Convert to uppercase
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }

        CachedGlyph *g = get_cached_glyph(c);

        if (g) {
            // Draw from cache
            font_draw_char(framebuffer, screen_width, screen_height, x, y, c, color);
            x += g->advance;
        } else {
            // Space or unknown character
            x += FONT_CHAR_SPACING;
        }

        text++;
    }
}

int font_measure_text(const char *text) {
    if (!text || !cache_initialized) return 0;

    int width = 0;

    while (*text) {
        // Skip newlines
        if (*text == '\n') {
            text++;
            continue;
        }

        char c = *text;

        // Convert to uppercase
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }

        CachedGlyph *g = get_cached_glyph(c);

        if (g) {
            width += g->advance;
        } else {
            // Space or unknown character
            width += FONT_CHAR_SPACING;
        }

        text++;
    }

    return width;
}
