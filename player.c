#include "player.h"
#include "browser.h"
#include "ui.h"
#include "font.h"
#include "libmad/libmad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* External app state flag - defined in libretro.c */
extern int app_state;

#define STATE_BROWSER 0
#define STATE_PLAYER  1

/* Packed ID3 header */
#pragma pack(1)
typedef struct {
    uint8_t  id[3];
    uint16_t ver;
    uint8_t  flags;
    uint8_t  size[4];
} ID3Hdr;
#pragma pack()

/* Player state */
PlayMode player_mode      = MODE_NEXT;
char     player_song[128] = "";
int      player_song_idx  = -1;
int      player_paused    = 0;
int      player_loading   = 0;
int      player_warmup    = 0;   /* frames to wait before audio output */

/* Minimum bytes needed before playback starts (1MB or file size) */
#define MIN_BUFFER_START (1024 * 1024)

void    *player_mad       = NULL;
char    *player_data      = NULL;
uint32_t player_len       = 0;
uint32_t player_pos       = 0;
uint32_t player_start     = 0;
uint32_t player_rate      = 44100;
uint32_t player_bitrate   = 0;
uint32_t player_channels  = 2;
int16_t *player_pcm       = NULL;
uint16_t player_pcm_fill  = 0;

/* Background loading state */
static FILE    *player_file     = NULL;
static char     player_path[MAX_PATH];
static uint32_t player_loaded   = 0;   /* bytes loaded so far */
#define CHUNK_SIZE  (16 * 1024)        /* load 16KB per frame - smaller to avoid stutter */

/* Title scrolling */
static char  scroll_title[136];
static int   scroll_offset  = 0;
static int   scroll_delay   = 0;
static int   scroll_needed  = 0;
#define SCROLL_WAIT   30
#define SCROLL_SPEED  1
#define TITLE_MAX_W   280

void player_setup_title(void)
{
    int len, tw;

    strncpy(scroll_title, player_song, 127);
    scroll_title[127] = '\0';

    len = strlen(scroll_title);
    if (len > 4 && strcasecmp(scroll_title + len - 4, ".mp3") == 0)
        scroll_title[len - 4] = '\0';

    tw = font_measure_text(scroll_title);
    scroll_needed = (tw > TITLE_MAX_W) ? 1 : 0;
    scroll_offset = 0;
    scroll_delay  = SCROLL_WAIT;
}

/* Continue background loading - call each frame */
void player_bg_load(void)
{
    uint32_t to_read;

    if (!player_file || player_loaded >= player_len)
        return;

    to_read = player_len - player_loaded;
    if (to_read > CHUNK_SIZE)
        to_read = CHUNK_SIZE;

    fread(player_data + player_loaded, 1, to_read, player_file);
    player_loaded += to_read;

    /* done loading */
    if (player_loaded >= player_len) {
        fclose(player_file);
        player_file = NULL;
    }
}

/* Get pointer to data - just return buffer offset */
char *player_get_data(uint32_t pos, uint32_t need)
{
    /* wait if data not loaded yet */
    if (pos + need > player_loaded)
        return NULL;

    return player_data + pos;
}

int player_load(const char *path)
{
    ID3Hdr hdr;
    char *tmp;
    int rd, done, len;
    uint32_t initial_load;

    /* close previous file */
    if (player_file) {
        fclose(player_file);
        player_file = NULL;
    }

    /* free old buffer */
    if (player_data) {
        free(player_data);
        player_data = NULL;
    }

    player_file = fopen(path, "rb");
    if (!player_file) return 0;

    strncpy(player_path, path, MAX_PATH - 1);
    player_path[MAX_PATH - 1] = '\0';

    fseek(player_file, 0, SEEK_END);
    player_len = ftell(player_file);
    fseek(player_file, 0, SEEK_SET);

    /* allocate full buffer for the file */
    player_data = malloc(player_len);
    if (!player_data) {
        fclose(player_file);
        player_file = NULL;
        return 0;
    }

    /* load first 256KB, rest via background loading */
    initial_load = (player_len > 262144) ? 262144 : player_len;
    player_loaded = fread(player_data, 1, initial_load, player_file);

    player_pos = 0;

    /* skip ID3 tag */
    if (player_len > 10 && player_loaded >= 10) {
        memcpy(&hdr, player_data, 10);
        if (hdr.id[0] == 'I' && hdr.id[1] == 'D' && hdr.id[2] == '3') {
            player_pos = (hdr.size[0] & 0x7f);
            player_pos = (player_pos << 7) | (hdr.size[1] & 0x7f);
            player_pos = (player_pos << 7) | (hdr.size[2] & 0x7f);
            player_pos = (player_pos << 7) | (hdr.size[3] & 0x7f);
            player_pos += 10;
        }
    }
    player_start = player_pos;

    if (player_mad)
        mad_uninit(player_mad);
    player_mad = mad_init();
    if (!player_mad) return 0;

    player_pcm_fill = 0;

    /* probe first frame for metadata */
    len = (player_loaded - player_pos > 4096) ? 4096 : (player_loaded - player_pos);
    tmp = malloc(32768);
    if (tmp && len > 0) {
        if (mad_decode(player_mad, player_data + player_pos, len,
                       tmp, 32768, &rd, &done, 16, 0) == MAD_OK || done > 0) {
            player_rate     = mad_get_samplerate(player_mad);
            player_bitrate  = mad_get_bitrate(player_mad);
            player_channels = mad_get_channels(player_mad);
        }
        free(tmp);
        mad_uninit(player_mad);
        player_mad = mad_init();
    }

    player_paused = 0;
    player_loading = 1;  /* show LOADING screen */
    player_warmup = 10;  /* wait 10 frames for PCM buffer to fill */

    return 1;
}

void player_unload(void)
{
    if (player_mad) {
        mad_uninit(player_mad);
        player_mad = NULL;
    }
    if (player_file) {
        fclose(player_file);
        player_file = NULL;
    }
    if (player_data) {
        free(player_data);
        player_data = NULL;
    }
    player_len = 0;
    player_pos = 0;
    player_loaded = 0;
    player_pcm_fill = 0;
}

void player_seek(int secs)
{
    int bps, off, npos;

    if (!player_data || !player_bitrate) return;

    bps = player_bitrate / 8;
    off = secs * bps;
    npos = (int)player_pos + off;

    if (npos < (int)player_start)
        npos = player_start;
    if (npos >= (int)player_len) {
        npos = player_len - 1024;
        if (npos < (int)player_start)
            npos = player_start;
    }

    player_pos = npos;
    player_pcm_fill = 0;

    if (player_mad) {
        mad_uninit(player_mad);
        player_mad = mad_init();
    }
}

void player_toggle_pause(void)
{
    player_paused = !player_paused;
}

void player_play_at(int idx)
{
    char path[MAX_PATH];

    if (idx < 0 || idx >= browser_count || browser_files[idx].is_dir)
        return;

    snprintf(path, MAX_PATH, "%s/%s", browser_path, browser_files[idx].name);

    if (player_load(path)) {
        strncpy(player_song, browser_files[idx].name, 127);
        player_song[127] = '\0';
        player_song_idx = idx;
        player_setup_title();
        app_state = STATE_PLAYER;
    }
}

void player_next(void)
{
    int n = browser_next_song(player_song_idx);
    if (n >= 0) {
        player_play_at(n);
    } else {
        player_unload();
        player_song[0] = '\0';
        player_song_idx = -1;
        app_state = STATE_BROWSER;
    }
}

void player_prev(void)
{
    int p = browser_prev_song(player_song_idx);
    if (p >= 0)
        player_play_at(p);
}

void player_random(void)
{
    int r = browser_rand_song(player_song_idx);
    if (r >= 0) {
        player_play_at(r);
    } else {
        player_unload();
        player_song[0] = '\0';
        player_song_idx = -1;
        app_state = STATE_BROWSER;
    }
}

void player_cycle_mode(int dir)
{
    if (dir > 0)
        player_mode = (player_mode + 1) % MODE_COUNT;
    else {
        if (player_mode == 0)
            player_mode = MODE_COUNT - 1;
        else
            player_mode--;
    }
}

void player_on_end(void)
{
    switch (player_mode) {
    case MODE_REPEAT:
        player_pos = player_start;
        player_pcm_fill = 0;
        if (player_mad) {
            mad_uninit(player_mad);
            player_mad = mad_init();
        }
        break;
    case MODE_NEXT:
        player_next();
        break;
    case MODE_RANDOM:
        player_random();
        break;
    case MODE_NORMAL:
    default:
        player_unload();
        player_song[0] = '\0';
        player_song_idx = -1;
        app_state = STATE_BROWSER;
        break;
    }
}

void player_tick_scroll(int *dirty)
{
    int tw, max_off;

    if (!scroll_needed) return;

    if (scroll_delay > 0) {
        scroll_delay--;
        if (scroll_delay == 0 && scroll_needed == 2) {
            scroll_offset = 0;
            scroll_delay  = SCROLL_WAIT;
            scroll_needed = 1;
        }
        return;
    }

    tw = font_measure_text(scroll_title);
    max_off = tw - TITLE_MAX_W;

    scroll_offset += SCROLL_SPEED;
    if (scroll_offset >= max_off) {
        scroll_offset = max_off;
        scroll_delay  = SCROLL_WAIT;
        scroll_needed = 2;
    }
    (void)dirty; /* unused now - always redraw */
}

void player_draw(void)
{
    int title_y, tw, tx;
    int bar_x, bar_y, bar_w, bar_h, prog_w;
    int ctrl_y;
    const char *status, *mode_str;
    int sw, sx;
    int mode_w, mode_x, mode_y;

    ui_clear(COL_BG);

    /* Show loading screen while buffering */
    if (player_loading) {
        const char *msg = "LOADING...";
        int mw = font_measure_text(msg);
        ui_fill(0, 0, SCREEN_W, 28, COL_HEADER_BG);
        font_draw_text(pixels, SCREEN_W, SCREEN_H, 12, 6, "NOW PLAYING", COL_TEXT);
        font_draw_text(pixels, SCREEN_W, SCREEN_H,
                       (SCREEN_W - mw) / 2, SCREEN_H / 2 - 8, msg, COL_DIM);
        return;
    }

    /* header bar */
    ui_fill(0, 0, SCREEN_W, 28, COL_HEADER_BG);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, 12, 6, "NOW PLAYING", COL_TEXT);

    /* title (scrolling if needed) */
    title_y = 55;
    tw = font_measure_text(scroll_title);

    if (tw <= TITLE_MAX_W) {
        tx = (SCREEN_W - tw) / 2;
        font_draw_text(pixels, SCREEN_W, SCREEN_H, tx, title_y, scroll_title, COL_TEXT);
    } else {
        tx = (SCREEN_W - TITLE_MAX_W) / 2;
        font_draw_text(pixels, SCREEN_W, SCREEN_H, tx - scroll_offset, title_y,
                       scroll_title, COL_TEXT);
    }

    /* progress bar */
    bar_y = 95;
    bar_h = 6;
    bar_w = SCREEN_W - 40;
    bar_x = 20;

    ui_pill(bar_x, bar_y, bar_w, bar_h, 3, COL_BAR_BG);

    if (player_len > player_start) {
        prog_w = ((player_pos - player_start) * bar_w) / (player_len - player_start);
        if (prog_w > 6)
            ui_pill(bar_x, bar_y, prog_w, bar_h, 3, COL_BAR_FG);
        else if (prog_w > 0)
            ui_fill(bar_x, bar_y, prog_w, bar_h, COL_BAR_FG);
    }

    /* controls row: seek left, status, seek right */
    ctrl_y = 125;

    /* seek left */
    sw = font_measure_text("< 5S");
    sx = 24;
    ui_pill(sx - 4, ctrl_y - 4, sw + 8, 24, 12, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, sx, ctrl_y, "< 5S", COL_TEXT);

    /* status */
    status = player_paused ? "PAUSED" : "PLAYING";
    sw = font_measure_text(status);
    sx = (SCREEN_W - sw) / 2;
    ui_pill(sx - 12, ctrl_y - 5, sw + 24, 26, 13, player_paused ? COL_PILL : COL_SELECT_BG);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, sx, ctrl_y, status,
                   player_paused ? COL_DIM : COL_SELECT_FG);

    /* seek right */
    sw = font_measure_text("5S >");
    sx = SCREEN_W - sw - 24;
    ui_pill(sx - 4, ctrl_y - 4, sw + 8, 24, 12, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, sx, ctrl_y, "5S >", COL_TEXT);

    /* playback mode */
    mode_y = 165;
    switch (player_mode) {
    case MODE_NORMAL: mode_str = "STOP AT END"; break;
    case MODE_NEXT:   mode_str = "PLAY NEXT";   break;
    case MODE_REPEAT: mode_str = "REPEAT";      break;
    case MODE_RANDOM: mode_str = "SHUFFLE";     break;
    default:          mode_str = "PLAY NEXT";   break;
    }
    mode_w = font_measure_text(mode_str);
    mode_x = (SCREEN_W - mode_w) / 2;
    ui_pill(mode_x - 8, mode_y - 4, mode_w + 16, 24, 12, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, mode_x, mode_y, mode_str, COL_TEXT);

    /* footer bar - plain text */
    ui_fill(0, SCREEN_H - 32, SCREEN_W, 32, COL_HEADER_BG);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, 12, SCREEN_H - 24,
                   "Y/X PREV/NEXT", COL_DIM);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, SCREEN_W - 50, SCREEN_H - 24,
                   "B BACK", COL_DIM);
}
