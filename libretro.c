#include "libretro.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#ifdef SF2000
#include "dirent.h"
#else
#include <dirent.h>
#endif
#include <retro_endianness.h>
#include <streams/file_stream.h>
#include <libretro.h>

#include "platform.h"
#include "font.h"
#include "libmad/libmad.h"

#ifdef ABGR1555
#define GP_RGB24(r, g, b) (((((b >> 3)) & 0x1f) << 10) | ((((g >> 3)) & 0x1f) << 5) | (((r >> 3)) & 0x1f))
#else
#define GP_RGB24(r, g, b) (((((r >> 3)) & 0x1f) << 11) | ((((g >> 2)) & 0x3f) << 6) | (((b >> 3)) & 0x1f))
#endif

#define FPS 50

// MinUI Style Colors (black bg, white text, white pillbox selection with black text)
#define RGB565(r, g, b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
#define COLOR_BG          RGB565(0, 0, 0)
#define COLOR_TEXT        RGB565(255, 255, 255)
#define COLOR_SELECT_BG   RGB565(255, 255, 255)
#define COLOR_SELECT_TEXT RGB565(0, 0, 0)
#define COLOR_HEADER      RGB565(132, 132, 132)
#define COLOR_FOLDER      RGB565(255, 255, 255)
#define COLOR_LEGEND      RGB565(255, 255, 255)
#define COLOR_LEGEND_BG   RGB565(33, 33, 33)
#define COLOR_DISABLED    RGB565(132, 132, 132)
#define COLOR_PROGRESS_BG RGB565(33, 33, 33)
#define COLOR_PROGRESS    RGB565(255, 255, 255)

// Screen dimensions
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

// UI Constants
#define MAX_FILES 256
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 128
#define VISIBLE_ITEMS 8
#define ITEM_HEIGHT 24
#define HEADER_HEIGHT 30
#define START_Y 36
#define PADDING 10

// App states
typedef enum {
    STATE_BROWSER,
    STATE_PLAYING
} AppState;

// File entry
typedef struct {
    char name[MAX_NAME_LEN];
    int is_dir;
} FileEntry;

// Browser state
static AppState app_state = STATE_BROWSER;
static char current_path[MAX_PATH_LEN] = "/mnt/sda1/ROMS/MP3";
static FileEntry file_list[MAX_FILES];
static int file_count = 0;
static int selected_index = 0;
static int scroll_offset = 0;

// MP3 player state
static void *mp3Mad     = NULL;
static char *mp3        = NULL;
static u32 mp3Length    = 0;
static u32 mp3Position  = 0;
static u32 mp3StartPosition = 0;
static s16 *soundBuffer = NULL;
static u16 soundEnd     = 0;
static u8 kpause        = 0;
static u32 mp3SampleRate = 44100;
static u32 mp3Bitrate    = 0;
static u32 mp3Channels   = 2;
static char current_song[MAX_NAME_LEN] = "";

// Framebuffer
u16 pixels[SCREEN_WIDTH * SCREEN_HEIGHT];

// Callbacks
static void fallback_log(enum retro_log_level level, const char *fmt, ...);
retro_log_printf_t log_cb = fallback_log;
retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

int width, height;

#pragma pack(1)
typedef struct {
   u8 identifier[3];
   u16 version;
   u8 flags;
   u8 lengthSyncSafe[4];
} id3Header;
#pragma pack()

// Input tracking
static int key_pressed[16] = {0};
static int key_just_pressed[16] = {0};

// UI dirty flag - only re-render when needed
static int ui_dirty = 1;

// Scrolling text for player
static int title_scroll_offset = 0;
static int title_scroll_delay = 0;
static int title_needs_scroll = 0;
static char scrolling_title[MAX_NAME_LEN + 8]; // Extra space for padding
#define SCROLL_DELAY_FRAMES 30  // Pause at start/end
#define SCROLL_SPEED 1          // Pixels per frame
#define TITLE_MAX_WIDTH 280     // Max visible width for title

// Playback modes
typedef enum {
    PLAYBACK_NORMAL,   // Play once, stop at end
    PLAYBACK_NEXT,     // Play next song when current ends
    PLAYBACK_REPEAT,   // Repeat current song
    PLAYBACK_RANDOM,   // Play random song when current ends
    PLAYBACK_MODE_COUNT
} PlaybackMode;

static PlaybackMode playback_mode = PLAYBACK_NEXT;
static int current_song_index = -1;  // Index in file_list of currently playing song

// Forward declarations
static void scan_directory(void);
static void render_browser(void);
static void render_player(void);
static void clear_screen(uint16_t color);
static void fill_rect(int x, int y, int w, int h, uint16_t color);
static void render_rounded_rect(int x, int y, int w, int h, int radius, uint16_t color);
static void render_text_pillbox(int x, int y, const char *text, uint16_t bg_color, uint16_t text_color, int padding);
static int load_mp3_file(const char *path);
static void unload_mp3(void);
static void seek_mp3(int seconds);
static void play_song_at_index(int index);
static void play_next_song(void);
static void play_prev_song(void);
static void play_random_song(void);

// Compare function for sorting (directories first, then alphabetical)
static int compare_entries(const void *a, const void *b) {
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;

    // ".." always first
    if (strcmp(ea->name, "..") == 0) return -1;
    if (strcmp(eb->name, "..") == 0) return 1;

    // Directories before files
    if (ea->is_dir && !eb->is_dir) return -1;
    if (!ea->is_dir && eb->is_dir) return 1;

    // Alphabetical (case insensitive)
    return strcasecmp(ea->name, eb->name);
}

// Check if file has .mp3 extension
static int is_mp3_file(const char *name) {
    int len = strlen(name);
    if (len < 4) return 0;
    const char *ext = name + len - 4;
    return (strcasecmp(ext, ".mp3") == 0);
}

// Scan current directory for MP3 files and subdirectories
static void scan_directory(void) {
    DIR *dir;
    struct dirent *ent;

    file_count = 0;
    selected_index = 0;
    scroll_offset = 0;

    dir = opendir(current_path);
    if (!dir) {
        log_cb(RETRO_LOG_ERROR, "[froggymp3] Failed to open directory: %s\n", current_path);
        return;
    }

    // Add parent directory entry if not at root
    if (strcmp(current_path, "/") != 0 && strcmp(current_path, "/mnt/sda1") != 0) {
        strcpy(file_list[file_count].name, "..");
        file_list[file_count].is_dir = 1;
        file_count++;
    }

    while ((ent = readdir(dir)) != NULL && file_count < MAX_FILES) {
        // Skip hidden files and . / ..
        if (ent->d_name[0] == '.') continue;

        // Check if directory or MP3 file
        if (ent->d_type == DT_DIR || ent->d_type == DTYPE_DIRECTORY) {
            strncpy(file_list[file_count].name, ent->d_name, MAX_NAME_LEN - 1);
            file_list[file_count].name[MAX_NAME_LEN - 1] = '\0';
            file_list[file_count].is_dir = 1;
            file_count++;
        } else if (is_mp3_file(ent->d_name)) {
            strncpy(file_list[file_count].name, ent->d_name, MAX_NAME_LEN - 1);
            file_list[file_count].name[MAX_NAME_LEN - 1] = '\0';
            file_list[file_count].is_dir = 0;
            file_count++;
        }
    }

    closedir(dir);

    // Sort entries
    if (file_count > 1) {
        qsort(file_list, file_count, sizeof(FileEntry), compare_entries);
    }

    log_cb(RETRO_LOG_INFO, "[froggymp3] Found %d entries in %s\n", file_count, current_path);
}

// Navigate to parent directory
static void go_parent_directory(void) {
    char *last_slash = strrchr(current_path, '/');
    if (last_slash && last_slash != current_path) {
        *last_slash = '\0';
    }
    scan_directory();
}

// Navigate into a directory
static void enter_directory(const char *name) {
    if (strcmp(name, "..") == 0) {
        go_parent_directory();
        return;
    }

    char new_path[MAX_PATH_LEN];
    snprintf(new_path, MAX_PATH_LEN, "%s/%s", current_path, name);
    strncpy(current_path, new_path, MAX_PATH_LEN - 1);
    current_path[MAX_PATH_LEN - 1] = '\0';
    scan_directory();
}

// Clear screen
static void clear_screen(uint16_t color) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        pixels[i] = color;
    }
}

// Fill rectangle
static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int py = y; py < y + h && py < SCREEN_HEIGHT; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < SCREEN_WIDTH; px++) {
            if (px < 0) continue;
            pixels[py * SCREEN_WIDTH + px] = color;
        }
    }
}

// Rounded rectangle (pill shape)
static void render_rounded_rect(int x, int y, int w, int h, int radius, uint16_t color) {
    // Draw main body (excluding corners)
    fill_rect(x + radius, y, w - 2 * radius, h, color);
    fill_rect(x, y + radius, w, h - 2 * radius, color);

    // Draw rounded corners using circle approximation
    for (int corner_y = 0; corner_y < radius; corner_y++) {
        for (int corner_x = 0; corner_x < radius; corner_x++) {
            int dx = radius - corner_x;
            int dy = radius - corner_y;
            int dist_sq = dx * dx + dy * dy;
            int radius_sq = radius * radius;

            if (dist_sq <= radius_sq) {
                // Top-left corner
                int px = x + corner_x;
                int py_pos = y + corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py_pos >= 0 && py_pos < SCREEN_HEIGHT) {
                    pixels[py_pos * SCREEN_WIDTH + px] = color;
                }

                // Top-right corner
                px = x + w - 1 - corner_x;
                py_pos = y + corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py_pos >= 0 && py_pos < SCREEN_HEIGHT) {
                    pixels[py_pos * SCREEN_WIDTH + px] = color;
                }

                // Bottom-left corner
                px = x + corner_x;
                py_pos = y + h - 1 - corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py_pos >= 0 && py_pos < SCREEN_HEIGHT) {
                    pixels[py_pos * SCREEN_WIDTH + px] = color;
                }

                // Bottom-right corner
                px = x + w - 1 - corner_x;
                py_pos = y + h - 1 - corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py_pos >= 0 && py_pos < SCREEN_HEIGHT) {
                    pixels[py_pos * SCREEN_WIDTH + px] = color;
                }
            }
        }
    }
}

// Text pillbox (rounded background behind text)
static void render_text_pillbox(int x, int y, const char *text, uint16_t bg_color, uint16_t text_color, int padding) {
    int text_width = font_measure_text(text);
    int text_height = FONT_CHAR_HEIGHT;

    int left_padding = 6;
    int pillbox_width = text_width + left_padding + padding;
    int pillbox_height = text_height + padding;
    int pillbox_x = x - left_padding;
    int pillbox_y = y - (padding / 2);

    render_rounded_rect(pillbox_x, pillbox_y, pillbox_width, pillbox_height, 8, bg_color);
    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, x, y, text, text_color);
}

// Render legend button with pillbox
static void render_legend_button(int x, int y, const char *text) {
    int text_width = font_measure_text(text);
    render_rounded_rect(x - 4, y - 2, text_width + 8, 20, 10, COLOR_LEGEND_BG);
    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, x, y, text, COLOR_LEGEND);
}

// Draw text with horizontal clipping (for scrolling)
static void draw_text_clipped(int x, int y, const char *text, uint16_t color,
                              int clip_x, int clip_width, int scroll_offset) {
    // Draw text at offset position, clipped to region
    int draw_x = x - scroll_offset;

    // Simple approach: draw full text then we rely on screen bounds
    // For proper clipping, we'd need per-character logic
    // This works because font_draw_char already clips to screen bounds

    // Temporarily adjust draw position
    int text_width = font_measure_text(text);

    // Only draw if some part is visible
    if (draw_x + text_width < clip_x || draw_x > clip_x + clip_width) {
        return;
    }

    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, draw_x, y, text, color);
}

// Render file browser
static void render_browser(void) {
    clear_screen(COLOR_BG);

    // Header
    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 8, "MP3", COLOR_HEADER);

    // File list
    int visible_start = scroll_offset;
    int visible_end = scroll_offset + VISIBLE_ITEMS;
    if (visible_end > file_count) visible_end = file_count;

    for (int i = visible_start; i < visible_end; i++) {
        int y = START_Y + (i - scroll_offset) * ITEM_HEIGHT;
        int is_selected = (i == selected_index);

        // File/folder display name
        char display_name[MAX_NAME_LEN + 4];
        if (file_list[i].is_dir) {
            snprintf(display_name, sizeof(display_name), "[%s]", file_list[i].name);
        } else {
            strncpy(display_name, file_list[i].name, MAX_NAME_LEN);
            display_name[MAX_NAME_LEN] = '\0';
        }

        // Truncate long names
        if (strlen(display_name) > 26) {
            display_name[23] = '.';
            display_name[24] = '.';
            display_name[25] = '.';
            display_name[26] = '\0';
        }

        // Draw with pillbox if selected
        if (is_selected) {
            render_text_pillbox(PADDING, y, display_name, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
        } else {
            uint16_t text_color = file_list[i].is_dir ? COLOR_FOLDER : COLOR_TEXT;
            font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, display_name, text_color);
        }
    }

    // Scroll indicator
    if (file_count > VISIBLE_ITEMS) {
        int scroll_height = (VISIBLE_ITEMS * ITEM_HEIGHT * VISIBLE_ITEMS) / file_count;
        if (scroll_height < 10) scroll_height = 10;
        int max_scroll = file_count - VISIBLE_ITEMS;
        if (max_scroll > 0) {
            int scroll_y = START_Y + (scroll_offset * (VISIBLE_ITEMS * ITEM_HEIGHT - scroll_height)) / max_scroll;
            fill_rect(SCREEN_WIDTH - 4, scroll_y, 3, scroll_height, COLOR_DISABLED);
        }
    }

    // Legend at bottom - right aligned
    int legend_y = SCREEN_HEIGHT - 24;
    const char *select_text = " A - SELECT ";
    int select_width = font_measure_text(select_text);
    render_legend_button(SCREEN_WIDTH - select_width - 14, legend_y, select_text);
}

// Render player screen
static void render_player(void) {
    clear_screen(COLOR_BG);

    // Header
    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 8, "NOW PLAYING", COLOR_HEADER);

    // Song name - scrolling if too long
    int title_y = 70;
    int title_width = font_measure_text(scrolling_title);

    if (title_width <= TITLE_MAX_WIDTH) {
        // Center if fits
        int text_x = (SCREEN_WIDTH - title_width) / 2;
        font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, text_x, title_y, scrolling_title, COLOR_TEXT);
    } else {
        // Scrolling - draw centered on the visible portion
        int text_x = (SCREEN_WIDTH - TITLE_MAX_WIDTH) / 2;
        draw_text_clipped(text_x + title_scroll_offset, title_y, scrolling_title, COLOR_TEXT,
                          text_x, TITLE_MAX_WIDTH, title_scroll_offset);
    }

    // Progress bar with rounded ends
    int bar_y = 120;
    int bar_height = 8;
    int bar_width = SCREEN_WIDTH - PADDING * 4;
    int bar_x = PADDING * 2;

    // Background bar
    render_rounded_rect(bar_x, bar_y, bar_width, bar_height, 4, COLOR_PROGRESS_BG);

    // Progress fill
    if (mp3Length > mp3StartPosition) {
        int progress_width = ((mp3Position - mp3StartPosition) * bar_width) / (mp3Length - mp3StartPosition);
        if (progress_width > 8) {
            render_rounded_rect(bar_x, bar_y, progress_width, bar_height, 4, COLOR_PROGRESS);
        } else if (progress_width > 0) {
            fill_rect(bar_x, bar_y, progress_width, bar_height, COLOR_PROGRESS);
        }
    }

    // Seek and Play/Pause controls in a row
    int controls_y = 153;

    // Left seek: "< 5S"
    const char *seek_left = "< 5S";
    int seek_left_width = font_measure_text(seek_left);
    int seek_left_x = 30;
    render_rounded_rect(seek_left_x - 4, controls_y - 3, seek_left_width + 8, 22, 10, COLOR_LEGEND_BG);
    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, seek_left_x, controls_y, seek_left, COLOR_LEGEND);

    // Play/Pause status (centered)
    const char *status = kpause ? " PAUSED " : " PLAYING ";
    int status_width = font_measure_text(status);
    int status_x = (SCREEN_WIDTH - status_width) / 2;
    render_rounded_rect(status_x - 4, controls_y - 3, status_width + 8, 22, 10,
                       kpause ? COLOR_LEGEND_BG : COLOR_SELECT_BG);
    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, status_x, controls_y, status,
                  kpause ? COLOR_DISABLED : COLOR_SELECT_TEXT);

    // Right seek: "5S >"
    const char *seek_right = "5S >";
    int seek_right_width = font_measure_text(seek_right);
    int seek_right_x = SCREEN_WIDTH - seek_right_width - 30;
    render_rounded_rect(seek_right_x - 4, controls_y - 3, seek_right_width + 8, 22, 10, COLOR_LEGEND_BG);
    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, seek_right_x, controls_y, seek_right, COLOR_LEGEND);

    // Playback mode selector (below controls)
    int mode_y = 185;
    const char *mode_text;
    switch (playback_mode) {
        case PLAYBACK_NORMAL: mode_text = "STOP AT END"; break;
        case PLAYBACK_NEXT:   mode_text = "PLAY NEXT"; break;
        case PLAYBACK_REPEAT: mode_text = "REPEAT"; break;
        case PLAYBACK_RANDOM: mode_text = "RANDOM"; break;
        default:              mode_text = "PLAY NEXT"; break;
    }
    int mode_width = font_measure_text(mode_text);
    int mode_x = (SCREEN_WIDTH - mode_width) / 2;
    render_rounded_rect(mode_x - 6, mode_y - 3, mode_width + 12, 22, 10, COLOR_LEGEND_BG);
    font_draw_text(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, mode_x, mode_y, mode_text, COLOR_LEGEND);

    // Legend at bottom
    int legend_y = SCREEN_HEIGHT - 24;
    render_legend_button(PADDING, legend_y, " Y/X - PREV/NEXT ");
    render_legend_button(SCREEN_WIDTH - 85, legend_y, " START - BACK ");
}

// Load MP3 file
static int load_mp3_file(const char *path) {
    FILE *fic = NULL;
    log_cb(RETRO_LOG_INFO, "[froggymp3] Loading: %s\n", path);

    fic = fopen(path, "rb");
    if (!fic) {
        log_cb(RETRO_LOG_ERROR, "[froggymp3] Failed to open file\n");
        return 0;
    }

    fseek(fic, 0, SEEK_END);
    mp3Length = ftell(fic);
    fseek(fic, 0, SEEK_SET);

    if (mp3) {
        free(mp3);
        mp3 = NULL;
    }

    mp3 = (char *)malloc(mp3Length);
    if (!mp3) {
        log_cb(RETRO_LOG_ERROR, "[froggymp3] Failed to allocate memory\n");
        fclose(fic);
        return 0;
    }

    fread(mp3, 1, mp3Length, fic);
    fclose(fic);

    mp3Position = 0;

    // Skip ID3 tag
    if (mp3Length > 10) {
        id3Header header;
        memcpy(&header, mp3, 10);

        if ((header.identifier[0] == 0x49) && (header.identifier[1] == 0x44) && (header.identifier[2] == 0x33)) {
            mp3Position = (header.lengthSyncSafe[0] & 0x7f);
            mp3Position = (mp3Position << 7) | (header.lengthSyncSafe[1] & 0x7f);
            mp3Position = (mp3Position << 7) | (header.lengthSyncSafe[2] & 0x7f);
            mp3Position = (mp3Position << 7) | (header.lengthSyncSafe[3] & 0x7f);
            mp3Position = mp3Position + 10;
        }
    }

    mp3StartPosition = mp3Position;

    if (mp3Mad) {
        mad_uninit(mp3Mad);
    }
    mp3Mad = mad_init();
    if (!mp3Mad) {
        log_cb(RETRO_LOG_ERROR, "[froggymp3] mad_init() failed\n");
        return 0;
    }

    soundEnd = 0;

    // Decode first frame to get metadata
    {
        int read = 0, done = 0;
        int length = (mp3Length - mp3Position > 4096) ? 4096 : (mp3Length - mp3Position);
        char *tempBuffer = (char *)malloc(32768);

        if (tempBuffer) {
            int retour = mad_decode(mp3Mad, mp3 + mp3Position, length,
                                    tempBuffer, 32768, &read, &done, 16, 0);

            if (retour == MAD_OK || done > 0) {
                mp3SampleRate = mad_get_samplerate(mp3Mad);
                mp3Bitrate = mad_get_bitrate(mp3Mad);
                mp3Channels = mad_get_channels(mp3Mad);
                log_cb(RETRO_LOG_INFO, "[froggymp3] MP3: %dHz, %dbps, %dch\n",
                       mp3SampleRate, mp3Bitrate, mp3Channels);
            }

            free(tempBuffer);

            // Reset decoder
            mad_uninit(mp3Mad);
            mp3Mad = mad_init();
            mp3Position = mp3StartPosition;
        }
    }

    kpause = 0;
    return 1;
}

// Initialize scrolling title from current_song
static void init_scrolling_title(void) {
    // Copy song name and remove .mp3 extension
    strncpy(scrolling_title, current_song, MAX_NAME_LEN - 1);
    scrolling_title[MAX_NAME_LEN - 1] = '\0';

    int len = strlen(scrolling_title);
    if (len > 4 && strcasecmp(scrolling_title + len - 4, ".mp3") == 0) {
        scrolling_title[len - 4] = '\0';
    }

    // Check if scrolling is needed
    int title_width = font_measure_text(scrolling_title);
    title_needs_scroll = (title_width > TITLE_MAX_WIDTH);
    title_scroll_offset = 0;
    title_scroll_delay = SCROLL_DELAY_FRAMES;
}

// Unload current MP3
static void unload_mp3(void) {
    if (mp3Mad) {
        mad_uninit(mp3Mad);
        mp3Mad = NULL;
    }
    if (mp3) {
        free(mp3);
        mp3 = NULL;
    }
    mp3Length = 0;
    mp3Position = 0;
    soundEnd = 0;
}

// Seek MP3 by seconds (positive = forward, negative = backward)
static void seek_mp3(int seconds) {
    if (!mp3 || mp3Length == 0 || mp3Bitrate == 0) return;

    // Estimate bytes per second from bitrate
    int bytes_per_second = mp3Bitrate / 8;
    int seek_bytes = seconds * bytes_per_second;

    int new_position = (int)mp3Position + seek_bytes;

    if (new_position < (int)mp3StartPosition) {
        new_position = mp3StartPosition;
    }
    if (new_position >= (int)mp3Length) {
        new_position = mp3Length - 1024;
        if (new_position < (int)mp3StartPosition) {
            new_position = mp3StartPosition;
        }
    }

    mp3Position = new_position;
    soundEnd = 0;

    // Reinitialize decoder
    if (mp3Mad) {
        mad_uninit(mp3Mad);
        mp3Mad = mad_init();
    }

    log_cb(RETRO_LOG_INFO, "[froggymp3] Seek to position %u\n", mp3Position);
}

// Simple pseudo-random number generator
static unsigned int rand_seed = 12345;
static int simple_rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed >> 16) & 0x7FFF;
}

// Find index of a song in file_list by name
static int find_song_index(const char *name) {
    for (int i = 0; i < file_count; i++) {
        if (!file_list[i].is_dir && strcmp(file_list[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// Count MP3 files in file_list
static int count_mp3_files(void) {
    int count = 0;
    for (int i = 0; i < file_count; i++) {
        if (!file_list[i].is_dir) count++;
    }
    return count;
}

// Get next MP3 index (skipping directories)
static int get_next_mp3_index(int current) {
    int start = (current < 0) ? 0 : current + 1;
    for (int i = start; i < file_count; i++) {
        if (!file_list[i].is_dir) return i;
    }
    // Wrap around to beginning
    for (int i = 0; i < start && i < file_count; i++) {
        if (!file_list[i].is_dir) return i;
    }
    return -1;
}

// Get previous MP3 index (skipping directories)
static int get_prev_mp3_index(int current) {
    int start = (current <= 0) ? file_count - 1 : current - 1;
    for (int i = start; i >= 0; i--) {
        if (!file_list[i].is_dir) return i;
    }
    // Wrap around to end
    for (int i = file_count - 1; i > start; i--) {
        if (!file_list[i].is_dir) return i;
    }
    return -1;
}

// Get random MP3 index (different from current if possible)
static int get_random_mp3_index(int current) {
    int mp3_count = count_mp3_files();
    if (mp3_count == 0) return -1;
    if (mp3_count == 1) return get_next_mp3_index(-1);

    int attempts = 10;
    while (attempts-- > 0) {
        int target = simple_rand() % mp3_count;
        int count = 0;
        for (int i = 0; i < file_count; i++) {
            if (!file_list[i].is_dir) {
                if (count == target && i != current) {
                    return i;
                }
                count++;
            }
        }
    }
    // Fallback to next
    return get_next_mp3_index(current);
}

// Play song at given index in file_list
static void play_song_at_index(int index) {
    if (index < 0 || index >= file_count || file_list[index].is_dir) {
        return;
    }

    char full_path[MAX_PATH_LEN];
    snprintf(full_path, MAX_PATH_LEN, "%s/%s", current_path, file_list[index].name);

    if (load_mp3_file(full_path)) {
        strncpy(current_song, file_list[index].name, MAX_NAME_LEN - 1);
        current_song[MAX_NAME_LEN - 1] = '\0';
        current_song_index = index;
        init_scrolling_title();
        app_state = STATE_PLAYING;
        ui_dirty = 1;
    }
}

// Play next song
static void play_next_song(void) {
    int next = get_next_mp3_index(current_song_index);
    if (next >= 0) {
        play_song_at_index(next);
    } else {
        // No more songs
        unload_mp3();
        current_song[0] = '\0';
        current_song_index = -1;
        app_state = STATE_BROWSER;
        ui_dirty = 1;
    }
}

// Play previous song
static void play_prev_song(void) {
    int prev = get_prev_mp3_index(current_song_index);
    if (prev >= 0) {
        play_song_at_index(prev);
    }
}

// Play random song
static void play_random_song(void) {
    int next = get_random_mp3_index(current_song_index);
    if (next >= 0) {
        play_song_at_index(next);
    } else {
        unload_mp3();
        current_song[0] = '\0';
        current_song_index = -1;
        app_state = STATE_BROWSER;
        ui_dirty = 1;
    }
}

static void fallback_log(enum retro_log_level level, const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static void update_variables(void) { }

void retro_init(void) {
    log_cb(RETRO_LOG_INFO, "[froggymp3] retro_init()\n");
    update_variables();

    soundBuffer = (s16 *)malloc(32768);
    width = SCREEN_WIDTH;
    height = SCREEN_HEIGHT;

    // Initialize font system
    font_init();

    // Initialize browser
    scan_directory();

    log_cb(RETRO_LOG_INFO, "[froggymp3] retro_init() complete\n");
}

void retro_deinit(void) {
    unload_mp3();
    if (soundBuffer) {
        free(soundBuffer);
        soundBuffer = NULL;
    }
}

unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port;
    (void)device;
}

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "froggyMP3";
    info->need_fullpath    = false;
    info->valid_extensions = "mp3";
#ifdef GIT_VERSION
    info->library_version  = "git" GIT_VERSION;
#else
    info->library_version  = "1.0";
#endif
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps            = FPS;
    info->timing.sample_rate    = (double)mp3SampleRate;
    info->geometry.base_width   = SCREEN_WIDTH;
    info->geometry.base_height  = SCREEN_HEIGHT;
    info->geometry.max_width    = SCREEN_WIDTH;
    info->geometry.max_height   = SCREEN_HEIGHT;
    info->geometry.aspect_ratio = 4.0f/3.0f;
}

void retro_set_environment(retro_environment_t cb) {
    struct retro_vfs_interface_info vfs_iface_info;
    struct retro_log_callback logging;
    static const struct retro_controller_description port[] = {
        {"RetroPad", RETRO_DEVICE_JOYPAD},
    };
    static const struct retro_controller_info ports[] = {
        {port, 1},
        {NULL, 0},
    };

    environ_cb = cb;

    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;

    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);

    vfs_iface_info.required_interface_version = 2;
    vfs_iface_info.iface = NULL;
    if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
        filestream_vfs_init(&vfs_iface_info);
}

void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_reset(void) { }

// Calculate samples needed per frame
#define NEEDFRAME_FOR_RATE(rate) ((rate) / FPS)
#define NEEDBYTE_FOR_RATE(rate)  (NEEDFRAME_FOR_RATE(rate) * 4)

void retro_run(void) {
    static bool updated = false;

    u32 needFrame = NEEDFRAME_FOR_RATE(mp3SampleRate);
    u32 needByte = NEEDBYTE_FOR_RATE(mp3SampleRate);

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        update_variables();

    input_poll_cb();

    // Track button states
    int buttons[] = {
        RETRO_DEVICE_ID_JOYPAD_A,      // 0
        RETRO_DEVICE_ID_JOYPAD_B,      // 1
        RETRO_DEVICE_ID_JOYPAD_UP,     // 2
        RETRO_DEVICE_ID_JOYPAD_DOWN,   // 3
        RETRO_DEVICE_ID_JOYPAD_LEFT,   // 4
        RETRO_DEVICE_ID_JOYPAD_RIGHT,  // 5
        RETRO_DEVICE_ID_JOYPAD_START,  // 6
        RETRO_DEVICE_ID_JOYPAD_SELECT, // 7
        RETRO_DEVICE_ID_JOYPAD_L,      // 8
        RETRO_DEVICE_ID_JOYPAD_R,      // 9
        RETRO_DEVICE_ID_JOYPAD_Y,      // 10
        RETRO_DEVICE_ID_JOYPAD_X       // 11
    };

    for (int i = 0; i < 12; i++) {
        int pressed = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, buttons[i]);
        key_just_pressed[i] = (pressed && !key_pressed[i]);
        key_pressed[i] = pressed;
    }

    // Handle input based on current state
    if (app_state == STATE_BROWSER) {
        // UP - move selection up
        if (key_just_pressed[2] && selected_index > 0) {
            selected_index--;
            if (selected_index < scroll_offset) {
                scroll_offset = selected_index;
            }
            ui_dirty = 1;
        }

        // DOWN - move selection down
        if (key_just_pressed[3] && selected_index < file_count - 1) {
            selected_index++;
            if (selected_index >= scroll_offset + VISIBLE_ITEMS) {
                scroll_offset = selected_index - VISIBLE_ITEMS + 1;
            }
            ui_dirty = 1;
        }

        // A or START - select item
        if ((key_just_pressed[0] || key_just_pressed[6]) && file_count > 0) {
            FileEntry *entry = &file_list[selected_index];
            if (entry->is_dir) {
                enter_directory(entry->name);
                ui_dirty = 1;
            } else {
                // Load and play MP3 using play_song_at_index
                play_song_at_index(selected_index);
            }
        }

        // B - go back / parent directory
        if (key_just_pressed[1]) {
            go_parent_directory();
            ui_dirty = 1;
        }

        if (ui_dirty) {
            render_browser();
            ui_dirty = 0;
        }
    }
    else if (app_state == STATE_PLAYING) {
        // A - toggle play/pause
        if (key_just_pressed[0]) {
            kpause = !kpause;
            ui_dirty = 1;
        }

        // L - rewind 5 seconds
        if (key_just_pressed[8]) {
            seek_mp3(-5);
            ui_dirty = 1;
        }

        // R - forward 5 seconds
        if (key_just_pressed[9]) {
            seek_mp3(5);
            ui_dirty = 1;
        }

        // LEFT - rewind 5 seconds
        if (key_just_pressed[4]) {
            seek_mp3(-5);
            ui_dirty = 1;
        }

        // RIGHT - forward 5 seconds
        if (key_just_pressed[5]) {
            seek_mp3(5);
            ui_dirty = 1;
        }

        // UP - cycle playback mode backward
        if (key_just_pressed[2]) {
            if (playback_mode == 0) {
                playback_mode = PLAYBACK_MODE_COUNT - 1;
            } else {
                playback_mode--;
            }
            ui_dirty = 1;
        }

        // DOWN - cycle playback mode forward
        if (key_just_pressed[3]) {
            playback_mode = (playback_mode + 1) % PLAYBACK_MODE_COUNT;
            ui_dirty = 1;
        }

        // Y - play previous song
        if (key_just_pressed[10]) {
            if (playback_mode == PLAYBACK_RANDOM) {
                play_random_song();
            } else {
                play_prev_song();
            }
        }

        // X - play next song
        if (key_just_pressed[11]) {
            if (playback_mode == PLAYBACK_RANDOM) {
                play_random_song();
            } else {
                play_next_song();
            }
        }

        // B or START - return to browser (keep playing)
        if (key_just_pressed[1] || key_just_pressed[6]) {
            app_state = STATE_BROWSER;
            ui_dirty = 1;
        }

        // Update scrolling title animation
        if (title_needs_scroll) {
            if (title_scroll_delay > 0) {
                title_scroll_delay--;
                if (title_scroll_delay == 0 && title_needs_scroll == 2) {
                    // Reset to beginning after pause at end
                    title_scroll_offset = 0;
                    title_scroll_delay = SCROLL_DELAY_FRAMES;
                    title_needs_scroll = 1;
                    ui_dirty = 1;
                }
            } else {
                int title_width = font_measure_text(scrolling_title);
                int max_scroll = title_width - TITLE_MAX_WIDTH;

                title_scroll_offset += SCROLL_SPEED;

                if (title_scroll_offset >= max_scroll) {
                    // Reached end, pause then reset
                    title_scroll_offset = max_scroll;
                    title_scroll_delay = SCROLL_DELAY_FRAMES;
                    title_needs_scroll = 2; // Flag to reset after delay
                }
                ui_dirty = 1;
            }
        }

        if (ui_dirty) {
            render_player();
            ui_dirty = 0;
        }

        // Play audio if not paused and file loaded
        if (!kpause && mp3Mad && mp3) {
            int error = 0;

            while (soundEnd <= needByte) {
                int length = 2048;
                int read;
                int retour;
                int done;
                int resolution = 16;
                int halfsamplerate = 0;

                if (mp3Position + length > mp3Length) {
                    length = mp3Length - mp3Position;
                    if (length <= 128) {
                        // Song ended - handle based on playback mode
                        log_cb(RETRO_LOG_INFO, "[froggymp3] Song ended, mode=%d\n", playback_mode);

                        switch (playback_mode) {
                            case PLAYBACK_REPEAT:
                                // Restart current song
                                mp3Position = mp3StartPosition;
                                soundEnd = 0;
                                if (mp3Mad) {
                                    mad_uninit(mp3Mad);
                                    mp3Mad = mad_init();
                                }
                                ui_dirty = 1;
                                break;

                            case PLAYBACK_NEXT:
                                play_next_song();
                                break;

                            case PLAYBACK_RANDOM:
                                play_random_song();
                                break;

                            case PLAYBACK_NORMAL:
                            default:
                                // Stop playback
                                unload_mp3();
                                current_song[0] = '\0';
                                current_song_index = -1;
                                app_state = STATE_BROWSER;
                                ui_dirty = 1;
                                break;
                        }
                        break;
                    }
                }

                retour = mad_decode(mp3Mad, mp3 + mp3Position, length,
                      (char *)soundBuffer + soundEnd, 10000, &read,
                      &done, resolution, halfsamplerate);

                soundEnd += done;

                if (done == 0) {
                    read++;
                    error++;
                    if (error > 65536) break;
                }

                mp3Position += read;
            }

            if (RETRO_IS_BIG_ENDIAN) {
                for (int i = 0; i < (int)(needFrame * 2); i++)
                    soundBuffer[i] = SWAP16(soundBuffer[i]);
            }

            audio_batch_cb(soundBuffer, needFrame);
            soundEnd -= needByte;
            memcpy((char *)soundBuffer, (char *)soundBuffer + needByte, soundEnd);
        }
    }

    video_cb(pixels, width, height, width * sizeof(uint16_t));
}

bool retro_load_game(const struct retro_game_info *info) {
    struct retro_input_descriptor desc[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left / Rewind"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right / Forward"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Select / Play/Pause"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Back"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Play / Stop"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Browser"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Rewind 5s"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Forward 5s"},
        {0},
    };
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

    log_cb(RETRO_LOG_INFO, "[froggymp3] retro_load_game()\n");

    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log_cb(RETRO_LOG_ERROR, "RGB565 not supported\n");
        return false;
    }

    // If a specific file was passed, try to load it directly
    if (info && info->path) {
        log_cb(RETRO_LOG_INFO, "[froggymp3] Loading file: %s\n", info->path);

        // Extract directory from path
        char *last_slash = strrchr(info->path, '/');
        if (last_slash) {
            int dir_len = last_slash - info->path;
            if (dir_len < MAX_PATH_LEN) {
                strncpy(current_path, info->path, dir_len);
                current_path[dir_len] = '\0';
            }

            // Scan the directory first so we can find the song index
            scan_directory();

            // Load the MP3
            if (load_mp3_file(info->path)) {
                strncpy(current_song, last_slash + 1, MAX_NAME_LEN - 1);
                current_song[MAX_NAME_LEN - 1] = '\0';
                current_song_index = find_song_index(current_song);
                init_scrolling_title();
                app_state = STATE_PLAYING;
            }
        }
    }

    // Make sure directory is scanned if we haven't already
    if (file_count == 0) {
        scan_directory();
    }

    return true;
}

void retro_unload_game(void) {
    CDGUnload();
    unload_mp3();
}

unsigned retro_get_region(void) { return RETRO_REGION_PAL; }
bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) { return false; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data_, size_t size) { return false; }
bool retro_unserialize(const void *data_, size_t size) { return false; }
void *retro_get_memory_data(unsigned id) { return NULL; }
size_t retro_get_memory_size(unsigned id) { return 0; }
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned index, bool enabled, const char *code) { }
