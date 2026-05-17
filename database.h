/*
 * Nixly Media Server - Database Module
 * SQLite-based media library database with TMDB metadata
 */

#ifndef DATABASE_H
#define DATABASE_H

#include <stdint.h>

typedef enum {
    MEDIA_TYPE_MOVIE = 0,
    MEDIA_TYPE_TVSHOW = 1,
    MEDIA_TYPE_EPISODE = 2
} MediaType;

typedef struct {
    int id;
    MediaType type;
    char *title;
    char *filepath;
    char *show_name;      /* For episodes: parent show name */
    int season;           /* For episodes */
    int episode;          /* For episodes */
    int64_t size;         /* File size in bytes */
    int duration;         /* Duration in seconds */
    int width;            /* Video width */
    int height;           /* Video height */
    char *codec_video;
    char *codec_audio;
    int64_t bitrate;
    char *added_date;

    /* Per-file tech properties extracted by scanner */
    char *hdr_format;     /* HDR10, HDR10+, HLG, DV, DV+HDR10 */
    int bit_depth;
    int audio_channels;
    char *audio_layout;   /* "2.0", "5.1", "7.1" */
    char *audio_features; /* "atmos", "joc", "dts-x" comma-sep */

    /* TMDB metadata */
    int tmdb_id;          /* TMDB movie/show ID */
    char *tmdb_title;     /* Title from TMDB */
    char *overview;       /* Plot summary */
    char *poster_path;    /* Local path to poster */
    char *backdrop_path;  /* Local path to backdrop */
    char *release_date;   /* YYYY-MM-DD */
    int year;
    float rating;         /* TMDB vote average 0-10 */
    int vote_count;
    char *genres;         /* Comma-separated */

    /* For episodes */
    int tmdb_show_id;     /* Parent show's TMDB ID */
    char *episode_title;  /* Episode name from TMDB */
    char *episode_overview;
    char *still_path;     /* Episode screenshot */

    /* TMDB show totals (stored per-episode, aggregated for show view) */
    int tmdb_total_seasons;    /* Total seasons from TMDB */
    int tmdb_total_episodes;   /* Total episodes from TMDB */
    int tmdb_episode_runtime;  /* Typical episode runtime in minutes from TMDB */
    char *tmdb_status;         /* "Returning Series", "Ended", "Canceled", etc. */
    char *tmdb_next_episode;   /* YYYY-MM-DD of next episode air date */
} MediaEntry;

/* Initialize database, create tables if needed */
int database_init(const char *db_path);

/* Close database connection */
void database_close(void);

/* Add or update media entry */
int database_add_media(MediaEntry *entry);

/* Update TMDB metadata for existing entry */
int database_update_tmdb(int id, MediaEntry *tmdb_data);

/* Check if file already exists in database */
int database_file_exists(const char *filepath);

/* Get media ID by filepath */
int database_get_id_by_path(const char *filepath);

/* Delete media entry by filepath */
int database_delete_by_path(const char *filepath);

/* Remove entries for files that no longer exist on disk */
int database_cleanup_missing(void);

/* Get total media count */
int database_get_count(void);

/* Monotonic version counter, bumped on every write. Clients poll this
 * cheaply and only refetch the full library when the value changes. */
int database_get_version(void);

/* Get filepath by media ID */
char *database_get_filepath(int id);

/* Lightweight metadata lookup for the live-stream admin view.
 * All out buffers may be empty strings if no data is available.
 * Returns 0 on success, -1 if id not found. */
int database_get_stream_meta(int id,
    char *title, size_t title_sz,
    char *show_name, size_t show_sz,
    char *poster, size_t poster_sz,
    int *season, int *episode, int *type, int64_t *size);

/* Get entries missing TMDB data */
int database_get_entries_without_tmdb(MediaEntry **entries, int *count);

/* Get all entries for full TMDB rescan */
int database_get_all_entries(MediaEntry **entries, int *count);

/* Get all entries of a specific media type. */
int database_get_entries_by_type(int media_type, MediaEntry **entries, int *count);

/* Get a single entry by id (caller frees with database_free_entry). */
int database_get_entry(int id, MediaEntry *out);

/* JSON exports for API */
char *database_get_library_json(void);
char *database_get_movies_json(void);
char *database_get_tvshows_json(void);
char *database_get_media_json(int id);
char *database_get_show_seasons_json(const char *show_name);
char *database_get_show_episodes_json(const char *show_name, int season);

/* Delete all media entries from database (full wipe for fresh start) */
int database_delete_all_media(void);

/* Update show status and totals for all episodes of a show */
int database_update_show_status(int tmdb_show_id, const char *status, const char *next_episode,
                                int total_seasons, int total_episodes, int episode_runtime);

/* Get unique TMDB show IDs for active (non-ended) shows */
int database_get_active_show_ids(int **ids, int *count);

/* Free entries */
void database_free_entry(MediaEntry *entry);

/* Blocked-IP persistence (survives restart).
 * IPs stored as dotted-quad strings; load returns network-byte-order ints
 * matching sockaddr_in.sin_addr.s_addr. */
int database_load_blocked_ips(uint32_t **ips, int *count);
int database_add_blocked_ip(const char *ip_str);
int database_remove_blocked_ip(const char *ip_str);

/* Watch-progress tracking. max_offset is the highest byte the client has
 * requested from /stream/<id> (proxy for furthest position watched). */
int database_watch_progress_update(const char *ip, int media_id,
                                   int64_t max_offset, int64_t file_size);

/* JSON map of {media_id: percent} for the given IP. Caller frees. */
char *database_watch_progress_json_for_ip(const char *ip);

/* Cast member (actor) for a movie or TV show. */
typedef struct {
    int tmdb_person_id;
    char *name;
    char *character;
    char *profile_path;   /* Local cached path or empty */
} CastEntry;

/* Replace all cast for a tmdb_id (movie tmdb_id or show tmdb_show_id). */
int database_replace_cast(int tmdb_id, CastEntry *entries, int count);

/* Returns 1 if any cast row exists for tmdb_id, else 0. */
int database_has_cast(int tmdb_id);

/* Get cast JSON array for a media row id. Caller frees. */
char *database_get_cast_json(int media_id);

/* --- Collections --- */
int database_collection_create(const char *name);  /* returns new id, -1 on error */
int database_collection_delete(int collection_id);
int database_collection_add_item(int collection_id, int media_id);
int database_collection_remove_item(int collection_id, int media_id);
char *database_collections_list_json(void);              /* [{id,name,count,poster}] */
char *database_collection_detail_json(int collection_id); /* {id,name,items:[...]} */

#endif /* DATABASE_H */
