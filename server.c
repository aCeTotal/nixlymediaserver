/*
 * Nixly Media Server
 * Lossless streaming server for movies and TV shows with TMDB metadata
 *
 * - Serves media files without transcoding (full quality)
 * - SQLite database with TMDB metadata
 * - HTTP server with range request support for seeking
 * - Real-time file watching with inotify
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>

#include "database.h"
#include "scanner.h"
#include "config.h"
#include "tmdb.h"
#include <sys/syscall.h>
#include "watcher.h"
#include "downloads.h"
#include "pages.h"
#include "errors.h"

/* No connection limit - kernel handles backlog */
#define BUFFER_SIZE 1048576  /* 1MB for efficient streaming */
#define STREAM_CHUNK_SIZE 4194304  /* 4MB sendfile chunks — fewer syscalls for large UHD files */
#define MAX_PATH 4096
#define MAX_HEADER 8192
#define DISCOVERY_PORT 8081
#define DISCOVERY_MAGIC "NIXLY_DISCOVER"
#define DISCOVERY_RESPONSE "NIXLY_SERVER"

static int server_fd = -1;
static int discovery_fd = -1;
static volatile int running = 1;
static volatile int startup_scanning = 0;  /* 1 while initial scan/TMDB fetch is running */

/* Concurrent streaming gate — at most MAX_STREAM_IPS distinct client IPs
 * may stream at once. The same IP can open any number of streams (tabs,
 * range requests, multiple files) without counting against the limit.
 * Each stream runs at full disk/network speed — no per-stream throttle. */
#define MAX_STREAM_IPS 3
#define MAX_BLOCKED_IPS 128
typedef struct {
    uint32_t ip;
    int refcount;
    time_t first_seen;
    int file_id;
    int media_type;          /* 0=movie, 1=tvshow, 2=episode */
    int season, episode;
    int64_t file_size;
    char title[256];
    char show_name[256];
    char poster[512];
} StreamIpEntry;
static StreamIpEntry stream_ips[MAX_STREAM_IPS];
static uint32_t blocked_ips[MAX_BLOCKED_IPS];
static int blocked_count = 0;
static int active_streams = 0;  /* total connections, informational */
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_ip_blocked_locked(uint32_t ip) {
    for (int i = 0; i < blocked_count; i++)
        if (blocked_ips[i] == ip) return 1;
    return 0;
}

static int is_ip_blocked(uint32_t ip) {
    int blocked;
    pthread_mutex_lock(&stream_lock);
    blocked = is_ip_blocked_locked(ip);
    pthread_mutex_unlock(&stream_lock);
    return blocked;
}

/* Returns 0 = granted, -1 = capacity full, -2 = ip is blocked. */
static int stream_ip_acquire(uint32_t ip) {
    pthread_mutex_lock(&stream_lock);
    if (is_ip_blocked_locked(ip)) {
        pthread_mutex_unlock(&stream_lock);
        return -2;
    }
    for (int i = 0; i < MAX_STREAM_IPS; i++) {
        if (stream_ips[i].refcount > 0 && stream_ips[i].ip == ip) {
            stream_ips[i].refcount++;
            active_streams++;
            pthread_mutex_unlock(&stream_lock);
            return 0;
        }
    }
    for (int i = 0; i < MAX_STREAM_IPS; i++) {
        if (stream_ips[i].refcount == 0) {
            stream_ips[i].ip = ip;
            stream_ips[i].refcount = 1;
            stream_ips[i].first_seen = time(NULL);
            stream_ips[i].file_id = 0;
            stream_ips[i].title[0] = '\0';
            stream_ips[i].show_name[0] = '\0';
            stream_ips[i].poster[0] = '\0';
            stream_ips[i].season = 0;
            stream_ips[i].episode = 0;
            stream_ips[i].file_size = 0;
            stream_ips[i].media_type = 0;
            active_streams++;
            pthread_mutex_unlock(&stream_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&stream_lock);
    return -1;
}

static void stream_ip_release(uint32_t ip) {
    pthread_mutex_lock(&stream_lock);
    for (int i = 0; i < MAX_STREAM_IPS; i++) {
        if (stream_ips[i].refcount > 0 && stream_ips[i].ip == ip) {
            stream_ips[i].refcount--;
            if (active_streams > 0) active_streams--;
            break;
        }
    }
    pthread_mutex_unlock(&stream_lock);
}

static void stream_ip_set_meta(uint32_t ip, int file_id) {
    char title[256] = "", show[256] = "", poster[512] = "";
    int season = 0, episode = 0, type = 0;
    int64_t size = 0;
    if (database_get_stream_meta(file_id, title, sizeof(title),
            show, sizeof(show), poster, sizeof(poster),
            &season, &episode, &type, &size) != 0) {
        return;
    }
    pthread_mutex_lock(&stream_lock);
    for (int i = 0; i < MAX_STREAM_IPS; i++) {
        if (stream_ips[i].refcount > 0 && stream_ips[i].ip == ip) {
            stream_ips[i].file_id = file_id;
            stream_ips[i].media_type = type;
            stream_ips[i].season = season;
            stream_ips[i].episode = episode;
            stream_ips[i].file_size = size;
            strncpy(stream_ips[i].title, title, sizeof(stream_ips[i].title) - 1);
            strncpy(stream_ips[i].show_name, show, sizeof(stream_ips[i].show_name) - 1);
            strncpy(stream_ips[i].poster, poster, sizeof(stream_ips[i].poster) - 1);
            stream_ips[i].title[sizeof(stream_ips[i].title) - 1] = '\0';
            stream_ips[i].show_name[sizeof(stream_ips[i].show_name) - 1] = '\0';
            stream_ips[i].poster[sizeof(stream_ips[i].poster) - 1] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&stream_lock);
}

static void stream_status_snapshot(uint32_t requester_ip,
                                   int *active_ips, int *can_start) {
    int n = 0, has_requester = 0, blocked;
    pthread_mutex_lock(&stream_lock);
    blocked = is_ip_blocked_locked(requester_ip);
    for (int i = 0; i < MAX_STREAM_IPS; i++) {
        if (stream_ips[i].refcount > 0) {
            n++;
            if (stream_ips[i].ip == requester_ip) has_requester = 1;
        }
    }
    pthread_mutex_unlock(&stream_lock);
    *active_ips = n;
    *can_start = (!blocked && (has_requester || n < MAX_STREAM_IPS)) ? 1 : 0;
}

static int parse_ipv4(const char *s, uint32_t *out) {
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return -1;
    *out = a.s_addr;
    return 0;
}

static int block_ip(uint32_t ip) {
    pthread_mutex_lock(&stream_lock);
    if (is_ip_blocked_locked(ip)) {
        pthread_mutex_unlock(&stream_lock);
        return 0;
    }
    if (blocked_count >= MAX_BLOCKED_IPS) {
        pthread_mutex_unlock(&stream_lock);
        return -1;
    }
    blocked_ips[blocked_count++] = ip;
    /* Force-disconnect: zero out any active sessions for this IP so the
     * gate appears closed to it on next request. The in-flight sendfile
     * loop continues until client disconnects or chunk ends — best we
     * can do without a connection registry. */
    for (int i = 0; i < MAX_STREAM_IPS; i++) {
        if (stream_ips[i].refcount > 0 && stream_ips[i].ip == ip) {
            stream_ips[i].refcount = 0;
            stream_ips[i].file_id = 0;
            stream_ips[i].title[0] = '\0';
            stream_ips[i].show_name[0] = '\0';
            stream_ips[i].poster[0] = '\0';
        }
    }
    pthread_mutex_unlock(&stream_lock);
    return 0;
}

static int unblock_ip(uint32_t ip) {
    pthread_mutex_lock(&stream_lock);
    for (int i = 0; i < blocked_count; i++) {
        if (blocked_ips[i] == ip) {
            blocked_ips[i] = blocked_ips[--blocked_count];
            pthread_mutex_unlock(&stream_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&stream_lock);
    return -1;
}

/* Forward declaration - defined after startup_scan_thread */
static void on_file_change(const char *filepath, int is_delete);

/* Background thread for /api/tmdb/rescan — non-blocking endpoint */
static void *tmdb_rescan_thread(void *arg) {
    (void)arg;
    scanner_scrape_session_begin(&tmdb_progress);
    scanner_rescan_all_tmdb();
    scanner_scrape_session_end(&tmdb_progress);
    printf("API: Background TMDB rescan complete\n");
    return NULL;
}

static void *scrape_tmdb_thread(void *arg) {
    (void)arg;
    printf("Scrape: TMDB thread started\n");
    scanner_fetch_missing_tmdb();
    scanner_refresh_show_status();
    printf("Scrape: TMDB thread done\n");
    return NULL;
}

/* Initial startup scan thread - runs heavy I/O and network work in background
 * so the HTTP server can accept connections immediately */
static void *startup_scan_thread(void *arg) {
    (void)arg;
    startup_scanning = 1;

    /* Clean up entries for files that no longer exist */
    printf("Startup scan: Checking for removed files...\n");
    int removed = database_cleanup_missing();
    if (removed > 0) {
        printf("Startup scan: Removed %d entries for missing files\n", removed);
    }

    /* Initial scan of configured media library paths */
    printf("Startup scan: Scanning media library paths...\n");
    for (int i = 0; i < server_config.media_path_count && running; i++) {
        printf("  Media path: %s\n", server_config.media_paths[i]);
        scanner_scan_directory(server_config.media_paths[i]);
    }
    printf("Startup scan: Found %d media files.\n", database_get_count());

    /* Initialize file watcher — instantly picks up new files in media paths */
    if (running && watcher_init() == 0) {
        watcher_set_callback(on_file_change);

        for (int i = 0; i < server_config.media_path_count; i++) {
            watcher_add_path(server_config.media_paths[i]);
        }

        watcher_start();
    }

    scanner_scrape_session_begin(&tmdb_progress);

    {
        pthread_t tmdb_thread;
        int have_tmdb = 0;

        if (running) {
            have_tmdb = (pthread_create(&tmdb_thread, NULL, scrape_tmdb_thread, NULL) == 0);
        }

        if (!have_tmdb && running) {
            scanner_fetch_missing_tmdb();
            scanner_refresh_show_status();
        }

        if (have_tmdb) pthread_join(tmdb_thread, NULL);
    }

    scanner_scrape_session_end(&tmdb_progress);

    startup_scanning = 0;
    printf("Startup scan: Complete. Server fully ready.\n");
    printf("  Media files: %d\n", database_get_count());
    printf("  Watching %d directories\n", watcher_get_count());
    return NULL;
}

/* Periodic sync thread - checks for changes every 5 minutes as backup to inotify */
static void *sync_thread(void *arg) {
    (void)arg;

    while (running) {
        for (int i = 0; i < 300 && running; i++) sleep(1);
        if (!running) break;

        printf("Periodic sync: Checking for changes...\n");

        int removed = database_cleanup_missing();
        if (removed > 0) {
            printf("Periodic sync: Removed %d missing entries\n", removed);
        }

        /* Scan for new files in media library paths */
        int before = database_get_count();
        for (int i = 0; i < server_config.media_path_count; i++) {
            scanner_scan_directory(server_config.media_paths[i]);
        }
        int after = database_get_count();

        if (after != before) {
            printf("Periodic sync: Media count changed %d -> %d\n", before, after);
        }

        scanner_scrape_session_begin(&tmdb_progress);

        {
            pthread_t tmdb_t;
            int have_tmdb = (pthread_create(&tmdb_t, NULL, scrape_tmdb_thread, NULL) == 0);
            if (!have_tmdb) {
                scanner_fetch_missing_tmdb();
                scanner_refresh_show_status();
            }
            if (have_tmdb) pthread_join(tmdb_t, NULL);
        }

        scanner_scrape_session_end(&tmdb_progress);
    }

    return NULL;
}

/* Discovery thread - responds to broadcast queries */
static void *discovery_thread(void *arg) {
    (void)arg;
    struct sockaddr_in addr, client_addr;
    socklen_t addr_len;
    char buf[128];
    char response[128];

    discovery_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (discovery_fd < 0) {
        perror("discovery socket");
        return NULL;
    }

    int reuse = 1;
    setsockopt(discovery_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(discovery_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("discovery bind");
        close(discovery_fd);
        discovery_fd = -1;
        return NULL;
    }

    printf("Discovery: Listening on UDP port %d\n", DISCOVERY_PORT);

    while (running) {
        addr_len = sizeof(client_addr);
        ssize_t len = recvfrom(discovery_fd, buf, sizeof(buf) - 1, 0,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (len > 0) {
            buf[len] = '\0';
            if (strncmp(buf, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC)) == 0) {
                /* Respond with our port */
                snprintf(response, sizeof(response), "%s:%d", DISCOVERY_RESPONSE, server_config.port);

                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                printf("Discovery: Request from %s, responding with port %d\n",
                       client_ip, server_config.port);

                sendto(discovery_fd, response, strlen(response), 0,
                       (struct sockaddr *)&client_addr, addr_len);
            }
        }
    }

    close(discovery_fd);
    discovery_fd = -1;
    return NULL;
}

typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
    int is_local;  /* 1 if client is on local network */
} ClientConnection;

/* Check if client IP is on local network (private IP ranges) */
static int is_local_client(struct sockaddr_in *addr) {
    uint32_t ip = ntohl(addr->sin_addr.s_addr);

    /* 127.0.0.0/8 - localhost */
    if ((ip >> 24) == 127) return 1;

    /* 10.0.0.0/8 - private */
    if ((ip >> 24) == 10) return 1;

    /* 172.16.0.0/12 - private */
    if ((ip >> 20) == (172 << 4 | 1)) return 1;

    /* 192.168.0.0/16 - private */
    if ((ip >> 16) == (192 << 8 | 168)) return 1;

    return 0;
}

/* File change callback - scrape new files, drop removed ones from DB.
 * Completeness gate: scanner_is_media_file rejects .nixlypart/.part/etc,
 * scanner_file_is_complete confirms size is stable + > 1 MiB + mtime old
 * enough. Files still being written are skipped and re-picked up either
 * on the next inotify event or by the periodic sync_thread. */
static void on_file_change(const char *filepath, int is_delete) {
    if (is_delete) {
        if (database_delete_by_path(filepath) == 0) {
            printf("DB: Removed %s\n", filepath);
        }
        return;
    }

    if (!scanner_is_media_file(filepath)) return;

    if (!scanner_file_is_complete(filepath)) {
        printf("DB: Deferring %s — file not yet complete\n", filepath);
        return;
    }

    int id = scanner_scan_file(filepath, 1);
    if (id > 0) {
        printf("DB: Added %s (id=%d)\n", filepath, id);
    }
}

/* HTTP Basic Auth — expected "Basic <b64(user:pass)>" string built at
 * startup from server_config.auth_user / auth_password, then compared as
 * a single constant-string check on every request. */
static char expected_auth_header[256] = "";

static void b64_encode(const char *in, size_t in_len, char *out, size_t out_size) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        if (o + 5 >= out_size) break;
        unsigned int v = (unsigned char)in[i] << 16;
        int pad = 0;
        if (i + 1 < in_len) v |= (unsigned char)in[i+1] << 8; else pad++;
        if (i + 2 < in_len) v |= (unsigned char)in[i+2];      else pad++;
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = pad >= 2 ? '=' : tbl[(v >> 6) & 0x3F];
        out[o++] = pad >= 1 ? '=' : tbl[v & 0x3F];
    }
    out[o] = '\0';
}

static void build_expected_auth(void) {
    char raw[256];
    int n = snprintf(raw, sizeof(raw), "%s:%s",
                     server_config.auth_user, server_config.auth_password);
    if (n <= 0 || n >= (int)sizeof(raw)) {
        expected_auth_header[0] = '\0';
        return;
    }
    char b64[384];
    b64_encode(raw, n, b64, sizeof(b64));
    snprintf(expected_auth_header, sizeof(expected_auth_header),
             "Basic %s", b64);
}

/* Returns 1 if request carries valid Authorization header, 0 otherwise. */
static int check_auth(const char *request) {
    if (!expected_auth_header[0]) return 1;  /* auth disabled (empty creds) */
    const char *h = strcasestr(request, "Authorization:");
    if (!h) return 0;
    h += 14;
    while (*h == ' ' || *h == '\t') h++;
    size_t exp_len = strlen(expected_auth_header);
    if (strncmp(h, expected_auth_header, exp_len) != 0) return 0;
    char tail = h[exp_len];
    return (tail == '\r' || tail == '\n' || tail == ' ' || tail == '\0');
}

static void send_401(int fd) {
    const char *body = "Authentication required";
    size_t body_len = strlen(body);
    char header[MAX_HEADER];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"Nixly\", charset=\"UTF-8\"\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", body_len);
    ssize_t w = write(fd, header, n); (void)w;
    w = write(fd, body, body_len); (void)w;
}

/* HTTP response helpers */
static void send_response(int fd, int status, const char *status_text,
                          const char *content_type, const char *body, size_t body_len) {
    char header[MAX_HEADER];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);

    write(fd, header, header_len);
    if (body && body_len > 0) {
        write(fd, body, body_len);
    }
}

static void send_error(int fd, int status, const char *message) {
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"error\": \"%s\"}", message);
    send_response(fd, status, message, "application/json", body, body_len);
}

/* Get MIME type for file extension */
static const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcasecmp(ext, ".mkv") == 0) return "video/x-matroska";
    if (strcasecmp(ext, ".avi") == 0) return "video/x-msvideo";
    if (strcasecmp(ext, ".mov") == 0) return "video/quicktime";
    if (strcasecmp(ext, ".webm") == 0) return "video/webm";
    if (strcasecmp(ext, ".m4v") == 0) return "video/x-m4v";
    if (strcasecmp(ext, ".ts") == 0) return "video/mp2t";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".html") == 0) return "text/html";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".png") == 0) return "image/png";

    return "application/octet-stream";
}

/* Serve a local file (for cached images) */
static void serve_file(int fd, const char *filepath) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        send_error(fd, 404, "File not found");
        return;
    }

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 500, "Cannot open file");
        return;
    }

    const char *mime = get_mime_type(filepath);
    char header[MAX_HEADER];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: max-age=86400\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, st.st_size);

    write(fd, header, header_len);

    char buffer[BUFFER_SIZE];
    ssize_t bytes;
    while ((bytes = read(file_fd, buffer, sizeof(buffer))) > 0) {
        write(fd, buffer, bytes);
    }

    close(file_fd);
}

/* Stream file with range request support (for seeking in video)
 * Uses sendfile() for zero-copy kernel-to-socket transfer - most efficient
 * for lossless streaming of large media files */
static void stream_file(int fd, const char *filepath, const char *range_header) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        send_error(fd, 404, "File not found");
        return;
    }

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 500, "Cannot open file");
        return;
    }

    /* Aggressive sequential readahead — tells kernel to prefetch ~2MB+ ahead
     * instead of default ~128KB.  Huge win for streaming large files from
     * busy HDDs where seek latency dominates. */
    posix_fadvise(file_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    /* Elevate this thread's I/O scheduling priority to best-effort class,
     * highest priority (0).  Transcoding threads run at IOPRIO_CLASS_IDLE,
     * so direct streaming always wins disk access. */
    syscall(SYS_ioprio_set, 1 /*IOPRIO_WHO_PROCESS*/, 0 /*self*/,
            (2 << 13) | 0 /*IOPRIO_CLASS_BE, prio 0*/);

    off_t start = 0;
    off_t end = st.st_size - 1;
    int partial = 0;

    /* Parse Range header: bytes=start-end */
    if (range_header) {
        if (sscanf(range_header, "bytes=%ld-%ld", &start, &end) >= 1) {
            partial = 1;
            if (end <= 0 || end >= st.st_size) {
                end = st.st_size - 1;
            }
        }
    }

    off_t content_length = end - start + 1;
    const char *mime = get_mime_type(filepath);

    char header[MAX_HEADER];
    int header_len;

    if (partial) {
        header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Content-Range: bytes %ld-%ld/%ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            mime, content_length, start, end, st.st_size);
    } else {
        header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            mime, st.st_size);
    }

    write(fd, header, header_len);

    /* Stream using sendfile() - zero-copy kernel-to-socket transfer
     * This is the most efficient way to stream large files:
     * - No user-space buffer copies
     * - Kernel handles DMA directly from disk to network
     * - Supports lossless transfer of any codec (video, audio, subtitles) */
    off_t offset = start;
    off_t remaining = content_length;

    while (remaining > 0 && running) {
        /* Send in chunks to allow checking 'running' flag and handle partial sends */
        size_t chunk = (remaining > STREAM_CHUNK_SIZE) ? STREAM_CHUNK_SIZE : remaining;
        ssize_t sent = sendfile(fd, file_fd, &offset, chunk);

        if (sent <= 0) {
            if (sent < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;  /* Retry on temporary errors */
            }
            break;  /* Client disconnected or error */
        }

        remaining -= sent;
    }

    close(file_fd);
}

/* URL-decode a percent-encoded string (e.g. %20 → space, + → space) */
static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/* Handle API requests */
static void handle_api(int fd, const char *method, const char *path,
                       const char *body, size_t body_len) {
    (void)body_len;
    (void)method;
    if (strcmp(path, "/api/status") == 0) {
        /* Status with server identity, rating, locality, and stream gate.
         * "status": "open" if requester is allowed to start a new stream
         * right now (i.e. their IP is already streaming OR fewer than
         * MAX_STREAM_IPS distinct IPs are active). "closed" otherwise.
         * Clients MUST poll this before initiating a stream. */
        int rating = config_get_server_rating();
        int is_local = config_get_client_local();
        int priority = config_get_server_priority();
        int active_ips = 0, can_start = 0;
        stream_status_snapshot(config_get_client_ip(), &active_ips, &can_start);
        const char *gate = can_start ? "open" : "closed";
        char json[640];
        int len = snprintf(json, sizeof(json),
            "{\"status\":\"%s\","
            "\"server_id\":\"%s\","
            "\"server_name\":\"%s\","
            "\"rating\":%d,"
            "\"is_local\":%s,"
            "\"priority\":%d,"
            "\"upload_mbps\":%d,"
            "\"active_streams\":%d,"
            "\"active_ips\":%d,"
            "\"max_ips\":%d,"
            "\"can_start\":%s,"
            "\"scanning\":%s}",
            gate,
            server_config.server_id,
            server_config.server_name,
            rating,
            is_local ? "true" : "false",
            priority,
            server_config.upload_mbps,
            active_streams, active_ips, MAX_STREAM_IPS,
            can_start ? "true" : "false",
            startup_scanning ? "true" : "false");
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/library") == 0) {
        /* Return entire media library as JSON */
        char *json = database_get_library_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strcmp(path, "/api/movies") == 0) {
        char *json = database_get_movies_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strcmp(path, "/api/tvshows") == 0) {
        char *json = database_get_tvshows_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strncmp(path, "/api/show/", 10) == 0) {
        /* /api/show/{show_name}/seasons or /api/show/{show_name}/episodes/{season} */
        char show_name[256];
        char *show_start = (char *)path + 10;
        char *seasons_pos = strstr(show_start, "/seasons");
        char *episodes_pos = strstr(show_start, "/episodes/");

        if (seasons_pos) {
            /* Extract show name (URL decoded) */
            size_t name_len = seasons_pos - show_start;
            if (name_len >= sizeof(show_name)) name_len = sizeof(show_name) - 1;
            strncpy(show_name, show_start, name_len);
            show_name[name_len] = '\0';

            /* Simple URL decode for spaces */
            for (char *p = show_name; *p; p++) {
                if (*p == '+') *p = ' ';
                else if (*p == '%' && p[1] == '2' && p[2] == '0') {
                    *p = ' ';
                    memmove(p + 1, p + 3, strlen(p + 3) + 1);
                }
            }

            char *json = database_get_show_seasons_json(show_name);
            if (json) {
                send_response(fd, 200, "OK", "application/json", json, strlen(json));
                free(json);
            } else {
                send_error(fd, 500, "Database error");
            }
        }
        else if (episodes_pos) {
            /* Extract show name */
            size_t name_len = episodes_pos - show_start;
            if (name_len >= sizeof(show_name)) name_len = sizeof(show_name) - 1;
            strncpy(show_name, show_start, name_len);
            show_name[name_len] = '\0';

            /* Simple URL decode */
            for (char *p = show_name; *p; p++) {
                if (*p == '+') *p = ' ';
                else if (*p == '%' && p[1] == '2' && p[2] == '0') {
                    *p = ' ';
                    memmove(p + 1, p + 3, strlen(p + 3) + 1);
                }
            }

            int season = atoi(episodes_pos + 10);
            char *json = database_get_show_episodes_json(show_name, season);
            if (json) {
                send_response(fd, 200, "OK", "application/json", json, strlen(json));
                free(json);
            } else {
                send_error(fd, 500, "Database error");
            }
        }
        else {
            send_error(fd, 400, "Invalid show endpoint");
        }
    }
    else if (strcmp(path, "/api/scan") == 0) {
        /* Trigger library rescan of media library paths */
        printf("API: Starting rescan...\n");
        int before = database_get_count();
        int total_found = 0;
        for (int i = 0; i < server_config.media_path_count; i++) {
            int found = scanner_scan_directory(server_config.media_paths[i]);
            printf("API: Scanned %s -> %d new files\n", server_config.media_paths[i], found);
            if (found > 0) total_found += found;
        }
        int after = database_get_count();
        char json[512];
        int len = snprintf(json, sizeof(json),
            "{\"status\":\"scan complete\",\"paths_scanned\":%d,"
            "\"new_files\":%d,\"total_before\":%d,\"total_after\":%d}",
            server_config.media_path_count, total_found, before, after);
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/paths") == 0) {
        /* Diagnostic: show configured paths and what exists */
        size_t buf_size = 8192;
        size_t buf_used = 0;
        char *json = malloc(buf_size);
        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "{\"media_path_count\":%d,\"media_paths\":[",
            server_config.media_path_count);
        for (int i = 0; i < server_config.media_path_count; i++) {
            struct stat st;
            int dir_exists = (stat(server_config.media_paths[i], &st) == 0 && S_ISDIR(st.st_mode));
            int file_count = 0;
            if (dir_exists) {
                file_count = scanner_scan_directory(server_config.media_paths[i]);
                if (file_count < 0) file_count = 0;
            }
            if (i > 0) json[buf_used++] = ',';
            buf_used += snprintf(json + buf_used, buf_size - buf_used,
                "{\"path\":\"%s\",\"exists\":%s,\"files_found\":%d}",
                server_config.media_paths[i],
                dir_exists ? "true" : "false", file_count);
        }
        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "],\"db_path\":\"%s\",\"total_media\":%d}",
            server_config.db_path, database_get_count());
        send_response(fd, 200, "OK", "application/json", json, buf_used);
        free(json);
    }
    else if (strcmp(path, "/api/tmdb/refresh") == 0) {
        /* Fetch missing TMDB metadata */
        printf("API: Fetching missing TMDB metadata...\n");
        scanner_fetch_missing_tmdb();
        send_response(fd, 200, "OK", "application/json",
                     "{\"status\": \"tmdb refresh complete\"}", 35);
    }
    else if (strcmp(path, "/api/tmdb/rescan") == 0) {
        /* Re-fetch TMDB metadata for ALL entries — run in background thread */
        printf("API: Full TMDB rescan starting (background)...\n");
        pthread_t rescan_thread;
        if (pthread_create(&rescan_thread, NULL, tmdb_rescan_thread, NULL) == 0) {
            pthread_detach(rescan_thread);
            send_response(fd, 200, "OK", "application/json",
                         "{\"status\":\"started\"}", 20);
        } else {
            send_response(fd, 500, "Internal Server Error", "application/json",
                         "{\"status\":\"failed to start rescan\"}", 35);
        }
    }
    else if (strncmp(path, "/api/media/", 11) == 0) {
        /* Get media info by ID */
        int id = atoi(path + 11);
        char *json = database_get_media_json(id);
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 404, "Media not found");
        }
    }
    else if (strcmp(path, "/api/scrape/status") == 0) {
        int t_active, t_total, t_processed, t_success;
        const char *t_operation;
        char t_item[256];
        time_t t_start;
        scanner_get_progress(&tmdb_progress, &t_active, &t_operation, t_item,
            sizeof(t_item), &t_total, &t_processed, &t_success, &t_start);

        char t_esc[512];
        char *d = t_esc;
        for (const char *s = t_item; *s && d < t_esc + sizeof(t_esc) - 6; s++) {
            if (*s == '"') { *d++ = '\\'; *d++ = '"'; }
            else if (*s == '\\') { *d++ = '\\'; *d++ = '\\'; }
            else *d++ = *s;
        }
        *d = '\0';

        int t_elapsed = t_active ? (int)(time(NULL) - t_start) : 0;
        float t_percent = (t_total > 0) ? (100.0f * t_processed / t_total) : 0;
        if (t_active && t_percent >= 100.0f) t_percent = 99.0f;
        int t_failed = t_processed - t_success; if (t_failed < 0) t_failed = 0;

        int media_count = database_get_count();

        /* Pull pending entries first so we know the count and items before
         * building the JSON. Capped sample of labels goes in pending_items. */
        MediaEntry *tmdb_entries = NULL;
        int tmdb_pending = 0;
        database_get_entries_without_tmdb(&tmdb_entries, &tmdb_pending);

        const int max_items = 50;
        size_t items_sz = 4096;
        char *items = malloc(items_sz);
        items[0] = '\0';
        size_t items_len = 0;
        int emitted = 0;

        for (int i = 0; i < tmdb_pending && emitted < max_items; i++) {
            MediaEntry *e = &tmdb_entries[i];
            char label[640];
            const char *show = e->show_name ? e->show_name : "";
            const char *title = e->title ? e->title : "";
            if (e->type != MEDIA_TYPE_MOVIE && *show && e->season > 0 && e->episode > 0) {
                snprintf(label, sizeof(label), "%s S%02dE%02d",
                         show, e->season, e->episode);
            } else if (e->type != MEDIA_TYPE_MOVIE && *show) {
                snprintf(label, sizeof(label), "%s — %s", show, title);
            } else {
                snprintf(label, sizeof(label), "%s", title);
            }
            char esc[1300];
            char *d = esc;
            for (const char *s = label; *s && d < esc + sizeof(esc) - 6; s++) {
                if (*s == '"') { *d++ = '\\'; *d++ = '"'; }
                else if (*s == '\\') { *d++ = '\\'; *d++ = '\\'; }
                else if ((unsigned char)*s >= 0x20) *d++ = *s;
            }
            *d = '\0';
            size_t need = strlen(esc) + 8;
            if (items_len + need > items_sz) {
                items_sz = (items_len + need) * 2;
                items = realloc(items, items_sz);
            }
            items_len += snprintf(items + items_len, items_sz - items_len,
                                  "%s{\"id\":%d,\"type\":%d,\"season\":%d,\"episode\":%d,\"label\":\"%s\"}",
                                  emitted ? "," : "", e->id, e->type,
                                  e->season, e->episode, esc);
            emitted++;
        }
        for (int i = 0; i < tmdb_pending; i++)
            database_free_entry(&tmdb_entries[i]);
        free(tmdb_entries);

        size_t buf_sz = 2048 + items_len;
        char *json = malloc(buf_sz);
        int len = snprintf(json, buf_sz,
            "{\"tmdb\":{\"active\":%s,\"operation\":\"%s\",\"current_item\":\"%s\","
            "\"total\":%d,\"processed\":%d,\"success\":%d,\"failed\":%d,"
            "\"percent\":%.1f,\"elapsed\":%d},"
            "\"media_count\":%d,\"tmdb_pending\":%d,"
            "\"pending_items\":[%s],\"pending_truncated\":%s}",
            t_active ? "true" : "false", t_operation, t_esc,
            t_total, t_processed, t_success, t_failed, t_percent, t_elapsed,
            media_count, tmdb_pending,
            items, tmdb_pending > max_items ? "true" : "false");
        send_response(fd, 200, "OK", "application/json", json, len);
        free(items);
        free(json);
    }
    else if (strcmp(path, "/api/counts") == 0) {
        char json[256];
        int len = snprintf(json, sizeof(json),
            "{\"total\":%d}", database_get_count());
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/errors") == 0) {
        char *json = errors_get_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Error log unavailable");
        }
    }
    else if (strcmp(path, "/api/errors/clear") == 0 && strcmp(method, "POST") == 0) {
        errors_clear();
        send_response(fd, 200, "OK", "application/json", "{\"ok\":true}", 11);
    }
    else if (strncmp(path, "/api/media/", 11) == 0 &&
             strstr(path, "/tmdb") && strcmp(method, "POST") == 0) {
        /* /api/media/<id>/tmdb — manual TMDB override.
         * Body: tmdb_id=N&type=M&season=S&episode=E (season/episode for episodes) */
        int media_id = atoi(path + 11);
        int tmdb_id = 0, mtype = 0, season = 0, episode = 0;
        if (body && body_len > 0) {
            char *b = strndup(body, body_len);
            for (char *tok = strtok(b, "&"); tok; tok = strtok(NULL, "&")) {
                char *eq = strchr(tok, '=');
                if (!eq) continue;
                *eq = '\0';
                int v = atoi(eq + 1);
                if (strcmp(tok, "tmdb_id") == 0) tmdb_id = v;
                else if (strcmp(tok, "type") == 0) mtype = v;
                else if (strcmp(tok, "season") == 0) season = v;
                else if (strcmp(tok, "episode") == 0) episode = v;
            }
            free(b);
        }
        if (media_id <= 0 || tmdb_id <= 0) {
            send_error(fd, 400, "Missing media_id or tmdb_id");
        } else {
            int ok = scanner_apply_manual_tmdb(media_id, mtype, tmdb_id, season, episode);
            char resp[64];
            int rlen = snprintf(resp, sizeof(resp), "{\"ok\":%s}",
                                ok ? "true" : "false");
            send_response(fd, ok ? 200 : 500, ok ? "OK" : "Failed",
                          "application/json", resp, rlen);
        }
    }
    else if (strcmp(path, "/api/streams/active") == 0) {
        /* Rich snapshot — IP, what they're watching, poster, since when. */
        char json[8192];
        size_t used = 0;
        used += snprintf(json + used, sizeof(json) - used, "{\"streams\":[");
        pthread_mutex_lock(&stream_lock);
        int first = 1;
        for (int i = 0; i < MAX_STREAM_IPS; i++) {
            if (stream_ips[i].refcount <= 0) continue;
            char ipbuf[INET_ADDRSTRLEN];
            struct in_addr a; a.s_addr = stream_ips[i].ip;
            inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf));

            /* JSON-escape helper, inline */
            #define ESC_FIELD(src, dst) do { \
                char *_d = (dst); \
                for (const char *_s = (src); *_s && _d < (dst) + sizeof(dst) - 4; _s++) { \
                    if (*_s == '"' || *_s == '\\') *_d++ = '\\'; \
                    if ((unsigned char)*_s >= 0x20) *_d++ = *_s; \
                } \
                *_d = '\0'; \
            } while (0)

            char title_esc[512], show_esc[512], poster_esc[512];
            ESC_FIELD(stream_ips[i].title, title_esc);
            ESC_FIELD(stream_ips[i].show_name, show_esc);
            ESC_FIELD(stream_ips[i].poster, poster_esc);
            #undef ESC_FIELD

            int elapsed = (int)(time(NULL) - stream_ips[i].first_seen);
            if (!first) json[used++] = ',';
            first = 0;
            used += snprintf(json + used, sizeof(json) - used,
                "{\"ip\":\"%s\",\"connections\":%d,"
                "\"file_id\":%d,\"type\":%d,"
                "\"title\":\"%s\",\"show\":\"%s\","
                "\"season\":%d,\"episode\":%d,"
                "\"poster\":\"%s\",\"size\":%lld,\"elapsed\":%d}",
                ipbuf, stream_ips[i].refcount,
                stream_ips[i].file_id, stream_ips[i].media_type,
                title_esc, show_esc,
                stream_ips[i].season, stream_ips[i].episode,
                poster_esc, (long long)stream_ips[i].file_size, elapsed);
        }
        pthread_mutex_unlock(&stream_lock);
        used += snprintf(json + used, sizeof(json) - used,
            "],\"max_ips\":%d}", MAX_STREAM_IPS);
        send_response(fd, 200, "OK", "application/json", json, used);
    }
    else if (strcmp(path, "/api/blocked") == 0) {
        char json[4096];
        size_t used = 0;
        used += snprintf(json + used, sizeof(json) - used, "{\"blocked\":[");
        pthread_mutex_lock(&stream_lock);
        for (int i = 0; i < blocked_count; i++) {
            char ipbuf[INET_ADDRSTRLEN];
            struct in_addr a; a.s_addr = blocked_ips[i];
            inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf));
            used += snprintf(json + used, sizeof(json) - used,
                "%s\"%s\"", i ? "," : "", ipbuf);
        }
        pthread_mutex_unlock(&stream_lock);
        used += snprintf(json + used, sizeof(json) - used, "]}");
        send_response(fd, 200, "OK", "application/json", json, used);
    }
    else if (strcmp(path, "/api/blocked/add") == 0 ||
             strcmp(path, "/api/blocked/remove") == 0) {
        if (strcmp(method, "POST") != 0) {
            send_error(fd, 405, "POST required");
            return;
        }
        if (!body || body_len == 0) {
            send_error(fd, 400, "Missing ip");
            return;
        }
        const char *ip_field = strstr(body, "ip=");
        if (!ip_field) { send_error(fd, 400, "Missing ip"); return; }
        ip_field += 3;
        char ip_str[64]; size_t n = 0;
        while (*ip_field && *ip_field != '&' && n + 1 < sizeof(ip_str))
            ip_str[n++] = *ip_field++;
        ip_str[n] = '\0';
        uint32_t ip;
        if (parse_ipv4(ip_str, &ip) != 0) {
            send_error(fd, 400, "Invalid ip"); return;
        }
        int is_add = (strstr(path, "add") != NULL);
        int rc = is_add ? block_ip(ip) : unblock_ip(ip);
        if (rc == 0) {
            /* Persist so blocks survive restart. */
            if (is_add) database_add_blocked_ip(ip_str);
            else        database_remove_blocked_ip(ip_str);
            send_response(fd, 200, "OK", "application/json",
                "{\"ok\":true}", 11);
        } else {
            send_error(fd, 400, "Operation failed");
        }
    }
    else if (strcmp(path, "/api/version") == 0) {
        /* Cheap polling endpoint — clients re-fetch full library only
         * when this value changes. Bumped on every DB write. */
        char json[128];
        int len = snprintf(json, sizeof(json),
            "{\"version\":%d,\"count\":%d}",
            database_get_version(), database_get_count());
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/debug/ls") == 0) {
        /* Diagnostic: recursively list files in media library paths */
        size_t buf_size = 65536;
        size_t buf_used = 0;
        char *json = malloc(buf_size);
        buf_used += snprintf(json + buf_used, buf_size - buf_used, "{\"paths\":[");

        for (int i = 0; i < server_config.media_path_count; i++) {
            /* Recursive listing using a simple stack */
            char dirs[64][MAX_PATH];
            int dir_count = 0;
            strncpy(dirs[0], server_config.media_paths[i], MAX_PATH - 1);
            dir_count = 1;
            int first_file = 1;

            while (dir_count > 0) {
                dir_count--;
                char *cur_dir = dirs[dir_count];
                DIR *d = opendir(cur_dir);
                if (!d) {
                    if (buf_used < buf_size - 256) {
                        if (!first_file) json[buf_used++] = ',';
                        first_file = 0;
                        buf_used += snprintf(json + buf_used, buf_size - buf_used,
                            "{\"error\":\"opendir failed\",\"path\":\"%s\",\"errno\":%d,\"strerror\":\"%s\"}",
                            cur_dir, errno, strerror(errno));
                    }
                    continue;
                }
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_name[0] == '.') continue;
                    char fullpath[MAX_PATH];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", cur_dir, ent->d_name);
                    struct stat st;
                    int stat_ok = (stat(fullpath, &st) == 0);
                    if (stat_ok && S_ISDIR(st.st_mode) && dir_count < 63) {
                        strncpy(dirs[dir_count], fullpath, MAX_PATH - 1);
                        dir_count++;
                    } else if (buf_used < buf_size - 512) {
                        if (!first_file) json[buf_used++] = ',';
                        first_file = 0;
                        int is_media = scanner_is_media_file(fullpath);
                        buf_used += snprintf(json + buf_used, buf_size - buf_used,
                            "{\"path\":\"%s\",\"is_dir\":%s,\"is_media\":%s,\"size\":%lld,\"stat_ok\":%s}",
                            fullpath,
                            (stat_ok && S_ISDIR(st.st_mode)) ? "true" : "false",
                            is_media ? "true" : "false",
                            stat_ok ? (long long)st.st_size : 0LL,
                            stat_ok ? "true" : "false");
                    }
                }
                closedir(d);
            }
        }

        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "],\"media_path_count\":%d,\"db_count\":%d}",
            server_config.media_path_count, database_get_count());
        send_response(fd, 200, "OK", "application/json", json, buf_used);
        free(json);
    }
    else if (strcmp(path, "/api/downloads/status") == 0) {
        char *json = downloads_status_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Status error");
        }
    }
    else if (strcmp(path, "/api/downloads/add") == 0) {
        if (strcmp(method, "POST") != 0) {
            send_error(fd, 405, "POST required");
            return;
        }
        if (!body || body_len == 0) {
            send_error(fd, 400, "Empty body");
            return;
        }
        /* Parse form-urlencoded: url0=...&type0=tv&url1=...&type1=movie */
        int added = 0;
        for (int i = 0; i < 10; i++) {
            char url_key[16], type_key[16];
            snprintf(url_key, sizeof(url_key), "url%d=", i);
            snprintf(type_key, sizeof(type_key), "type%d=", i);

            const char *up = strstr(body, url_key);
            if (!up) continue;
            up += strlen(url_key);
            const char *uend = strchr(up, '&');
            size_t ulen = uend ? (size_t)(uend - up) : strlen(up);
            if (ulen == 0 || ulen >= DOWNLOAD_URL_LEN) continue;

            char url_enc[DOWNLOAD_URL_LEN];
            memcpy(url_enc, up, ulen);
            url_enc[ulen] = '\0';

            char url_dec[DOWNLOAD_URL_LEN];
            url_decode(url_enc, url_dec, sizeof(url_dec));
            if (!url_dec[0]) continue;

            DownloadType t = DL_TYPE_MOVIE;
            const char *tp = strstr(body, type_key);
            if (tp) {
                tp += strlen(type_key);
                if (strncmp(tp, "tv", 2) == 0) t = DL_TYPE_TV;
            }

            if (downloads_add(url_dec, t) >= 0) added++;
        }
        char json[128];
        int len = snprintf(json, sizeof(json), "{\"added\":%d}", added);
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else {
        send_error(fd, 404, "API endpoint not found");
    }
}

/* Parse HTTP request and handle it */
static void handle_request(int fd, const char *request,
                           const char *body, size_t body_len) {
    char method[16], path[MAX_PATH], version[16];

    if (sscanf(request, "%15s %4095s %15s", method, path, version) != 3) {
        send_error(fd, 400, "Bad Request");
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        send_error(fd, 405, "Method Not Allowed");
        return;
    }
    /* Auth is enforced once in client_handler before stream-gate is taken,
     * so by the time we get here it's already been verified. */

    /* Extract Range header if present */
    char *range_header = NULL;
    char *range_start = strstr(request, "Range:");
    if (range_start) {
        range_start += 6;
        while (*range_start == ' ') range_start++;
        char *range_end = strstr(range_start, "\r\n");
        if (range_end) {
            size_t len = range_end - range_start;
            range_header = malloc(len + 1);
            strncpy(range_header, range_start, len);
            range_header[len] = '\0';
        }
    }

    /* Route request */
    if (strncmp(path, "/api/", 5) == 0) {
        handle_api(fd, method, path, body, body_len);
    }
    else if (strncmp(path, "/stream/", 8) == 0) {
        /* Stream media file by ID */
        int id = atoi(path + 8);
        char *filepath = database_get_filepath(id);
        if (filepath) {
            stream_file(fd, filepath, range_header);
            free(filepath);
        } else {
            send_error(fd, 404, "Media not found");
        }
    }
    else if (strncmp(path, "/image/", 7) == 0) {
        /* Serve cached image — URL-decode then reduce to basename.
         * Legacy DB rows store absolute paths in poster_path; new rows may
         * store just the filename. Either way, only the basename is used,
         * always resolved under cache_dir. Blocks path traversal. */
        char decoded[MAX_PATH];
        url_decode(path + 7, decoded, sizeof(decoded));
        const char *slash = strrchr(decoded, '/');
        const char *name = slash ? slash + 1 : decoded;
        if (*name == '\0' || strstr(name, "..")) {
            send_error(fd, 400, "Bad image name");
        } else {
            char filepath[MAX_PATH];
            snprintf(filepath, sizeof(filepath), "%s/%s",
                     server_config.cache_dir, name);
            serve_file(fd, filepath);
        }
    }
    else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        send_response(fd, 200, "OK", "text/html",
                      page_index_html, strlen(page_index_html));
    }
    else if (strcmp(path, "/status") == 0) {
        send_response(fd, 200, "OK", "text/html",
                      page_status_html, strlen(page_status_html));
    }
    else if (strcmp(path, "/wget") == 0) {
        send_response(fd, 200, "OK", "text/html",
                      page_wget_html, strlen(page_wget_html));
    }
    else if (strcmp(path, "/errors") == 0) {
        send_response(fd, 200, "OK", "text/html",
                      page_errors_html, strlen(page_errors_html));
    }
    else if (strcmp(path, "/pending") == 0) {
        send_response(fd, 200, "OK", "text/html",
                      page_pending_html, strlen(page_pending_html));
    }
    else {
        send_error(fd, 404, "Not Found");
    }

    if (range_header) free(range_header);
}

/* Optimize socket for high-throughput streaming */
static void optimize_socket(int fd) {
    /* TCP_NODELAY: disable Nagle's algorithm for lower latency */
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Large send buffer for streaming (4MB) */
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* TCP_CORK: batch small writes for efficiency (disabled for streaming) */
    int cork = 0;
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
}

/* Client handler thread */
static void *client_handler(void *arg) {
    ClientConnection *conn = (ClientConnection *)arg;
    char buffer[MAX_HEADER];
    int is_stream = 0;

    /* Set thread-local request context for API handlers */
    config_set_client_local(is_local_client(&conn->client_addr));
    config_set_client_ip(conn->client_addr.sin_addr.s_addr);

    /* Optimize socket for streaming before handling request */
    optimize_socket(conn->client_fd);

    /* Read headers until \r\n\r\n */
    ssize_t total = 0;
    char *header_end = NULL;
    while (total < (ssize_t)sizeof(buffer) - 1) {
        ssize_t n = recv(conn->client_fd, buffer + total, sizeof(buffer) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buffer[total] = '\0';
        header_end = strstr(buffer, "\r\n\r\n");
        if (header_end) break;
    }

    if (total > 0 && header_end) {
        size_t header_len = (header_end - buffer) + 4;

        /* Parse Content-Length, then read remaining body */
        char *body_buf = NULL;
        size_t body_len = 0;
        const char *cl = strcasestr(buffer, "Content-Length:");
        if (cl) {
            cl += 15;
            while (*cl == ' ') cl++;
            long n = strtol(cl, NULL, 10);
            if (n > 0 && n < 1024 * 1024) {
                body_len = (size_t)n;
                body_buf = malloc(body_len + 1);
                if (body_buf) {
                    size_t have = total - header_len;
                    if (have > body_len) have = body_len;
                    if (have > 0) memcpy(body_buf, buffer + header_len, have);
                    size_t got = have;
                    while (got < body_len) {
                        ssize_t r = recv(conn->client_fd, body_buf + got,
                                         body_len - got, 0);
                        if (r <= 0) break;
                        got += r;
                    }
                    body_len = got;
                    body_buf[body_len] = '\0';
                }
            }
        }

        if (!check_auth(buffer)) {
            send_401(conn->client_fd);
            if (body_buf) free(body_buf);
            close(conn->client_fd);
            free(conn);
            return NULL;
        }

        uint32_t client_ip = conn->client_addr.sin_addr.s_addr;
        char *stream_pos = strstr(buffer, "/stream/");
        if (stream_pos) {
            int rc = stream_ip_acquire(client_ip);
            if (rc == -2) {
                send_error(conn->client_fd, 403,
                    "Blocked — your IP has been temporarily restricted");
                if (body_buf) free(body_buf);
                close(conn->client_fd);
                free(conn);
                return NULL;
            }
            if (rc == -1) {
                send_error(conn->client_fd, 503,
                    "Server closed — max 3 concurrent IPs streaming");
                if (body_buf) free(body_buf);
                close(conn->client_fd);
                free(conn);
                return NULL;
            }
            is_stream = 1;
            /* Pull rich metadata for the admin UI (title/show/poster). */
            int sid = atoi(stream_pos + 8);
            if (sid > 0) stream_ip_set_meta(client_ip, sid);
        }

        handle_request(conn->client_fd, buffer, body_buf, body_len);
        if (body_buf) free(body_buf);

        if (is_stream) {
            stream_ip_release(client_ip);
        }
    }

    close(conn->client_fd);
    free(conn);
    return NULL;
}

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
    if (server_fd >= 0) {
        close(server_fd);
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c <config>  Config file path (default: ~/.config/nixly-server/config.conf)\n");
    fprintf(stderr, "  -p <port>    Override port (default: %d)\n", server_config.port);
    fprintf(stderr, "  -h           Show this help\n");
}

int main(int argc, char *argv[]) {
    const char *config_path = "~/.config/nixly-server/config.conf";
    int port_override = 0;
    int opt;

    while ((opt = getopt(argc, argv, "c:p:h")) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'p':
                port_override = atoi(optarg);
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    /* Load configuration */
    config_load(config_path);

    if (port_override > 0) {
        server_config.port = port_override;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Create cache directory */
    char cache_expanded[MAX_PATH];
    const char *home = getenv("HOME");
    if (server_config.cache_dir[0] == '~' && home) {
        snprintf(cache_expanded, sizeof(cache_expanded), "%s%s",
                 home, server_config.cache_dir + 1);
        strcpy(server_config.cache_dir, cache_expanded);
    }
    mkdir(server_config.cache_dir, 0755);

    /* Expand db_path and create parent directory */
    if (server_config.db_path[0] == '~' && home) {
        char db_expanded[MAX_PATH];
        snprintf(db_expanded, sizeof(db_expanded), "%s%s",
                 home, server_config.db_path + 1);
        strcpy(server_config.db_path, db_expanded);
    }
    {
        char db_dir[MAX_PATH];
        strncpy(db_dir, server_config.db_path, sizeof(db_dir) - 1);
        db_dir[sizeof(db_dir) - 1] = '\0';
        char *slash = strrchr(db_dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(db_dir, 0755);
        }
    }

    /* Initialize database */
    if (database_init(server_config.db_path) != 0) {
        fprintf(stderr, "Failed to initialize database\n");
        return 1;
    }

    /* Build the expected Authorization header once from config creds */
    build_expected_auth();

    /* Restore persisted block list from the DB. */
    {
        uint32_t *ips = NULL;
        int n = 0;
        if (database_load_blocked_ips(&ips, &n) == 0 && ips) {
            pthread_mutex_lock(&stream_lock);
            for (int i = 0; i < n && blocked_count < MAX_BLOCKED_IPS; i++) {
                blocked_ips[blocked_count++] = ips[i];
            }
            pthread_mutex_unlock(&stream_lock);
            printf("Loaded %d blocked IP(s) from DB\n", n);
            free(ips);
        }
    }

    /* Initialize downloads module */
    downloads_init();
    mkdir(server_config.tv_download_path, 0755);
    mkdir(server_config.movie_download_path, 0755);

    /* Clean up any .nixlypart leftovers from a previous crashed run so
     * they don't accumulate as orphan files. */
    {
        const char *dirs[2] = {
            server_config.tv_download_path,
            server_config.movie_download_path
        };
        for (int di = 0; di < 2; di++) {
            DIR *d = opendir(dirs[di]);
            if (!d) continue;
            struct dirent *e;
            int removed = 0;
            while ((e = readdir(d)) != NULL) {
                size_t nl = strlen(e->d_name);
                if (nl > 10 && strcmp(e->d_name + nl - 10, ".nixlypart") == 0) {
                    char full[MAX_PATH];
                    snprintf(full, sizeof(full), "%s/%s", dirs[di], e->d_name);
                    if (unlink(full) == 0) removed++;
                }
            }
            closedir(d);
            if (removed > 0)
                printf("Cleanup: removed %d stale .nixlypart in %s\n",
                       removed, dirs[di]);
        }
    }

    /* Ensure download destinations are also scanned + watched, so files
     * fetched via /wget land in the library and DB automatically. */
    {
        const char *dl_paths[2] = {
            server_config.tv_download_path,
            server_config.movie_download_path
        };
        for (int d = 0; d < 2; d++) {
            int present = 0;
            for (int i = 0; i < server_config.media_path_count; i++) {
                if (strcmp(server_config.media_paths[i], dl_paths[d]) == 0) {
                    present = 1;
                    break;
                }
            }
            if (!present && server_config.media_path_count < MAX_WATCH_PATHS) {
                strncpy(server_config.media_paths[server_config.media_path_count],
                        dl_paths[d], MAX_PATH_LEN - 1);
                server_config.media_paths[server_config.media_path_count][MAX_PATH_LEN - 1] = '\0';
                server_config.media_path_count++;
                printf("Auto-added download path to library: %s\n", dl_paths[d]);
            }
        }
    }

    /* Initialize TMDB */
    if (server_config.tmdb_api_key[0]) {
        if (tmdb_init(server_config.tmdb_api_key, server_config.tmdb_language) == 0) {
            printf("TMDB: Initialized with API key\n");
        }
    }

    /* Create server socket FIRST - accept connections while scanning in background */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(server_config.port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    /* SOMAXCONN = kernel max (typically 4096+), allows unlimited concurrent streams */
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        return 1;
    }

    /* Start discovery thread */
    pthread_t discovery_tid;
    if (pthread_create(&discovery_tid, NULL, discovery_thread, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to start discovery thread\n");
    } else {
        pthread_detach(discovery_tid);
    }

    /* Start periodic sync thread (backup to inotify) */
    pthread_t sync_tid;
    if (pthread_create(&sync_tid, NULL, sync_thread, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to start sync thread\n");
    } else {
        pthread_detach(sync_tid);
    }

    /* Start initial scan in background - server responds immediately with
     * whatever is already in the database while scan populates new entries */
    pthread_t startup_tid;
    if (pthread_create(&startup_tid, NULL, startup_scan_thread, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to start startup scan thread, running synchronously\n");
        startup_scan_thread(NULL);
    } else {
        pthread_detach(startup_tid);
    }

    int rating = config_get_server_rating();
    printf("\n");
    printf("========================================\n");
    printf("  Nixly Media Server running\n");
    printf("  http://0.0.0.0:%d\n", server_config.port);
    printf("  Discovery: UDP port %d\n", DISCOVERY_PORT);
    printf("========================================\n");
    printf("  Server: %s\n", server_config.server_name);
    printf("  ID: %s\n", server_config.server_id);
    printf("  Rating: %d/10 (%d Mbps)\n", rating, server_config.upload_mbps);
    printf("  Stream gate: max %d concurrent IPs (full speed each)\n", MAX_STREAM_IPS);
    printf("========================================\n");
    printf("  Scanning in background...\n");
    printf("========================================\n\n");

    /* Auto-open browser if running in a graphical session.
     * Set NIXLY_NO_BROWSER=1 to opt out (useful for headless service runs). */
    if (!getenv("NIXLY_NO_BROWSER") &&
        (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "xdg-open http://localhost:%d >/dev/null 2>&1 &",
                 server_config.port);
        if (system(cmd) == -1) {
            /* ignore — graphical session may lack xdg-open */
        }
    }

    /* Accept connections */
    while (running) {
        ClientConnection *conn = malloc(sizeof(ClientConnection));
        socklen_t addr_len = sizeof(conn->client_addr);

        conn->client_fd = accept(server_fd, (struct sockaddr *)&conn->client_addr, &addr_len);
        if (conn->client_fd < 0) {
            free(conn);
            if (running) perror("accept");
            continue;
        }

        pthread_t thread;
        if (pthread_create(&thread, NULL, client_handler, conn) != 0) {
            close(conn->client_fd);
            free(conn);
            continue;
        }
        pthread_detach(thread);
    }

    /* Cleanup */
    if (discovery_fd >= 0) {
        close(discovery_fd);
        discovery_fd = -1;
    }
    watcher_cleanup();
    tmdb_cleanup();
    downloads_cleanup();
    database_close();
    printf("\nServer stopped.\n");
    return 0;
}
