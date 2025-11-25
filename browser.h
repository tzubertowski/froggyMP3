#ifndef FROGGY_BROWSER_H
#define FROGGY_BROWSER_H

#define MAX_FILES    256
#define MAX_PATH     512
#define MAX_NAME     128
#define VISIBLE_ROWS 8

typedef struct {
    char name[MAX_NAME];
    int  is_dir;
} FileEntry;

/* Browser state - owned by browser.c */
extern char         browser_path[MAX_PATH];
extern FileEntry    browser_files[MAX_FILES];
extern int          browser_count;
extern int          browser_sel;
extern int          browser_scroll;

/* Directory operations */
void browser_scan(void);
void browser_enter(const char *name);
void browser_back(void);

/* Rendering */
void browser_draw(void);

/* Helpers */
int  browser_is_mp3(const char *name);
int  browser_find_song(const char *name);
int  browser_count_songs(void);
int  browser_next_song(int cur);
int  browser_prev_song(int cur);
int  browser_rand_song(int cur);

#endif
