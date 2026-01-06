#include "player.h"
#include "browser.h"
#include "ui.h"
#include "font.h"
#include "libmad/libmad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* External app state - defined in libretro.c */
extern int app_state;
#define STATE_BROWSER 0
#define STATE_PLAYER  1

/* ID3 header for tag skipping */
#pragma pack(1)
typedef struct {
    uint8_t  id[3];
    uint16_t ver;
    uint8_t  flags;
    uint8_t  size[4];
} ID3Hdr;
#pragma pack()

/* Player state */
PlayMode player_mode     = MODE_NEXT;
char     player_song[128];
int      player_song_idx = -1;
int      player_paused   = 0;
int      player_ready    = 0;  /* 0=loading, 1=ready to play */
static int load_frames   = 0;  /* frames since load started */

/* Decoder state */
void    *player_mad      = NULL;
char    *player_data     = NULL;
uint32_t player_len      = 0;
uint32_t player_pos      = 0;
uint32_t player_start    = 0;
uint32_t player_rate     = 44100;
uint32_t player_bitrate  = 0;
uint32_t player_channels = 2;
int16_t *player_pcm      = NULL;
uint32_t player_pcm_fill = 0;

/* File loading state */
static FILE    *player_file   = NULL;
static uint32_t player_loaded = 0;
#define INITIAL_LOAD  (128 * 1024)  /* 128KB loaded before playback */
#define CHUNK_SIZE    (16 * 1024)   /* 16KB per frame background load */

/* Title scrolling state */
static char scroll_title[136];
static int  scroll_offset;
static int  scroll_delay;
static int  scroll_needed;
#define SCROLL_WAIT  30
#define SCROLL_SPEED 1
#define TITLE_MAX_W  280


void player_setup_title(void)
{
    int len = strlen(player_song);

    strncpy(scroll_title, player_song, sizeof(scroll_title) - 1);
    scroll_title[sizeof(scroll_title) - 1] = '\0';

    /* strip .mp3 extension */
    len = strlen(scroll_title);
    if (len > 4 && strcasecmp(scroll_title + len - 4, ".mp3") == 0)
        scroll_title[len - 4] = '\0';

    scroll_needed = (font_measure_text(scroll_title) > TITLE_MAX_W);
    scroll_offset = 0;
    scroll_delay = SCROLL_WAIT;
}

void player_tick_scroll(void)
{
    int max_off;

    if (!scroll_needed) return;

    if (scroll_delay > 0) {
        scroll_delay--;
        if (scroll_delay == 0 && scroll_needed == 2) {
            scroll_offset = 0;
            scroll_delay = SCROLL_WAIT;
            scroll_needed = 1;
        }
        return;
    }

    max_off = font_measure_text(scroll_title) - TITLE_MAX_W;
    scroll_offset += SCROLL_SPEED;

    if (scroll_offset >= max_off) {
        scroll_offset = max_off;
        scroll_delay = SCROLL_WAIT;
        scroll_needed = 2;
    }
}

static uint32_t parse_id3_size(const uint8_t *buf)
{
    return ((buf[0] & 0x7f) << 21) |
           ((buf[1] & 0x7f) << 14) |
           ((buf[2] & 0x7f) << 7)  |
            (buf[3] & 0x7f);
}

static void reset_decoder(void)
{
    if (player_mad) {
        mad_uninit(player_mad);
        player_mad = mad_init();
    }
    player_pcm_fill = 0;
}

int player_load(const char *path)
{
    ID3Hdr hdr;
    char *tmp;
    int rd, done, len;
    uint32_t initial;

    player_unload();

    player_file = fopen(path, "rb");
    if (!player_file) return 0;

    /* get file size */
    fseek(player_file, 0, SEEK_END);
    player_len = ftell(player_file);
    fseek(player_file, 0, SEEK_SET);

    /* allocate full buffer */
    player_data = malloc(player_len);
    if (!player_data) {
        fclose(player_file);
        player_file = NULL;
        return 0;
    }

    /* load initial chunk */
    initial = (player_len > INITIAL_LOAD) ? INITIAL_LOAD : player_len;
    player_loaded = fread(player_data, 1, initial, player_file);

    /* close if fully loaded */
    if (player_loaded >= player_len) {
        fclose(player_file);
        player_file = NULL;
    }

    /* skip ID3v2 tag if present */
    player_pos = 0;
    if (player_loaded >= 10) {
        memcpy(&hdr, player_data, 10);
        if (hdr.id[0] == 'I' && hdr.id[1] == 'D' && hdr.id[2] == '3')
            player_pos = parse_id3_size(hdr.size) + 10;
    }
    player_start = player_pos;

    /* init decoder */
    player_mad = mad_init();
    if (!player_mad) {
        free(player_data);
        player_data = NULL;
        return 0;
    }

    /* probe first frame for sample rate/bitrate */
    len = (player_loaded - player_pos > 4096) ? 4096 : (player_loaded - player_pos);
    tmp = malloc(32768);
    if (tmp && len > 0) {
        if (mad_decode(player_mad, player_data + player_pos, len,
                       tmp, 32768, &rd, &done, 16, 0) == MAD_OK || done > 0) {
            player_rate = mad_get_samplerate(player_mad);
            player_bitrate = mad_get_bitrate(player_mad);
            player_channels = mad_get_channels(player_mad);
        }
        free(tmp);
        reset_decoder();
    }

    player_paused = 0;
    player_ready = 0;
    player_pcm_fill = 0;
    load_frames = 0;

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
    player_ready = 0;
}

/* Background load - call each frame */
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

    if (player_loaded >= player_len) {
        fclose(player_file);
        player_file = NULL;
    }
}

void player_seek(int secs)
{
    int offset, newpos;

    if (!player_data || !player_bitrate) return;

    offset = secs * (player_bitrate / 8);
    newpos = (int)player_pos + offset;

    if (newpos < (int)player_start)
        newpos = player_start;
    if (newpos >= (int)player_len)
        newpos = player_len - 1024;
    if (newpos < (int)player_start)
        newpos = player_start;

    player_pos = newpos;
    reset_decoder();
}

void player_toggle_pause(void)
{
    player_paused = !player_paused;
}

/* Helper: go back to browser */
static void go_to_browser(void)
{
    player_unload();
    player_song[0] = '\0';
    player_song_idx = -1;
    app_state = STATE_BROWSER;
}

void player_play_at(int idx)
{
    char path[MAX_PATH];

    if (idx < 0 || idx >= browser_count || browser_files[idx].is_dir)
        return;

    snprintf(path, sizeof(path), "%s/%s", browser_path, browser_files[idx].name);

    if (player_load(path)) {
        strncpy(player_song, browser_files[idx].name, sizeof(player_song) - 1);
        player_song[sizeof(player_song) - 1] = '\0';
        player_song_idx = idx;
        player_setup_title();
        app_state = STATE_PLAYER;
    }
}

void player_next(void)
{
    int n = browser_next_song(player_song_idx);
    if (n >= 0)
        player_play_at(n);
    else
        go_to_browser();
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
    if (r >= 0)
        player_play_at(r);
    else
        go_to_browser();
}

void player_cycle_mode(int dir)
{
    player_mode = (player_mode + (dir > 0 ? 1 : MODE_COUNT - 1)) % MODE_COUNT;
}

void player_on_end(void)
{
    switch (player_mode) {
    case MODE_REPEAT:
        player_pos = player_start;
        reset_decoder();
        break;
    case MODE_NEXT:
        player_next();
        break;
    case MODE_RANDOM:
        player_random();
        break;
    default:
        go_to_browser();
        break;
    }
}

/*
 * Simple PCM buffering:
 * - 64KB buffer (~0.36 seconds at 44100Hz stereo)
 * - Wait 4 seconds (200 frames) for MP3 data to preload
 * - Small buffer = fast memmove = smooth playback
 */
#define PCM_BUFFER_SIZE   (64 * 1024)
#define PCM_FILL_TARGET   (56 * 1024)   /* fill target */
#define MIN_LOAD_FRAMES   100           /* 2 seconds at 50fps */

/* Called each frame from libretro.c to decode audio */
void player_decode(uint32_t need_bytes)
{
    int err = 0;
    int loops = 0;

    if (player_paused || !player_mad || player_len == 0)
        return;

    /* count loading frames */
    if (!player_ready)
        load_frames++;

    /* fill PCM buffer - limit iterations per frame */
    while (player_pcm_fill < PCM_FILL_TARGET && loops < 200) {
        int len = 2048;
        int rd = 0, done = 0;

        loops++;

        if (player_pos + len > player_len) {
            len = player_len - player_pos;
            if (len <= 128) {
                player_on_end();
                return;
            }
        }

        /* wait if MP3 data not loaded yet */
        if (player_pos + len > player_loaded)
            break;

        mad_decode(player_mad, player_data + player_pos, len,
                   (char *)player_pcm + player_pcm_fill,
                   PCM_BUFFER_SIZE - player_pcm_fill,
                   &rd, &done, 16, 0);

        if (done > 0)
            player_pcm_fill += done;
        if (rd > 0)
            player_pos += rd;

        if (done == 0 && rd == 0) {
            player_pos++;
            if (++err > 4096) break;
        }
    }

    /* Ready after minimum load time AND buffer is full */
    if (!player_ready && load_frames >= MIN_LOAD_FRAMES && player_pcm_fill >= PCM_FILL_TARGET) {
        player_ready = 1;
    }
}

void player_draw(void)
{
    int tw, tx, sw, sx;
    const char *status, *mode_str;
    static const char *mode_names[] = {
        "STOP AT END", "PLAY NEXT", "REPEAT", "SHUFFLE"
    };

    ui_clear(COL_BG);

    /* header */
    ui_fill(0, 0, SCREEN_W, 28, COL_HEADER_BG);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, 12, 6, "NOW PLAYING", COL_TEXT);

    /* loading screen */
    if (!player_ready) {
        tw = font_measure_text("LOADING...");
        font_draw_text(pixels, SCREEN_W, SCREEN_H,
                       (SCREEN_W - tw) / 2, SCREEN_H / 2 - 8, "LOADING...", COL_DIM);
        return;
    }

    /* title */
    tw = font_measure_text(scroll_title);
    if (tw <= TITLE_MAX_W) {
        tx = (SCREEN_W - tw) / 2;
    } else {
        tx = (SCREEN_W - TITLE_MAX_W) / 2 - scroll_offset;
    }
    font_draw_text(pixels, SCREEN_W, SCREEN_H, tx, 55, scroll_title, COL_TEXT);

    /* progress bar */
    ui_pill(20, 95, SCREEN_W - 40, 6, 3, COL_BAR_BG);
    if (player_len > player_start) {
        int prog = ((player_pos - player_start) * (SCREEN_W - 40)) / (player_len - player_start);
        if (prog > 6)
            ui_pill(20, 95, prog, 6, 3, COL_BAR_FG);
        else if (prog > 0)
            ui_fill(20, 95, prog, 6, COL_BAR_FG);
    }

    /* seek left */
    sw = font_measure_text("< 5S");
    ui_pill(20, 121, sw + 8, 24, 12, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, 24, 125, "< 5S", COL_TEXT);

    /* play/pause status */
    status = player_paused ? "PAUSED" : "PLAYING";
    sw = font_measure_text(status);
    sx = (SCREEN_W - sw) / 2;
    ui_pill(sx - 12, 120, sw + 24, 26, 13, player_paused ? COL_PILL : COL_SELECT_BG);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, sx, 125, status,
                   player_paused ? COL_DIM : COL_SELECT_FG);

    /* seek right */
    sw = font_measure_text("5S >");
    ui_pill(SCREEN_W - sw - 28, 121, sw + 8, 24, 12, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, SCREEN_W - sw - 24, 125, "5S >", COL_TEXT);

    /* mode */
    mode_str = mode_names[player_mode];
    sw = font_measure_text(mode_str);
    sx = (SCREEN_W - sw) / 2;
    ui_pill(sx - 8, 161, sw + 16, 24, 12, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, sx, 165, mode_str, COL_TEXT);

    /* footer */
    ui_fill(0, SCREEN_H - 32, SCREEN_W, 32, COL_HEADER_BG);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, 12, SCREEN_H - 24, "Y/X PREV/NEXT", COL_DIM);
    tw = font_measure_text("B BACK");
    font_draw_text(pixels, SCREEN_W, SCREEN_H, SCREEN_W - tw - 12, SCREEN_H - 24, "B BACK", COL_DIM);
}
