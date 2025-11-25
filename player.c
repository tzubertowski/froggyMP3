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
extern int ui_dirty;

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

/* Streaming state */
static FILE    *player_file   = NULL;
static char     player_path[MAX_PATH];
#define STREAM_BUF_SIZE  (256 * 1024)  /* 256KB buffer */
static uint32_t buf_file_pos  = 0;     /* file offset of buffer start */
static uint32_t buf_fill      = 0;     /* bytes in buffer */

/* Title scrolling */
static char  scroll_title[136];
static int   scroll_offset  = 0;
static int   scroll_delay   = 0;
static int   scroll_needed  = 0;
#define SCROLL_WAIT   30
#define SCROLL_SPEED  1
#define TITLE_MAX_W   280

static void setup_title(void)
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

/* Fill stream buffer from current file position */
static void fill_buffer(uint32_t file_pos)
{
    if (!player_file) return;

    fseek(player_file, file_pos, SEEK_SET);
    buf_fill = fread(player_data, 1, STREAM_BUF_SIZE, player_file);
    buf_file_pos = file_pos;
}

/* Get pointer to data at file position, refill buffer if needed */
char *player_get_data(uint32_t pos, uint32_t need)
{
    uint32_t buf_offset;

    /* check if requested range is in buffer */
    if (pos >= buf_file_pos && pos + need <= buf_file_pos + buf_fill) {
        buf_offset = pos - buf_file_pos;

        /* proactive refill: if we're past halfway, shift and top up */
        if (buf_offset > STREAM_BUF_SIZE / 2 && player_file) {
            uint32_t keep = buf_fill - buf_offset;
            memmove(player_data, player_data + buf_offset, keep);
            buf_file_pos = pos;
            fseek(player_file, pos + keep, SEEK_SET);
            buf_fill = keep + fread(player_data + keep, 1, STREAM_BUF_SIZE - keep, player_file);
        }

        return player_data + (pos - buf_file_pos);
    }

    /* data not in buffer - full refill */
    fill_buffer(pos);

    if (buf_fill == 0) return NULL;
    return player_data;
}

int player_load(const char *path)
{
    ID3Hdr hdr;
    char *tmp, *data;
    int rd, done, len;

    /* close previous file */
    if (player_file) {
        fclose(player_file);
        player_file = NULL;
    }

    player_file = fopen(path, "rb");
    if (!player_file) return 0;

    strncpy(player_path, path, MAX_PATH - 1);
    player_path[MAX_PATH - 1] = '\0';

    fseek(player_file, 0, SEEK_END);
    player_len = ftell(player_file);
    fseek(player_file, 0, SEEK_SET);

    /* allocate stream buffer */
    if (!player_data) {
        player_data = malloc(STREAM_BUF_SIZE);
        if (!player_data) {
            fclose(player_file);
            player_file = NULL;
            return 0;
        }
    }

    /* read first chunk */
    buf_file_pos = 0;
    buf_fill = fread(player_data, 1, STREAM_BUF_SIZE, player_file);

    player_pos = 0;

    /* skip ID3 tag */
    if (player_len > 10 && buf_fill >= 10) {
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
    len = (player_len - player_pos > 4096) ? 4096 : (player_len - player_pos);
    data = player_get_data(player_pos, len);
    tmp = malloc(32768);
    if (tmp && data) {
        if (mad_decode(player_mad, data, len,
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
    /* keep buffer allocated for reuse */
    player_len = 0;
    player_pos = 0;
    player_pcm_fill = 0;
    buf_fill = 0;
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
        setup_title();
        app_state = STATE_PLAYER;
        ui_dirty = 1;
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
        ui_dirty = 1;
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
        ui_dirty = 1;
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
        ui_dirty = 1;
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
        ui_dirty = 1;
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
            *dirty = 1;
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
    *dirty = 1;
}

void player_draw(void)
{
    int title_y, tw, tx;
    int bar_x, bar_y, bar_w, bar_h, prog_w;
    int ctrl_y;
    const char *seek_l, *seek_r, *status, *mode_str;
    int sw, sx;
    int mode_w, mode_x, mode_y;
    int leg_y;

    ui_clear(COL_BG);

    /* header */
    font_draw_text(pixels, SCREEN_W, SCREEN_H, 10, 8, "NOW PLAYING", COL_DIM);

    /* title (scrolling if needed) */
    title_y = 70;
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
    bar_y = 120;
    bar_h = 8;
    bar_w = SCREEN_W - 40;
    bar_x = 20;

    ui_pill(bar_x, bar_y, bar_w, bar_h, 4, COL_BAR_BG);

    if (player_len > player_start) {
        prog_w = ((player_pos - player_start) * bar_w) / (player_len - player_start);
        if (prog_w > 8)
            ui_pill(bar_x, bar_y, prog_w, bar_h, 4, COL_BAR_FG);
        else if (prog_w > 0)
            ui_fill(bar_x, bar_y, prog_w, bar_h, COL_BAR_FG);
    }

    /* controls row */
    ctrl_y = 153;

    seek_l = "< 5S";
    sw = font_measure_text(seek_l);
    sx = 30;
    ui_pill(sx - 4, ctrl_y - 3, sw + 8, 22, 10, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, sx, ctrl_y, seek_l, COL_TEXT);

    status = player_paused ? " PAUSED " : " PLAYING ";
    sw = font_measure_text(status);
    sx = (SCREEN_W - sw) / 2;
    ui_pill(sx - 4, ctrl_y - 3, sw + 8, 22, 10, player_paused ? COL_PILL : COL_SELECT_BG);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, sx, ctrl_y, status,
                   player_paused ? COL_DIM : COL_SELECT_FG);

    seek_r = "5S >";
    sw = font_measure_text(seek_r);
    sx = SCREEN_W - sw - 30;
    ui_pill(sx - 4, ctrl_y - 3, sw + 8, 22, 10, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, sx, ctrl_y, seek_r, COL_TEXT);

    /* playback mode */
    mode_y = 185;
    switch (player_mode) {
    case MODE_NORMAL: mode_str = "STOP AT END"; break;
    case MODE_NEXT:   mode_str = "PLAY NEXT";   break;
    case MODE_REPEAT: mode_str = "REPEAT";      break;
    case MODE_RANDOM: mode_str = "RANDOM";      break;
    default:          mode_str = "PLAY NEXT";   break;
    }
    mode_w = font_measure_text(mode_str);
    mode_x = (SCREEN_W - mode_w) / 2;
    ui_pill(mode_x - 6, mode_y - 3, mode_w + 12, 22, 10, COL_PILL);
    font_draw_text(pixels, SCREEN_W, SCREEN_H, mode_x, mode_y, mode_str, COL_TEXT);

    /* legend */
    leg_y = SCREEN_H - 24;
    ui_legend_btn(10, leg_y, " Y/X - PREV/NEXT ");
    ui_legend_btn(SCREEN_W - 40, leg_y, " B ");
}
