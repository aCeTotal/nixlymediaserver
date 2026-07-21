/*
 * Nixly Media Server - Disk keepalive implementation
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include "keepalive.h"
#include "config.h"
#include "database.h"

/* Hvor ofte tråden vekker diskene mens en klient er aktiv. Må være godt
 * under diskenes spindown-timer (typisk 5-20 min). */
#define KEEPALIVE_TICK_SECS   25
/* Hold diskene våkne så lenge etter siste stream-aktivitet. Dekker pauser,
 * mellom-episode-gap og bibliotek-bla etter avspilling. Klienten poker
 * /stream jevnlig når den er aktiv (egen keepalive i menyer/pause), så dette
 * vinduet trenger bare å ri over korte hull. */
#define KEEPALIVE_WINDOW_SECS 900
/* O_DIRECT krever justert buffer + lengde. 4 KiB dekker alle vanlige
 * sektor-/blokkstørrelser. */
#define WAKE_BLOCK 4096

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_last_activity = 0;

void keepalive_notify_activity(void) {
    pthread_mutex_lock(&g_lock);
    g_last_activity = time(NULL);
    pthread_mutex_unlock(&g_lock);
}

static time_t last_activity(void) {
    pthread_mutex_lock(&g_lock);
    time_t t = g_last_activity;
    pthread_mutex_unlock(&g_lock);
    return t;
}

/* Fysisk lesning av én blokk → tvinger platen ut av standby. O_DIRECT går
 * utenom page-cache = garantert disk-I/O. Faller tilbake til fadvise+read
 * (best-effort) om filsystemet ikke støtter O_DIRECT. */
static void wake_file(const char *path) {
    int direct = 1;
    int fd = open(path, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        fd = open(path, O_RDONLY);
        direct = 0;
    }
    if (fd < 0) return;

    void *buf = NULL;
    if (posix_memalign(&buf, WAKE_BLOCK, WAKE_BLOCK) != 0 || !buf) {
        close(fd);
        return;
    }
    if (!direct) {
        /* Uten O_DIRECT kan lesningen serveres fra cache. Dropp cachen for
         * området først så read faktisk treffer platen. */
        posix_fadvise(fd, 0, WAKE_BLOCK, POSIX_FADV_DONTNEED);
    }
    ssize_t n = pread(fd, buf, WAKE_BLOCK, 0);
    (void)n;
    free(buf);
    close(fd);
}

/* Vekk hver fysiske media-disk én gang. Dedup på st_dev — flere media_paths
 * kan ligge på samme disk. */
static void wake_all_disks(void) {
    dev_t seen[MAX_WATCH_PATHS];
    int seen_count = 0;
    for (int i = 0; i < server_config.media_path_count; i++) {
        char *path = database_get_sample_filepath(server_config.media_paths[i]);
        if (!path) continue;
        struct stat st;
        if (stat(path, &st) == 0) {
            int dup = 0;
            for (int j = 0; j < seen_count; j++) {
                if (seen[j] == st.st_dev) { dup = 1; break; }
            }
            if (!dup) {
                if (seen_count < MAX_WATCH_PATHS) seen[seen_count++] = st.st_dev;
                wake_file(path);
            }
        }
        free(path);
    }
}

static void *keepalive_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(KEEPALIVE_TICK_SECS);
        if (time(NULL) - last_activity() <= KEEPALIVE_WINDOW_SECS) {
            wake_all_disks();
        }
    }
    return NULL;
}

void keepalive_start(void) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, keepalive_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("  Disk keepalive: media disks kept awake %ds after each stream\n",
               KEEPALIVE_WINDOW_SECS);
    } else {
        fprintf(stderr, "Warning: failed to start disk keepalive thread\n");
    }
}
