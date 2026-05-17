/*
 * Nixly Media Server - Runtime error log
 */

#include "errors.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ErrorEntry ring[ERROR_LOG_CAPACITY];
static int ring_head = 0;  /* next slot to write */
static int ring_count = 0;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;

void error_log(const char *source, const char *fmt, ...) {
    if (!source || !fmt) return;

    ErrorEntry e;
    e.ts = time(NULL);
    strncpy(e.source, source, sizeof(e.source) - 1);
    e.source[sizeof(e.source) - 1] = '\0';

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.message, sizeof(e.message), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&ring_lock);
    ring[ring_head] = e;
    ring_head = (ring_head + 1) % ERROR_LOG_CAPACITY;
    if (ring_count < ERROR_LOG_CAPACITY) ring_count++;
    pthread_mutex_unlock(&ring_lock);

    fprintf(stderr, "[%s] %s\n", source, e.message);
}

static void json_escape_into(const char *src, char *dst, size_t dst_sz) {
    char *d = dst;
    char *end = dst + dst_sz - 8;
    for (const char *s = src; *s && d < end; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') { *d++ = '\\'; *d++ = '"'; }
        else if (c == '\\') { *d++ = '\\'; *d++ = '\\'; }
        else if (c == '\n') { *d++ = '\\'; *d++ = 'n'; }
        else if (c == '\r') { *d++ = '\\'; *d++ = 'r'; }
        else if (c == '\t') { *d++ = '\\'; *d++ = 't'; }
        else if (c >= 0x20) { *d++ = (char)c; }
    }
    *d = '\0';
}

char *errors_get_json(void) {
    pthread_mutex_lock(&ring_lock);

    size_t cap = 4096 + ring_count * 600;
    char *buf = malloc(cap);
    if (!buf) { pthread_mutex_unlock(&ring_lock); return NULL; }
    size_t len = 0;
    len += snprintf(buf + len, cap - len, "{\"count\":%d,\"errors\":[", ring_count);

    int first = 1;
    /* Walk oldest-to-newest, emit newest-first by reversing iteration */
    for (int i = ring_count - 1; i >= 0; i--) {
        int idx = (ring_head - 1 - (ring_count - 1 - i) + ERROR_LOG_CAPACITY) % ERROR_LOG_CAPACITY;
        ErrorEntry *e = &ring[idx];

        char src_esc[64];
        char msg_esc[1200];
        json_escape_into(e->source, src_esc, sizeof(src_esc));
        json_escape_into(e->message, msg_esc, sizeof(msg_esc));

        size_t need = strlen(msg_esc) + strlen(src_esc) + 96;
        if (len + need > cap) {
            cap = (len + need) * 2;
            buf = realloc(buf, cap);
        }
        len += snprintf(buf + len, cap - len,
                        "%s{\"ts\":%lld,\"source\":\"%s\",\"message\":\"%s\"}",
                        first ? "" : ",", (long long)e->ts, src_esc, msg_esc);
        first = 0;
    }

    if (len + 4 > cap) buf = realloc(buf, len + 4);
    len += snprintf(buf + len, cap - len, "]}");

    pthread_mutex_unlock(&ring_lock);
    return buf;
}

void errors_clear(void) {
    pthread_mutex_lock(&ring_lock);
    ring_head = 0;
    ring_count = 0;
    pthread_mutex_unlock(&ring_lock);
}
