#include "libretro.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <retro_endianness.h>
#include <streams/file_stream.h>

#include "platform.h"
#include "font.h"
#include "ui.h"
#include "browser.h"
#include "player.h"
#include "libmad/libmad.h"

#define FPS 50

/* App states */
#define STATE_BROWSER 0
#define STATE_PLAYER  1
int app_state = STATE_BROWSER;
int ui_dirty = 1;

/* Framebuffer */
uint16_t pixels[SCREEN_W * SCREEN_H];

/* Callbacks */
static void fallback_log(enum retro_log_level level, const char *fmt, ...);
retro_log_printf_t       log_cb = fallback_log;
retro_video_refresh_t    video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
retro_environment_t      environ_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;

int width, height;

/* Input tracking */
static int btn_held[16];
static int btn_down[16];

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static void update_variables(void) { }

void retro_init(void)
{
    update_variables();

    player_pcm = (int16_t *)malloc(32768);
    width  = SCREEN_W;
    height = SCREEN_H;

    font_init();
    browser_scan();
}

void retro_deinit(void)
{
    player_unload();
    if (player_pcm) {
        free(player_pcm);
        player_pcm = NULL;
    }
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port;
    (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
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

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    info->timing.fps            = FPS;
    info->timing.sample_rate    = (double)player_rate;
    info->geometry.base_width   = SCREEN_W;
    info->geometry.base_height  = SCREEN_H;
    info->geometry.max_width    = SCREEN_W;
    info->geometry.max_height   = SCREEN_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
}

void retro_set_environment(retro_environment_t cb)
{
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

void retro_set_audio_sample(retro_audio_sample_t cb)       { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)           { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)         { input_state_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb)     { video_cb = cb; }
void retro_reset(void) { }

#define SAMPLES_PER_FRAME(rate) ((rate) / FPS)
#define BYTES_PER_FRAME(rate)   (SAMPLES_PER_FRAME(rate) * 4)

void retro_run(void)
{
    static int buttons[] = {
        RETRO_DEVICE_ID_JOYPAD_A,
        RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_UP,
        RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_LEFT,
        RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_START,
        RETRO_DEVICE_ID_JOYPAD_SELECT,
        RETRO_DEVICE_ID_JOYPAD_L,
        RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_Y,
        RETRO_DEVICE_ID_JOYPAD_X
    };
    static bool updated = false;
    static int prev_state = -1;
    uint32_t need_samples, need_bytes;
    int i, cur;

    need_samples = SAMPLES_PER_FRAME(player_rate);
    need_bytes   = BYTES_PER_FRAME(player_rate);

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        update_variables();

    input_poll_cb();

    for (i = 0; i < 12; i++) {
        cur = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, buttons[i]);
        btn_down[i] = cur && !btn_held[i];
        btn_held[i] = cur;
    }

    /* force redraw on state change */
    if (app_state != prev_state) {
        ui_dirty = 1;
        prev_state = app_state;
    }

    if (app_state == STATE_BROWSER) {
        /* navigation */
        if (btn_down[2] && browser_sel > 0) {
            browser_sel--;
            if (browser_sel < browser_scroll)
                browser_scroll = browser_sel;
            ui_dirty = 1;
        }
        if (btn_down[3] && browser_sel < browser_count - 1) {
            browser_sel++;
            if (browser_sel >= browser_scroll + VISIBLE_ROWS)
                browser_scroll = browser_sel - VISIBLE_ROWS + 1;
            ui_dirty = 1;
        }

        /* select */
        if ((btn_down[0] || btn_down[6]) && browser_count > 0) {
            if (browser_files[browser_sel].is_dir) {
                browser_enter(browser_files[browser_sel].name);
                ui_dirty = 1;
            } else {
                player_play_at(browser_sel);
            }
        }

        /* back */
        if (btn_down[1]) {
            browser_back();
            ui_dirty = 1;
        }

        if (ui_dirty) {
            browser_draw();
            ui_dirty = 0;
        }
    }
    else if (app_state == STATE_PLAYER) {
        /* play/pause */
        if (btn_down[0]) {
            player_toggle_pause();
            ui_dirty = 1;
        }

        /* seek */
        if (btn_down[4] || btn_down[8]) {
            player_seek(-5);
            ui_dirty = 1;
        }
        if (btn_down[5] || btn_down[9]) {
            player_seek(5);
            ui_dirty = 1;
        }

        /* mode cycling */
        if (btn_down[2]) {
            player_cycle_mode(-1);
            ui_dirty = 1;
        }
        if (btn_down[3]) {
            player_cycle_mode(1);
            ui_dirty = 1;
        }

        /* prev/next song */
        if (btn_down[10]) {
            if (player_mode == MODE_RANDOM)
                player_random();
            else
                player_prev();
        }
        if (btn_down[11]) {
            if (player_mode == MODE_RANDOM)
                player_random();
            else
                player_next();
        }

        /* back to browser */
        if (btn_down[1] || btn_down[6]) {
            app_state = STATE_BROWSER;
            ui_dirty = 1;
        }

        player_tick_scroll(&ui_dirty);

        /* always redraw player - progress bar updates continuously */
        player_draw();
        ui_dirty = 0;
    }

    /* continue background loading (runs in any state) */
    player_bg_load();

    /* audio decode (runs in any state if playing) */
    if (!player_paused && player_mad && player_len > 0) {
        int err = 0;

        while (player_pcm_fill <= need_bytes) {
            int len = 2048;
            int rd, done;
            char *data;

            if (player_pos + len > player_len) {
                len = player_len - player_pos;
                if (len <= 128) {
                    player_on_end();
                    break;
                }
            }

            data = player_get_data(player_pos, len);
            if (!data) {
                /* data not loaded yet, wait */
                break;
            }

            mad_decode(player_mad, data, len,
                       (char *)player_pcm + player_pcm_fill, 10000,
                       &rd, &done, 16, 0);

            player_pcm_fill += done;

            if (done == 0) {
                rd++;
                err++;
                if (err > 65536) break;
            }

            player_pos += rd;
        }

        if (RETRO_IS_BIG_ENDIAN) {
            for (i = 0; i < (int)(need_samples * 2); i++)
                player_pcm[i] = SWAP16(player_pcm[i]);
        }

        audio_batch_cb(player_pcm, need_samples);
        player_pcm_fill -= need_bytes;
        memmove(player_pcm, (char *)player_pcm + need_bytes, player_pcm_fill);
    }

    video_cb(pixels, width, height, width * sizeof(uint16_t));
}

bool retro_load_game(const struct retro_game_info *info)
{
    struct retro_input_descriptor desc[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Rewind"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Mode Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Mode Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Forward"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Select/Play"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Back"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Back"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Prev Song"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Next Song"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Seek -5s"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Seek +5s"},
        {0},
    };
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
        return false;

    if (info && info->path) {
        char *slash = strrchr(info->path, '/');
        if (slash) {
            int dir_len = slash - info->path;
            if (dir_len < MAX_PATH) {
                strncpy(browser_path, info->path, dir_len);
                browser_path[dir_len] = '\0';
            }
            browser_scan();

            if (player_load(info->path)) {
                strncpy(player_song, slash + 1, 127);
                player_song[127] = '\0';
                player_song_idx = browser_find_song(player_song);
                player_setup_title();
                app_state = STATE_PLAYER;
                ui_dirty = 1;
            }
        }
    }

    if (browser_count == 0)
        browser_scan();

    return true;
}

void retro_unload_game(void)
{
    CDGUnload();
    player_unload();
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
