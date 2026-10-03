/* Streaming parser for the KiwiSDR public directory.
 *
 * Source: http://rx.linkfanel.net/kiwisdr_com.js  (plain HTTP, ~1 MB)
 * Format: `var kiwisdr_com = [ {..}, {..}, .. ];` — a JSON array of objects with
 * all values quoted. We only need a few fields per receiver, so rather than
 * buffer and fully parse the megabyte, we feed HTTP body chunks through a small
 * character state machine that extracts one record per `{...}` object.
 *
 * Portable core; unit-tested against a sample on the host.
 */
#ifndef VITASDR_KIWIDIR_H
#define VITASDR_KIWIDIR_H

#include <stddef.h>

typedef struct {
    char host[96];
    int  port;
    char name[64];
    char loc[48];
    int  users;
    int  users_max;
    int  snr;        /* first value of the "a,b" snr pair; -1 if unknown */
    int  online;     /* status=="active" && offline=="no" */
    /* Which client speaks to this receiver: 0 = KiwiSDR, 1 = OpenWebRX. Public
     * directory entries are always KiwiSDR; favourites/manual adds may differ.
     * `path` is the WebSocket path for OpenWebRX (usually "/ws/"); unused for
     * KiwiSDR, which builds its own SND/WF paths. */
    int  proto;
    char path[64];
} kiwi_server;

typedef struct {
    kiwi_server *arr;
    int          cap;
    int          count;

    /* ---- state machine ---- */
    int   depth;         /* brace nesting */
    int   in_obj;        /* currently inside a receiver object */
    int   in_str;        /* inside a "..." token */
    int   escape;        /* previous char was backslash inside a string */
    int   in_comment;    /* inside a // line comment */
    int   prev_slash;    /* previous non-string char was '/' (comment detect) */
    int   have_key;      /* a key token is complete, awaiting ':' then value */
    int   expect_value;  /* saw ':', next string is the value */
    char  key[24];
    int   keylen;
    char  val[160];
    int   vallen;
    kiwi_server cur;     /* record being assembled */
    int   cur_has_url;
} kiwidir_parser;

/* `arr`/`cap` is the caller's output array. */
void kiwidir_init(kiwidir_parser *p, kiwi_server *arr, int cap);

/* Feed a chunk of the HTTP body. Safe across arbitrary chunk boundaries. */
void kiwidir_feed(kiwidir_parser *p, const char *data, size_t len);

/* Returns the number of receivers parsed so far. */
int  kiwidir_count(const kiwidir_parser *p);

#endif /* VITASDR_KIWIDIR_H */
