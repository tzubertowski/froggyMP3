#ifndef FROGGY_PLAYER_H
#define FROGGY_PLAYER_H

#include <stdint.h>

typedef enum {
    MODE_NORMAL,  /* stop at end */
    MODE_NEXT,    /* auto-play next */
    MODE_REPEAT,  /* loop current */
    MODE_RANDOM,  /* shuffle */
    MODE_COUNT
} PlayMode;

/* Player state */
extern PlayMode player_mode;
extern char     player_song[128];
extern int      player_song_idx;
extern int      player_paused;
extern int      player_ready;

/* Decoder state */
extern void    *player_mad;
extern char    *player_data;
extern uint32_t player_len;
extern uint32_t player_pos;
extern uint32_t player_start;
extern uint32_t player_rate;
extern uint32_t player_bitrate;
extern uint32_t player_channels;
extern int16_t *player_pcm;
extern uint32_t player_pcm_fill;

/* Core operations */
int   player_load(const char *path);
void  player_unload(void);
void  player_seek(int secs);
void  player_toggle_pause(void);

/* Playback control */
void player_play_at(int idx);
void player_next(void);
void player_prev(void);
void player_random(void);
void player_cycle_mode(int dir);
void player_on_end(void);

/* Audio decode - call each frame */
void player_decode(uint32_t need_bytes);
void player_bg_load(void);

/* Rendering */
void player_draw(void);
void player_setup_title(void);
void player_tick_scroll(void);

#endif
