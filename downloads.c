/*
 * Nixly Media Server - Downloads Module
 * Per-slot libcurl downloads with progress tracking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>

/* Give up after this many failed transfer attempts (resume between each). */
#define DOWNLOAD_MAX_RETRIES 5

#include "downloads.h"
#include "config.h"
#include "errors.h"

static DownloadSlot slots[DOWNLOAD_MAX_SLOTS];
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
static int next_id = 1;

static void extract_filename_from_url(const char *url, char *out, size_t out_size) {
    const char *q = strchr(url, '?');
    const char *end = q ? q : url + strlen(url);
    const char *slash = end;
    while (slash > url && *(slash - 1) != '/') slash--;
    size_t len = end - slash;
    if (len == 0 || len >= out_size) {
        snprintf(out, out_size, "download-%ld", (long)time(NULL));
        return;
    }
    memcpy(out, slash, len);
    out[len] = '\0';

    /* basic URL-decode for %20 → space */
    char *r = out, *w = out;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Free bytes on the filesystem holding `path`. The directory may not exist
 * yet, so walk up to the nearest existing ancestor. */
static unsigned long long free_bytes(const char *path) {
    char buf[MAX_PATH_LEN];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (;;) {
        struct statvfs v;
        if (statvfs(buf, &v) == 0)
            return (unsigned long long)v.f_bavail * v.f_frsize;
        char *slash = strrchr(buf, '/');
        if (!slash || slash == buf) return 0;
        *slash = '\0';
    }
}

/* Per download: the configured primary/secondary destination whose disk has
 * the most free space. Empty secondary means primary only. */
static const char *pick_dest_base(const char *primary, const char *secondary) {
    if (!secondary[0]) return primary;
    return free_bytes(secondary) > free_bytes(primary) ? secondary : primary;
}

static void mkdir_p(const char *path) {
    char buf[DOWNLOAD_URL_LEN];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(buf, 0755); *p = '/'; }
    }
    mkdir(buf, 0755);
}

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    DownloadSlot *slot = (DownloadSlot *)userdata;
    double now = mono_now();
    pthread_mutex_lock(&slots_lock);
    /* dltotal/dlnow only cover the current attempt — add bytes already on
     * disk from previous attempts (resume). */
    int64_t done = slot->resume_offset + (int64_t)dlnow;
    if (dltotal > 0)
        slot->total_bytes = slot->resume_offset + (int64_t)dltotal;
    slot->downloaded_bytes = done;

    /* Sample at most ~2× per second to keep EMA noise low. */
    if (slot->last_sample_time == 0.0) {
        slot->last_sample_time = now;
        slot->last_sample_bytes = done;
    } else {
        double dt = now - slot->last_sample_time;
        if (dt >= 0.5) {
            int64_t db = done - slot->last_sample_bytes;
            if (db < 0) db = 0;
            int64_t inst = (int64_t)((double)db / dt);
            /* EMA: smooth out brief stalls/spikes. */
            if (slot->speed_bps <= 0) slot->speed_bps = inst;
            else slot->speed_bps = (int64_t)(0.3 * (double)inst + 0.7 * (double)slot->speed_bps);
            slot->last_sample_time = now;
            slot->last_sample_bytes = done;
        }
    }
    pthread_mutex_unlock(&slots_lock);
    return 0;
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

static void *download_thread(void *arg) {
    DownloadSlot *slot = (DownloadSlot *)arg;

    pthread_mutex_lock(&slots_lock);
    slot->state = DL_STATE_ACTIVE;
    pthread_mutex_unlock(&slots_lock);

    char dest_dir[sizeof(slot->dest_path)];
    strncpy(dest_dir, slot->dest_path, sizeof(dest_dir) - 1);
    dest_dir[sizeof(dest_dir) - 1] = '\0';
    char *last_slash = strrchr(dest_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir_p(dest_dir);
    }

    /* Write to a .nixlypart temp file, then atomic rename when complete.
     * Watcher skips .nixlypart so scanner only sees the file once it's whole. */
    char tmp_path[sizeof(slot->dest_path) + 16];
    snprintf(tmp_path, sizeof(tmp_path), "%s.nixlypart", slot->dest_path);

    time_t t0 = time(NULL);
    CURLcode rc = CURLE_OK;
    int attempt = 0;

    for (;;) {
        /* Resume from whatever a previous attempt left on disk. */
        int64_t offset = 0;
        struct stat st;
        if (stat(tmp_path, &st) == 0) offset = st.st_size;

        pthread_mutex_lock(&slots_lock);
        slot->resume_offset = offset;
        slot->downloaded_bytes = offset;
        slot->last_sample_time = 0.0;
        slot->last_sample_bytes = 0;
        slot->speed_bps = 0;
        pthread_mutex_unlock(&slots_lock);

        FILE *fp = fopen(tmp_path, offset > 0 ? "ab" : "wb");
        if (!fp) {
            pthread_mutex_lock(&slots_lock);
            slot->state = DL_STATE_FAILED;
            snprintf(slot->error, sizeof(slot->error), "cannot open temp file");
            pthread_mutex_unlock(&slots_lock);
            return NULL;
        }

        CURL *curl = curl_easy_init();
        if (!curl) {
            fclose(fp);
            pthread_mutex_lock(&slots_lock);
            slot->state = DL_STATE_FAILED;
            snprintf(slot->error, sizeof(slot->error), "curl init failed");
            pthread_mutex_unlock(&slots_lock);
            return NULL;
        }

        curl_easy_setopt(curl, CURLOPT_URL, slot->url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, slot);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "nixly-server/1.0");
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        /* Abort transfers stuck under 1 KB/s for 60s — otherwise a dead
         * connection hangs forever and the slot stays "active" doing nothing. */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        if (offset > 0)
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)offset);

        rc = curl_easy_perform(curl);
        fclose(fp);
        curl_easy_cleanup(curl);

        if (rc == CURLE_OK) break;

        error_log("download", "%s — %s (attempt %d/%d)", slot->filename,
                  curl_easy_strerror(rc), attempt + 1, DOWNLOAD_MAX_RETRIES);

        /* Server can't do byte ranges — partial data unusable, start over. */
        if (rc == CURLE_RANGE_ERROR) remove(tmp_path);

        if (++attempt >= DOWNLOAD_MAX_RETRIES) break;

        int delay = 5 << attempt;
        if (delay > 60) delay = 60;
        sleep(delay);
    }

    time_t t1 = time(NULL);

    pthread_mutex_lock(&slots_lock);
    if (rc == CURLE_OK) {
        /* Atomic rename — only after this does watcher see the final file. */
        if (rename(tmp_path, slot->dest_path) != 0) {
            slot->state = DL_STATE_FAILED;
            snprintf(slot->error, sizeof(slot->error), "rename failed");
            remove(tmp_path);
        } else {
            slot->state = DL_STATE_DONE;
            if (slot->total_bytes == 0) slot->total_bytes = slot->downloaded_bytes;
            int dt = (int)(t1 - t0);
            slot->speed_bps = dt > 0 ? slot->downloaded_bytes / dt : 0;
        }
    } else {
        slot->state = DL_STATE_FAILED;
        snprintf(slot->error, sizeof(slot->error), "%s", curl_easy_strerror(rc));
        /* Keep the .nixlypart — re-adding the same URL resumes from it. */
    }
    pthread_mutex_unlock(&slots_lock);
    return NULL;
}

int downloads_init(void) {
    memset(slots, 0, sizeof(slots));
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return 0;
}

void downloads_cleanup(void) {
    curl_global_cleanup();
}

int downloads_add(const char *url, DownloadType type) {
    if (!url || !*url) return -1;

    pthread_mutex_lock(&slots_lock);
    int idx = -1;
    for (int i = 0; i < DOWNLOAD_MAX_SLOTS; i++) {
        if (slots[i].state == DL_STATE_IDLE ||
            slots[i].state == DL_STATE_DONE ||
            slots[i].state == DL_STATE_FAILED) {
            if (slots[i].thread_started) {
                pthread_join(slots[i].thread, NULL);
                slots[i].thread_started = 0;
            }
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&slots_lock);
        return -1;
    }

    DownloadSlot *slot = &slots[idx];
    memset(slot, 0, sizeof(*slot));
    slot->id = next_id++;
    slot->type = type;
    slot->state = DL_STATE_QUEUED;
    strncpy(slot->url, url, sizeof(slot->url) - 1);
    extract_filename_from_url(url, slot->filename, sizeof(slot->filename));

    const char *base = (type == DL_TYPE_TV)
        ? pick_dest_base(server_config.tv_download_path, server_config.tv_download_path2)
        : pick_dest_base(server_config.movie_download_path, server_config.movie_download_path2);
    snprintf(slot->dest_path, sizeof(slot->dest_path), "%s/%s", base, slot->filename);

    if (pthread_create(&slot->thread, NULL, download_thread, slot) != 0) {
        slot->state = DL_STATE_FAILED;
        snprintf(slot->error, sizeof(slot->error), "thread create failed");
        pthread_mutex_unlock(&slots_lock);
        return -1;
    }
    slot->thread_started = 1;
    pthread_detach(slot->thread);

    int id = slot->id;
    pthread_mutex_unlock(&slots_lock);
    return id;
}

static void json_escape(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_size; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 3 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = src[i];
        } else if ((unsigned char)src[i] < 0x20) {
            continue;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

char *downloads_status_json(void) {
    size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t used = 0;

    used += snprintf(buf + used, cap - used, "{\"slots\":[");

    pthread_mutex_lock(&slots_lock);
    int first = 1;
    for (int i = 0; i < DOWNLOAD_MAX_SLOTS; i++) {
        DownloadSlot *s = &slots[i];
        if (s->state == DL_STATE_IDLE) continue;

        if (used + 1024 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }

        char fn_esc[1024], err_esc[512];
        json_escape(s->filename, fn_esc, sizeof(fn_esc));
        json_escape(s->error, err_esc, sizeof(err_esc));

        float pct = 0.0f;
        if (s->total_bytes > 0)
            pct = 100.0f * (float)s->downloaded_bytes / (float)s->total_bytes;

        const char *st = "queued";
        if (s->state == DL_STATE_ACTIVE) st = "active";
        else if (s->state == DL_STATE_DONE) st = "done";
        else if (s->state == DL_STATE_FAILED) st = "failed";

        if (!first) buf[used++] = ',';
        first = 0;

        int64_t eta = -1;
        if (s->state == DL_STATE_ACTIVE && s->speed_bps > 0 && s->total_bytes > 0) {
            int64_t remaining = s->total_bytes - s->downloaded_bytes;
            if (remaining < 0) remaining = 0;
            eta = remaining / s->speed_bps;
        }

        used += snprintf(buf + used, cap - used,
            "{\"id\":%d,\"type\":\"%s\",\"state\":\"%s\","
            "\"filename\":\"%s\",\"total\":%lld,\"downloaded\":%lld,"
            "\"percent\":%.1f,\"speed_bps\":%lld,\"eta\":%lld,\"error\":\"%s\"}",
            s->id,
            s->type == DL_TYPE_TV ? "tv" : "movie",
            st, fn_esc,
            (long long)s->total_bytes,
            (long long)s->downloaded_bytes,
            pct,
            (long long)s->speed_bps,
            (long long)eta,
            err_esc);
    }
    pthread_mutex_unlock(&slots_lock);

    used += snprintf(buf + used, cap - used, "]}");
    return buf;
}
