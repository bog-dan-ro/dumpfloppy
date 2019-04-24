/*
    disk.c: data structure representing an FM/MFM floppy disk

    Copyright (C) 2013, 2019 Adam Sampson <ats@offog.org>

    Permission to use, copy, modify, and/or distribute this software for
    any purpose with or without fee is hereby granted, provided that the
    above copyright notice and this permission notice appear in all
    copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
    WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
    AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
    DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
    PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
    TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
    PERFORMANCE OF THIS SOFTWARE.
*/

#include "disk.h"
#include "util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

int sector_bytes(int code) {
    return 128 << code;
}

// Following what the .IMD spec says, the rates here are the data transfer rate
// to the drive -- FM-500k transfers half as much data as MFM-500k owing to the
// less efficient encoding.
const data_mode_t DATA_MODES[] = {
    // 5.25" DD/QD and 3.5" DD drives
    { 5, "MFM-250k", 2, false },
    { 2, "FM-250k", 2, true },

    // DD media in 5.25" HD drives
    { 4, "MFM-300k", 1, false },
    { 1, "FM-300k", 1, true },

    // 3.5" HD, 5.25" HD and 8" drives
    { 3, "MFM-500k", 0, false },
    { 0, "FM-500k", 0, true },

    // 3.5" ED drives
    { 6, "MFM-1000k", 3, false }, // FIXME: not in IMD spec
    // Rate 3 for FM isn't allowed.

    { -1, NULL, 0, false }
};

static const sector_t EMPTY_SECTOR = {
    .status = SECTOR_MISSING,
    .log_cyl = 0xFF,
    .log_head = 0xFF,
    .log_sector = 0xFF,
    .phys_sector = 0xFF,
    .deleted = false,
    .data = NULL,
};

void init_sector(sector_t *sector) {
    *sector = EMPTY_SECTOR;
}

void free_sector(sector_t *sector) {
    sector->status = SECTOR_MISSING;
    free(sector->data);
    sector->data = NULL;
}

static const track_t EMPTY_TRACK = {
    .status = TRACK_UNKNOWN,
    .data_mode = NULL,
    .phys_cyl = -1,
    .phys_head = -1,
    .num_sectors = -1,
    .sector_size_code = -1,
};

void init_track(int phys_cyl, int phys_head, track_t *track) {
    *track = EMPTY_TRACK;
    track->phys_cyl = phys_cyl;
    track->phys_head = phys_head;
    for (int i = 0; i < MAX_SECS; i++) {
        init_sector(&(track->sectors[i]));
    }
}

void free_track(track_t *track) {
    track->status = TRACK_UNKNOWN;
    track->num_sectors = 0;
    for (int i = 0; i < MAX_SECS; i++) {
        free_sector(&(track->sectors[i]));
    }
}

static const disk_t EMPTY_DISK = {
    .comment = NULL,
    .comment_len = -1,
    .num_phys_cyls = 0,
    .num_phys_heads = 0,
};

void init_disk(disk_t *disk) {
    *disk = EMPTY_DISK;
    for (int cyl = 0; cyl < MAX_CYLS; cyl++) {
        for (int head = 0; head < MAX_HEADS; head++) {
            init_track(cyl, head, &(disk->tracks[cyl][head]));
        }
    }
}

void free_disk(disk_t *disk) {
    free(disk->comment);
    disk->comment = NULL;
    for (int cyl = 0; cyl < MAX_CYLS; cyl++) {
        for (int head = 0; head < MAX_HEADS; head++) {
            free_track(&(disk->tracks[cyl][head]));
        }
    }
}

void make_disk_comment(const char *program, const char *version, disk_t *disk) {
    time_t now = time(NULL);
    const struct tm *local = localtime(&now);

    disk->comment = alloc_sprintf(
        "%s %s: %02d/%02d/%04d %02d:%02d:%02d\r\n",
        program, version,
        local->tm_mday, local->tm_mon + 1, local->tm_year + 1900,
        local->tm_hour, local->tm_min, local->tm_sec);
    if (disk->comment == NULL) {
        die("out of memory");
    }
    disk->comment_len = strlen(disk->comment);
}

void copy_track_layout(const disk_t *disk, const track_t *src, track_t *dest) {
    if (src->status == TRACK_UNKNOWN) return;

    free_track(dest);

    dest->status = TRACK_GUESSED;
    dest->data_mode = src->data_mode;
    dest->num_sectors = src->num_sectors;
    dest->sector_size_code = src->sector_size_code;

    int cyl_diff = dest->phys_cyl - src->phys_cyl;
    for (int i = 0; i < src->num_sectors; i++) {
        const sector_t *src_sec = &(src->sectors[i]);
        sector_t *dest_sec = &(dest->sectors[i]);

        dest_sec->log_cyl = src_sec->log_cyl + cyl_diff;
        dest_sec->log_head = src_sec->log_head;
        dest_sec->log_sector = src_sec->log_sector;
        dest_sec->phys_sector = src_sec->phys_sector;
    }
}

void track_scan_sectors(track_t *track,
                        sector_t **lowest, sector_t **highest,
                        bool *contiguous) {
    bool seen[MAX_SECS];
    for (int i = 0; i < MAX_SECS; i++) {
        seen[i] = false;
    }

    *lowest = NULL;
    int lowest_id = MAX_SECS;
    *highest = NULL;
    int highest_id = 0;
    for (int i = 0; i < track->num_sectors; i++) {
        sector_t *sector = &(track->sectors[i]);
        const int id = sector->log_sector;

        seen[sector->log_sector] = true;

        if (id < lowest_id) {
            lowest_id = id;
            *lowest = sector;
        }
        if (id > highest_id) {
            highest_id = id;
            *highest = sector;
        }
    }

    *contiguous = true;
    for (int i = lowest_id; i < highest_id; i++) {
        if (!seen[i]) {
            *contiguous = false;
        }
    }
}

bool same_sector_addr(const sector_t *a, const sector_t *b) {
    if (a->log_cyl != b->log_cyl) return false;
    if (a->log_head != b->log_head) return false;
    if (a->log_sector != b->log_sector) return false;
    return true;
}
