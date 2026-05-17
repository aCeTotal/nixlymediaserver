/*
 * Nixly Media Server - Runtime error log
 * Thread-safe ring buffer of recent errors, surfaced on /errors page.
 */

#ifndef ERRORS_H
#define ERRORS_H

#include <time.h>

#define ERROR_LOG_CAPACITY 256

typedef struct {
    time_t ts;
    char source[32];
    char message[480];
} ErrorEntry;

/* Append an error. Source is a short tag ("tmdb", "scanner", "db", "download"). */
void error_log(const char *source, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* JSON dump of all entries, newest first. Caller frees. */
char *errors_get_json(void);

/* Drop all entries. */
void errors_clear(void);

#endif
