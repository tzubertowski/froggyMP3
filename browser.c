#include "browser.h"
#include "ui.h"
#include "font.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef SF2000
#include "dirent.h"
#else
#include <dirent.h>
#endif

/* State */
char      browser_path[MAX_PATH] = "/mnt/sda1/ROMS/MP3";
FileEntry browser_files[MAX_FILES];
int       browser_count  = 0;
int       browser_sel    = 0;
int       browser_scroll = 0;

/* Simple PRNG for random mode */
static unsigned int prng_state = 12345;
static int prng_next(void)
{
    prng_state = prng_state * 1103515245 + 12345;
    return (prng_state >> 16) & 0x7FFF;
}

/* Sort comparator: parent dir first, then dirs, then files (case-insensitive) */
static int cmp_entry(const void *a, const void *b)
{
    const FileEntry *ea = a;
    const FileEntry *eb = b;

    if (strcmp(ea->name, "..") == 0) return -1;
    if (strcmp(eb->name, "..") == 0) return  1;
    if (ea->is_dir && !eb->is_dir)   return -1;
    if (!ea->is_dir && eb->is_dir)   return  1;
    return strcasecmp(ea->name, eb->name);
}

int browser_is_mp3(const char *name)
{
    int len = strlen(name);
    if (len < 4) return 0;
    return strcasecmp(name + len - 4, ".mp3") == 0;
}

void browser_scan(void)
{
    DIR *d;
    struct dirent *e;

    browser_count  = 0;
    browser_sel    = 0;
    browser_scroll = 0;

    d = opendir(browser_path);
    if (!d) return;

    /* Add ".." unless at root */
    if (strcmp(browser_path, "/") != 0 &&
        strcmp(browser_path, "/mnt/sda1") != 0) {
        strcpy(browser_files[browser_count].name, "..");
        browser_files[browser_count].is_dir = 1;
        browser_count++;
    }

    while ((e = readdir(d)) && browser_count < MAX_FILES) {
        if (e->d_name[0] == '.') continue;

        if (e->d_type == DT_DIR || e->d_type == DTYPE_DIRECTORY) {
            strncpy(browser_files[browser_count].name, e->d_name, MAX_NAME - 1);
            browser_files[browser_count].name[MAX_NAME - 1] = '\0';
            browser_files[browser_count].is_dir = 1;
            browser_count++;
        } else if (browser_is_mp3(e->d_name)) {
            strncpy(browser_files[browser_count].name, e->d_name, MAX_NAME - 1);
            browser_files[browser_count].name[MAX_NAME - 1] = '\0';
            browser_files[browser_count].is_dir = 0;
            browser_count++;
        }
    }
    closedir(d);

    if (browser_count > 1)
        qsort(browser_files, browser_count, sizeof(FileEntry), cmp_entry);
}

void browser_enter(const char *name)
{
    char tmp[MAX_PATH];
    if (strcmp(name, "..") == 0) {
        browser_back();
        return;
    }
    snprintf(tmp, MAX_PATH, "%s/%s", browser_path, name);
    strncpy(browser_path, tmp, MAX_PATH - 1);
    browser_path[MAX_PATH - 1] = '\0';
    browser_scan();
}

void browser_back(void)
{
    char *slash = strrchr(browser_path, '/');
    if (slash && slash != browser_path)
        *slash = '\0';
    browser_scan();
}

void browser_draw(void)
{
    int start, end, i, y, selected;
    char label[MAX_NAME + 4];
    int legend_y, tw;
    const char *leg;

    ui_clear(COL_BG);

    /* header */
    font_draw_text(pixels, SCREEN_W, SCREEN_H, 10, 8, "MP3", COL_DIM);

    /* file list */
    start = browser_scroll;
    end   = browser_scroll + VISIBLE_ROWS;
    if (end > browser_count) end = browser_count;

    for (i = start; i < end; i++) {
        y = 36 + (i - start) * 24;
        selected = (i == browser_sel);

        if (browser_files[i].is_dir)
            snprintf(label, sizeof(label), "[%s]", browser_files[i].name);
        else {
            strncpy(label, browser_files[i].name, MAX_NAME);
            label[MAX_NAME] = '\0';
        }

        /* truncate long names */
        if (strlen(label) > 26) {
            label[23] = '.';
            label[24] = '.';
            label[25] = '.';
            label[26] = '\0';
        }

        if (selected)
            ui_text_pill(10, y, label, COL_SELECT_BG, COL_SELECT_FG, 7);
        else
            font_draw_text(pixels, SCREEN_W, SCREEN_H, 10, y, label,
                           browser_files[i].is_dir ? COL_TEXT : COL_TEXT);
    }

    /* scrollbar */
    if (browser_count > VISIBLE_ROWS) {
        int bar_h = (VISIBLE_ROWS * 24 * VISIBLE_ROWS) / browser_count;
        int max_s = browser_count - VISIBLE_ROWS;
        int bar_y;
        if (bar_h < 10) bar_h = 10;
        bar_y = 36 + (browser_scroll * (VISIBLE_ROWS * 24 - bar_h)) / max_s;
        ui_fill(SCREEN_W - 4, bar_y, 3, bar_h, COL_DIM);
    }

    /* legend */
    legend_y = SCREEN_H - 24;
    leg = " A - SELECT ";
    tw = font_measure_text(leg);
    ui_legend_btn(SCREEN_W - tw - 14, legend_y, leg);
}

int browser_find_song(const char *name)
{
    int i;
    for (i = 0; i < browser_count; i++) {
        if (!browser_files[i].is_dir && strcmp(browser_files[i].name, name) == 0)
            return i;
    }
    return -1;
}

int browser_count_songs(void)
{
    int i, n = 0;
    for (i = 0; i < browser_count; i++)
        if (!browser_files[i].is_dir) n++;
    return n;
}

int browser_next_song(int cur)
{
    int i, start = (cur < 0) ? 0 : cur + 1;

    for (i = start; i < browser_count; i++)
        if (!browser_files[i].is_dir) return i;
    for (i = 0; i < start && i < browser_count; i++)
        if (!browser_files[i].is_dir) return i;
    return -1;
}

int browser_prev_song(int cur)
{
    int i, start = (cur <= 0) ? browser_count - 1 : cur - 1;

    for (i = start; i >= 0; i--)
        if (!browser_files[i].is_dir) return i;
    for (i = browser_count - 1; i > start; i--)
        if (!browser_files[i].is_dir) return i;
    return -1;
}

int browser_rand_song(int cur)
{
    int total = browser_count_songs();
    int tries, target, cnt, i;

    if (total == 0) return -1;
    if (total == 1) return browser_next_song(-1);

    for (tries = 10; tries > 0; tries--) {
        target = prng_next() % total;
        cnt = 0;
        for (i = 0; i < browser_count; i++) {
            if (!browser_files[i].is_dir) {
                if (cnt == target && i != cur) return i;
                cnt++;
            }
        }
    }
    return browser_next_song(cur);
}
