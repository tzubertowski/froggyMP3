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

/* Player state - owned by player.c */
extern PlayMode player_mode;
extern char     player_song[128];
extern int      player_song_idx;
extern int      player_paused;
extern int      player_loading;
extern int      player_warmup;

/* Decoder state accessible for audio output */
extern void    *player_mad;
extern char    *player_data;
extern uint32_t player_len;
extern uint32_t player_pos;
extern uint32_t player_start;
extern uint32_t player_rate;
extern uint32_t player_bitrate;
extern uint32_t player_channels;
extern int16_t *player_pcm;
extern uint16_t player_pcm_fill;

/* Core operations */
int   player_load(const char *path);
void  player_unload(void);
void  player_seek(int secs);
void  player_toggle_pause(void);
char *player_get_data(uint32_t pos, uint32_t need);
void  player_bg_load(void);

/* Playback control */
void player_play_at(int idx);
void player_next(void);
void player_prev(void);
void player_random(void);
void player_cycle_mode(int dir);

/* Called when song ends naturally */
void player_on_end(void);

/* Rendering */
void player_draw(void);
void player_setup_title(void);

/* Title scroll update (call each frame when playing) */
void player_tick_scroll(int *dirty);

#endif
