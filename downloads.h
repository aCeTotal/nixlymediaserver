/*
 * Nixly Media Server - Downloads Module
 * libcurl-based file downloader with per-slot progress tracking.
 * Routes TV vs Movie downloads to configured destination paths.
 */

#ifndef DOWNLOADS_H
#define DOWNLOADS_H

#include <stdint.h>
#include <pthread.h>

#define DOWNLOAD_MAX_SLOTS 64
#define DOWNLOAD_URL_LEN 2048
#define DOWNLOAD_NAME_LEN 512

typedef enum {
    DL_TYPE_TV = 0,
    DL_TYPE_MOVIE = 1
} DownloadType;

typedef enum {
    DL_STATE_IDLE = 0,
    DL_STATE_QUEUED = 1,
    DL_STATE_ACTIVE = 2,
    DL_STATE_DONE = 3,
    DL_STATE_FAILED = 4
} DownloadState;

typedef struct {
    int id;
    DownloadType type;
    DownloadState state;
    char url[DOWNLOAD_URL_LEN];
    char filename[DOWNLOAD_NAME_LEN];
    char dest_path[DOWNLOAD_URL_LEN];
    int64_t total_bytes;
    int64_t downloaded_bytes;
    int64_t resume_offset;        /* bytes already on disk when attempt started */
    int64_t speed_bps;            /* live for active, final avg for done */
    /* Sampling state for live-speed EMA. Updated from libcurl progress_cb. */
    double last_sample_time;      /* CLOCK_MONOTONIC seconds */
    int64_t last_sample_bytes;
    char error[256];
    pthread_t thread;
    int thread_started;
} DownloadSlot;

int downloads_init(void);
void downloads_cleanup(void);

/* Submit a URL for download. Returns slot id (>=0) or -1 on failure. */
int downloads_add(const char *url, DownloadType type);

/* Build JSON snapshot of all active/completed slots. Caller free()s. */
char *downloads_status_json(void);

#endif /* DOWNLOADS_H */
