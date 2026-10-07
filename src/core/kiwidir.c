/* See kiwidir.h. */
#include "kiwidir.h"
#include "geo.h"

#include <stdlib.h>
#include <string.h>

static void copy_trunc(char *dst, size_t dstsz, const char *src)
{
    size_t i = 0;
    for (; src[i] && i < dstsz - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Parse "http://host:port" (or https, or bare host[:port]) into cur. */
static void parse_url(kiwi_server *s, const char *url)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)  p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;

    char host[96];
    size_t i = 0;
    for (; p[i] && p[i] != ':' && p[i] != '/' && i < sizeof(host) - 1; i++)
        host[i] = p[i];
    host[i] = '\0';
    copy_trunc(s->host, sizeof(s->host), host);

    s->port = 8073;   /* KiwiSDR default */
    if (p[i] == ':')
        s->port = atoi(p + i + 1);
}

static void begin_record(kiwidir_parser *p)
{
    memset(&p->cur, 0, sizeof(p->cur));
    p->cur.snr = -1;
    p->cur.online = 1;   /* cleared if status/offline say otherwise */
    p->cur_has_url = 0;
    p->have_key = 0;
    p->expect_value = 0;
    p->keylen = 0;
    p->vallen = 0;
}

static void assign_field(kiwidir_parser *p)
{
    p->key[p->keylen] = '\0';
    p->val[p->vallen] = '\0';
    const char *k = p->key, *v = p->val;

    if (strcmp(k, "url") == 0) {
        parse_url(&p->cur, v);
        p->cur_has_url = (p->cur.host[0] != '\0');
    } else if (strcmp(k, "name") == 0) {
        copy_trunc(p->cur.name, sizeof(p->cur.name), v);
    } else if (strcmp(k, "loc") == 0) {
        copy_trunc(p->cur.loc, sizeof(p->cur.loc), v);
    } else if (strcmp(k, "users") == 0) {
        p->cur.users = atoi(v);
    } else if (strcmp(k, "users_max") == 0) {
        p->cur.users_max = atoi(v);
    } else if (strcmp(k, "snr") == 0) {
        p->cur.snr = (v[0] >= '0' && v[0] <= '9') ? atoi(v) : -1;
    } else if (strcmp(k, "gps") == 0) {
        double la, lo;
        if (geo_parse_gps(v, &la, &lo)) {
            p->cur.lat = (float)la;
            p->cur.lon = (float)lo;
            p->cur.has_gps = 1;
        }
    } else if (strcmp(k, "status") == 0) {
        if (strcmp(v, "active") != 0) p->cur.online = 0;
    } else if (strcmp(k, "offline") == 0) {
        if (strcmp(v, "no") != 0) p->cur.online = 0;
    }
}

static void end_record(kiwidir_parser *p)
{
    if (p->cur_has_url && p->count < p->cap)
        p->arr[p->count++] = p->cur;
}

void kiwidir_init(kiwidir_parser *p, kiwi_server *arr, int cap)
{
    memset(p, 0, sizeof(*p));
    p->arr = arr;
    p->cap = cap;
}

void kiwidir_feed(kiwidir_parser *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        if (p->in_comment) {
            if (c == '\n') p->in_comment = 0;
            continue;
        }

        if (p->in_str) {
            if (p->escape) {           /* previous char was backslash */
                p->escape = 0;
            } else if (c == '\\') {
                p->escape = 1;
                continue;
            } else if (c == '"') {     /* end of string token */
                p->in_str = 0;
                if (p->expect_value) {
                    assign_field(p);
                    p->have_key = 0;
                    p->expect_value = 0;
                } else {
                    p->have_key = 1;   /* this token was a key */
                }
                continue;
            }
            /* accumulate into key or value buffer */
            if (p->expect_value) {
                if (p->vallen < (int)sizeof(p->val) - 1)
                    p->val[p->vallen++] = c;
            } else {
                if (p->keylen < (int)sizeof(p->key) - 1)
                    p->key[p->keylen++] = c;
            }
            continue;
        }

        /* not in a string, not in a comment */
        switch (c) {
        case '/':
            /* second '/' starts a // line comment (works across chunks) */
            if (p->prev_slash) { p->in_comment = 1; p->prev_slash = 0; }
            else               { p->prev_slash = 1; }
            continue;
        case '"':
            p->in_str = 1;
            if (p->expect_value) p->vallen = 0;
            else                 p->keylen = 0;
            break;
        case '{':
            p->depth++;
            if (p->depth == 1) { p->in_obj = 1; begin_record(p); }
            break;
        case '}':
            if (p->depth == 1) { end_record(p); p->in_obj = 0; }
            if (p->depth > 0) p->depth--;
            break;
        case ':':
            if (p->have_key) p->expect_value = 1;
            break;
        case ',':
            p->have_key = 0;
            p->expect_value = 0;
            break;
        default:
            break;
        }
        p->prev_slash = 0;
    }
}

int kiwidir_count(const kiwidir_parser *p)
{
    return p->count;
}
