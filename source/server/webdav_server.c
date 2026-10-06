/*
 * WebDAV server implemented in C on top of libmicrohttpd.
 *
 * This is a C port of the original cpp-httplib based webdav_server.cpp. It
 * maps the WebDAV URL space directly onto the host filesystem (path "/" is the
 * real root) and implements the methods needed by rclone / Windows Explorer /
 * generic WebDAV clients:
 *
 *   GET/HEAD   - directory listing (HTML) and file download with Range support
 *   PUT        - streaming upload written straight to the target, with
 *                large-file preallocation; discarded if the upload aborts
 *   DELETE     - recursive delete with 207 multistatus error reporting
 *   MKCOL      - create collection (directory)
 *   COPY/MOVE  - with Destination/Overwrite/Depth handling
 *   PROPFIND   - allprop / propname / prop, Depth 0/1/infinity
 *   PROPPATCH  - accepted (no-op) with success multistatus
 *   LOCK/UNLOCK- in-memory exclusive/shared lock bookkeeping
 *   OPTIONS    - advertises DAV class 1,2 and NextCloud-ish headers
 *
 * XML is produced and parsed by hand (small, fixed "D:" prefixed vocabulary),
 * so no C++ XML dependency is required.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <microhttpd.h>

#include "server/webdav_server.h"
#ifdef DEBUG
#include "dbglogger.h"
#endif
#include "util.h"

/* Request logging toggle built on the shared DEBUG switch / DBG_LOG (see
 * util.h). When DEBUG is on LOG_REQUEST() calls log_request(); otherwise it is
 * a no-op that still casts its arguments to void, so log_request() is never
 * called yet the args (e.g. logged_status) aren't flagged as unused. */
#ifdef DEBUG
#define LOG_REQUEST(conn, method, url, status) log_request((conn), (method), (url), (status))
#else
#define LOG_REQUEST(conn, method, url, status) \
    ((void)(conn), (void)(method), (void)(url), (void)(status))
#endif

#ifndef APP_VERSION
#define APP_VERSION 1.00
#endif

#ifndef LOCK_TIMEOUT_SECONDS
#define LOCK_TIMEOUT_SECONDS 3600
#endif

/* Size of each disk-space preallocation step during PUT uploads. 0 disables. */
#ifndef PUT_PREALLOC_CHUNK_BYTES
#define PUT_PREALLOC_CHUNK_BYTES (100ULL * 1024 * 1024) /* 100 MB */
#endif

/* Preallocation only engages once an upload grows past this many bytes. */
#ifndef PUT_PREALLOC_MIN_BYTES
#define PUT_PREALLOC_MIN_BYTES (50ULL * 1024 * 1024) /* 50 MB */
#endif

#define DOWNLOAD_SEGMENTS 4
#define PUT_FLUSH_THRESHOLD (1024 * 1024) /* 1 MB */

static int http_server_port = 8880;
static volatile int stop_server = 0;
static volatile int in_rest_mode = 0;
static struct MHD_Daemon *g_daemon = NULL;

/* ------------------------------------------------------------------ */
/* Dynamic string buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

static void sb_init(strbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sb_reserve(strbuf_t *sb, size_t extra)
{
    if (sb->len + extra + 1 <= sb->cap)
        return 1;
    size_t ncap = sb->cap ? sb->cap * 2 : 256;
    while (ncap < sb->len + extra + 1)
        ncap *= 2;
    char *nd = (char *)realloc(sb->data, ncap);
    if (!nd)
        return 0;
    sb->data = nd;
    sb->cap = ncap;
    return 1;
}

static int sb_append_len(strbuf_t *sb, const char *s, size_t n)
{
    if (!sb_reserve(sb, n))
        return 0;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 1;
}

static int sb_append(strbuf_t *sb, const char *s)
{
    return sb_append_len(sb, s, strlen(s));
}

static int sb_appendf(strbuf_t *sb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char stackbuf[1024];
    int n = vsnprintf(stackbuf, sizeof stackbuf, fmt, args);
    va_end(args);
    if (n < 0)
        return 0;
    if ((size_t)n < sizeof stackbuf)
        return sb_append_len(sb, stackbuf, (size_t)n);

    /* Rare large case: allocate exactly. */
    char *big = (char *)malloc((size_t)n + 1);
    if (!big)
        return 0;
    va_start(args, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, args);
    va_end(args);
    int ok = sb_append_len(sb, big, (size_t)n);
    free(big);
    return ok;
}

static void sb_free(strbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Small string helpers                                              */
/* ------------------------------------------------------------------ */

/* Case-insensitive substring search. Portable replacement for strcasestr,
 * which the PS4 SDK declares but does not provide in its stub libraries. */
static const char *str_casestr(const char *haystack, const char *needle)
{
    if (!*needle)
        return haystack;
    for (; *haystack; ++haystack)
    {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n))
        {
            ++h;
            ++n;
        }
        if (!*n)
            return haystack;
    }
    return NULL;
}

/* XML-escape text content into the buffer. */
static void sb_append_xml_escaped(strbuf_t *sb, const char *s)
{
    for (; *s; ++s)
    {
        switch (*s)
        {
        case '&':  sb_append(sb, "&amp;"); break;
        case '<':  sb_append(sb, "&lt;"); break;
        case '>':  sb_append(sb, "&gt;"); break;
        case '"':  sb_append(sb, "&quot;"); break;
        case '\'': sb_append(sb, "&apos;"); break;
        default:   sb_append_len(sb, s, 1); break;
        }
    }
}

/* Percent-encode a path for use in a D:href. Keeps unreserved chars and '/'. */
static void sb_append_url_encoded(strbuf_t *sb, const char *s)
{
    static const char hexd[] = "0123456789ABCDEF";
    for (; *s; ++s)
    {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
        {
            sb_append_len(sb, (const char *)&c, 1);
        }
        else
        {
            char enc[3];
            enc[0] = '%';
            enc[1] = hexd[(c >> 4) & 0xF];
            enc[2] = hexd[c & 0xF];
            sb_append_len(sb, enc, 3);
        }
    }
}

/* URL-decode in place semantics into a freshly malloc'd string. */
static char *url_decode(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
        {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        else if (s[i] == '+')
        {
            out[j++] = ' ';
        }
        else
        {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* Date / etag / mime helpers                                        */
/* ------------------------------------------------------------------ */

static void format_http_date(time_t tt, char *out, size_t outlen)
{
    struct tm gmt;
    gmtime_r(&tt, &gmt);
    strftime(out, outlen, "%a, %d %b %Y %H:%M:%S GMT", &gmt);
}

static void format_iso8601_date(time_t tt, char *out, size_t outlen)
{
    struct tm gmt;
    gmtime_r(&tt, &gmt);
    strftime(out, outlen, "%Y-%m-%dT%H:%M:%SZ", &gmt);
}

static void compute_etag(const char *local_path, char *out, size_t outlen)
{
    struct stat st;
    if (stat(local_path, &st) != 0)
    {
        snprintf(out, outlen, "W/\"0-0\"");
        return;
    }
    snprintf(out, outlen, "W/\"%llx-%llx-%llx-%llx-%lx\"",
             (unsigned long long)st.st_dev,
             (unsigned long long)st.st_ino,
             (unsigned long long)st.st_size,
             (unsigned long long)st.st_mtim.tv_sec,
             (unsigned long)st.st_mtim.tv_nsec);
}

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash))
        return "";
    return dot;
}

static const char *guess_content_type(const char *path)
{
    const char *e = ext_of(path);
    if (strcasecmp(e, ".txt") == 0) return "text/plain";
    if (strcasecmp(e, ".html") == 0 || strcasecmp(e, ".htm") == 0) return "text/html";
    if (strcasecmp(e, ".css") == 0) return "text/css";
    if (strcasecmp(e, ".js") == 0) return "application/javascript";
    if (strcasecmp(e, ".png") == 0) return "image/png";
    if (strcasecmp(e, ".jpg") == 0 || strcasecmp(e, ".jpeg") == 0) return "image/jpeg";
    return "application/octet-stream";
}

static mode_t put_create_mode(const char *path)
{
    const char *e = ext_of(path);
    if (strcasecmp(e, ".bin") == 0 || strcasecmp(e, ".elf") == 0 ||
        strcasecmp(e, ".prx") == 0 || strcasecmp(e, ".sprx") == 0 ||
        strcasecmp(e, ".self") == 0)
    {
        return 0777;
    }
    return 0666;
}

/* ------------------------------------------------------------------ */
/* UUID                                                              */
/* ------------------------------------------------------------------ */

static void generate_uuid(char *out /* at least 37 bytes */)
{
    static const char hexd[] = "0123456789abcdef";
    int j = 0;
    for (int i = 0; i < 32; ++i)
    {
        if (i == 8 || i == 12 || i == 16 || i == 20)
            out[j++] = '-';
        out[j++] = hexd[rand() & 0xF];
    }
    out[j] = '\0';
}

/* ------------------------------------------------------------------ */
/* In-memory lock table                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    char path[1024];
    char token[160];   /* opaquelocktoken:<uuid> */
    char scope[16];    /* "exclusive" or "shared" */
    char owner[512];
    int depth;
    time_t expiry;     /* monotonic-ish wall-clock expiry */
    int in_use;
} webdav_lock_t;

#define MAX_LOCKS 256
static webdav_lock_t g_locks[MAX_LOCKS];
static pthread_mutex_t g_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

static void purge_expired_locks_locked(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_LOCKS; ++i)
    {
        if (g_locks[i].in_use && g_locks[i].expiry <= now)
            g_locks[i].in_use = 0;
    }
}

static webdav_lock_t *find_lock_by_path_locked(const char *path)
{
    for (int i = 0; i < MAX_LOCKS; ++i)
        if (g_locks[i].in_use && strcmp(g_locks[i].path, path) == 0)
            return &g_locks[i];
    return NULL;
}

static webdav_lock_t *find_lock_by_token_locked(const char *token)
{
    for (int i = 0; i < MAX_LOCKS; ++i)
        if (g_locks[i].in_use && strcmp(g_locks[i].token, token) == 0)
            return &g_locks[i];
    return NULL;
}

static webdav_lock_t *alloc_lock_locked(void)
{
    for (int i = 0; i < MAX_LOCKS; ++i)
        if (!g_locks[i].in_use)
            return &g_locks[i];
    return NULL;
}

static long lock_seconds_remaining(const webdav_lock_t *lk)
{
    time_t now = time(NULL);
    if (lk->expiry <= now)
        return 0;
    return (long)(lk->expiry - now);
}

/* ------------------------------------------------------------------ */
/* Filesystem helpers                                                */
/* ------------------------------------------------------------------ */

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int path_is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_parent(const char *p, char *out, size_t outlen)
{
    const char *slash = strrchr(p, '/');
    if (!slash)
    {
        snprintf(out, outlen, ".");
        return 1;
    }
    if (slash == p)
    {
        snprintf(out, outlen, "/");
        return 1;
    }
    size_t n = (size_t)(slash - p);
    if (n >= outlen)
        n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static const char *path_filename(const char *p)
{
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

/* Strip trailing '/' from a filesystem path, in place. WebDAV clients (notably
 * the Windows Mini-Redirector) append a trailing slash to collection paths in
 * both the request-URI and the Destination header, e.g. "/parent/New Folder/".
 * A trailing slash breaks path_parent() (it would return the directory itself
 * instead of its parent) and can make rename() fail, so normalize it away.
 * The root "/" is preserved. */
static void strip_trailing_slashes(char *p)
{
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/')
        p[--len] = '\0';
}

/* Recursively delete. Returns 0 on success; on failure fills fail_path and
 * sets *fail_errno, returning -1. */
static int delete_recursive_strict(const char *current, char *fail_path, size_t fp_len, int *fail_errno)
{
    struct stat st;
    if (lstat(current, &st) != 0)
    {
        snprintf(fail_path, fp_len, "%s", current);
        *fail_errno = errno;
        return -1;
    }

    if (S_ISDIR(st.st_mode))
    {
        DIR *d = opendir(current);
        if (d)
        {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL)
            {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                    continue;
                char child[2048];
                snprintf(child, sizeof child, "%s/%s", current, ent->d_name);
                if (delete_recursive_strict(child, fail_path, fp_len, fail_errno) != 0)
                {
                    closedir(d);
                    return -1;
                }
            }
            closedir(d);
        }
    }

    if (remove(current) != 0)
    {
        snprintf(fail_path, fp_len, "%s", current);
        *fail_errno = errno;
        return -1;
    }
    return 0;
}

/* Best-effort recursive removal (ignores errors), used before overwrite. */
static void safe_remove_all(const char *target)
{
    struct stat st;
    if (lstat(target, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode))
    {
        DIR *d = opendir(target);
        if (d)
        {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL)
            {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                    continue;
                char child[2048];
                snprintf(child, sizeof child, "%s/%s", target, ent->d_name);
                safe_remove_all(child);
            }
            closedir(d);
        }
    }
    remove(target);
}

/* Copy a single regular file preserving mode. Returns 0 on success. */
static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    struct stat st;
    if (fstat(in, &st) != 0)
    {
        close(in);
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0)
    {
        close(in);
        return -1;
    }
    char buf[0x100000];
    ssize_t r;
    int rc = 0;
    while ((r = read(in, buf, sizeof buf)) > 0)
    {
        ssize_t off = 0;
        while (off < r)
        {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0)
            {
                if (errno == EINTR)
                    continue;
                rc = -1;
                goto done;
            }
            off += w;
        }
    }
    if (r < 0)
        rc = -1;
done:
    close(in);
    if (close(out) != 0)
        rc = -1;
    return rc;
}

/* Recursively copy src to dst (dst must not exist for directories).
 * recursive==0 means for directories only create the dir (Depth: 0). */
static int copy_recursive(const char *src, const char *dst, int recursive)
{
    struct stat st;
    if (lstat(src, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode))
    {
        if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST)
            return -1;
        if (!recursive)
            return 0;
        DIR *d = opendir(src);
        if (!d)
            return -1;
        struct dirent *ent;
        int rc = 0;
        while ((ent = readdir(d)) != NULL)
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char s2[2048], d2[2048];
            snprintf(s2, sizeof s2, "%s/%s", src, ent->d_name);
            snprintf(d2, sizeof d2, "%s/%s", dst, ent->d_name);
            if (copy_recursive(s2, d2, 1) != 0)
            {
                rc = -1;
                break;
            }
        }
        closedir(d);
        return rc;
    }
    else
    {
        return copy_file(src, dst);
    }
}

/* Parse a Destination header into a decoded local path (strips scheme/host). */
static char *parse_destination_path(const char *dest_header)
{
    const char *p = strstr(dest_header, "://");
    const char *path_start = dest_header;
    if (p)
    {
        const char *slash = strchr(p + 3, '/');
        path_start = slash ? slash : "/";
    }
    char *decoded = url_decode(path_start);
    if (decoded)
        strip_trailing_slashes(decoded);
    return decoded;
}


/* ------------------------------------------------------------------ */
/* Minimal XML body parsing (hand-rolled, tolerant)                  */
/* ------------------------------------------------------------------ */

/* Does the XML body (anywhere) contain an element whose local name matches? */
static int xml_has_local_element(const char *xml, const char *local)
{
    const char *p = xml;
    size_t ln = strlen(local);
    while ((p = strchr(p, '<')) != NULL)
    {
        p++;
        if (*p == '/' || *p == '?' || *p == '!')
            continue;
        /* Skip optional namespace prefix. */
        const char *name = p;
        const char *colon = NULL;
        const char *q = p;
        while (*q && *q != '>' && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n' && *q != '/')
        {
            if (*q == ':')
                colon = q;
            q++;
        }
        if (colon)
            name = colon + 1;
        size_t namelen = (size_t)(q - name);
        if (namelen == ln && strncmp(name, local, ln) == 0)
            return 1;
    }
    return 0;
}

typedef enum
{
    PROPFIND_ALLPROP,
    PROPFIND_PROPNAME,
    PROPFIND_PROP
} propfind_mode_t;

#define MAX_REQ_PROPS 64

typedef struct
{
    propfind_mode_t mode;
    char props[MAX_REQ_PROPS][64]; /* local names only */
    int prop_count;
} propfind_request_t;

/* Collect the local names of child elements of the first <...prop> element. */
static void collect_prop_children(const char *xml, propfind_request_t *req)
{
    /* Find "<...prop>" (an element whose local name is exactly "prop"). */
    const char *p = xml;
    const char *prop_open_end = NULL;
    while ((p = strchr(p, '<')) != NULL)
    {
        const char *q = p + 1;
        if (*q == '/' || *q == '?' || *q == '!')
        {
            p = q;
            continue;
        }
        const char *name = q;
        const char *colon = NULL;
        const char *r = q;
        while (*r && *r != '>' && *r != ' ' && *r != '/' && *r != '\t' && *r != '\r' && *r != '\n')
        {
            if (*r == ':')
                colon = r;
            r++;
        }
        if (colon)
            name = colon + 1;
        size_t namelen = (size_t)(r - name);
        if (namelen == 4 && strncmp(name, "prop", 4) == 0)
        {
            const char *gt = strchr(r, '>');
            if (gt)
            {
                prop_open_end = gt + 1;
                break;
            }
        }
        p = q;
    }
    if (!prop_open_end)
        return;

    /* Walk children until the closing </...prop>. */
    p = prop_open_end;
    while ((p = strchr(p, '<')) != NULL && req->prop_count < MAX_REQ_PROPS)
    {
        const char *q = p + 1;
        if (*q == '/')
        {
            /* Likely </prop> or child close tag; if it's the prop close, stop. */
            const char *name = q + 1;
            const char *colon = strchr(name, ':');
            const char *base = name;
            if (colon)
            {
                const char *gt = strchr(q, '>');
                if (!gt || colon < gt)
                    base = colon + 1;
            }
            if (strncmp(base, "prop>", 5) == 0 || strncmp(base, "prop ", 5) == 0)
                break;
            p = q;
            continue;
        }
        if (*q == '?' || *q == '!')
        {
            p = q;
            continue;
        }
        const char *name = q;
        const char *colon = NULL;
        const char *r = q;
        while (*r && *r != '>' && *r != ' ' && *r != '/' && *r != '\t' && *r != '\r' && *r != '\n')
        {
            if (*r == ':')
                colon = r;
            r++;
        }
        if (colon)
            name = colon + 1;
        size_t namelen = (size_t)(r - name);
        if (namelen > 0 && namelen < 64)
        {
            memcpy(req->props[req->prop_count], name, namelen);
            req->props[req->prop_count][namelen] = '\0';
            req->prop_count++;
        }
        p = r;
    }
}

static void parse_propfind(const char *xml, propfind_request_t *req)
{
    req->mode = PROPFIND_ALLPROP;
    req->prop_count = 0;

    if (!xml || !*xml)
        return;

    if (xml_has_local_element(xml, "propname"))
    {
        req->mode = PROPFIND_PROPNAME;
        return;
    }
    if (xml_has_local_element(xml, "allprop"))
    {
        req->mode = PROPFIND_ALLPROP;
        return;
    }
    if (xml_has_local_element(xml, "prop"))
    {
        req->mode = PROPFIND_PROP;
        collect_prop_children(xml, req);
        return;
    }
}

/* Parse <lockinfo> for scope + owner. */
typedef struct
{
    char scope[16];
    char owner[512];
} lockinfo_t;

static void parse_lockinfo(const char *xml, lockinfo_t *info)
{
    snprintf(info->scope, sizeof info->scope, "exclusive");
    info->owner[0] = '\0';
    if (!xml || !*xml)
        return;

    /* Scope: look for <...shared/> inside the body. */
    if (xml_has_local_element(xml, "shared"))
        snprintf(info->scope, sizeof info->scope, "shared");
    else
        snprintf(info->scope, sizeof info->scope, "exclusive");

    /* Owner: grab text between <...owner> and </...owner> if present. */
    const char *owner_start = strstr(xml, "owner");
    if (owner_start)
    {
        const char *gt = strchr(owner_start, '>');
        if (gt)
        {
            const char *close = strstr(gt + 1, "</");
            if (close)
            {
                size_t n = (size_t)(close - (gt + 1));
                if (n >= sizeof info->owner)
                    n = sizeof info->owner - 1;
                memcpy(info->owner, gt + 1, n);
                info->owner[n] = '\0';
                /* Trim leading/trailing whitespace. */
                char *s = info->owner;
                while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
                    memmove(s, s + 1, strlen(s));
                size_t L = strlen(info->owner);
                while (L > 0 && (info->owner[L - 1] == ' ' || info->owner[L - 1] == '\t' ||
                                 info->owner[L - 1] == '\r' || info->owner[L - 1] == '\n'))
                    info->owner[--L] = '\0';
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* PROPFIND XML generation                                           */
/* ------------------------------------------------------------------ */

/* Emit the active lock (if any) for fs_path into <D:lockdiscovery>. */
static void build_lockdiscovery(strbuf_t *sb, const char *fs_path)
{
    pthread_mutex_lock(&g_lock_mutex);
    purge_expired_locks_locked();
    webdav_lock_t *lk = find_lock_by_path_locked(fs_path);
    if (lk)
    {
        sb_append(sb, "<D:activelock>");
        sb_append(sb, "<D:locktype><D:write/></D:locktype>");
        sb_appendf(sb, "<D:lockscope><D:%s/></D:lockscope>",
                   strcmp(lk->scope, "shared") == 0 ? "shared" : "exclusive");
        sb_appendf(sb, "<D:depth>%d</D:depth>", lk->depth);
        if (lk->owner[0])
        {
            sb_append(sb, "<D:owner>");
            sb_append_xml_escaped(sb, lk->owner);
            sb_append(sb, "</D:owner>");
        }
        sb_appendf(sb, "<D:timeout>Second-%ld</D:timeout>", lock_seconds_remaining(lk));
        sb_append(sb, "<D:locktoken><D:href>");
        sb_append_xml_escaped(sb, lk->token);
        sb_append(sb, "</D:href></D:locktoken>");
        sb_append(sb, "</D:activelock>");
    }
    pthread_mutex_unlock(&g_lock_mutex);
}

/* A single live property. For the "prop" request mode we match by name. */
typedef struct
{
    const char *name;   /* without prefix, e.g. "getetag" */
    char value[256];    /* text value (may be empty) */
    int is_resourcetype;/* special: emits <D:collection/> for dirs */
    int is_dir;
    int is_supportedlock;
    int is_lockdiscovery;
    char fs_path[1024]; /* for lockdiscovery */
} live_prop_t;

#define MAX_LIVE_PROPS 16

static int collect_live_properties(const char *href_path, const char *local_path,
                                   int is_dir, live_prop_t *props)
{
    (void)href_path;
    struct stat st;
    int have_stat = stat(local_path, &st) == 0;
    int n = 0;

    /* displayname */
    {
        const char *fn = path_filename(local_path);
        char tmp[256];
        if (!fn || !*fn)
        {
            /* derive from href */
            const char *h = href_path;
            const char *slash = strrchr(h, '/');
            fn = slash ? slash + 1 : h;
        }
        snprintf(tmp, sizeof tmp, "%s", fn ? fn : "");
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "displayname";
        snprintf(p->value, sizeof p->value, "%s", tmp);
    }

    /* resourcetype */
    {
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "resourcetype";
        p->is_resourcetype = 1;
        p->is_dir = is_dir;
    }

    if (is_dir)
    {
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "getcontenttype";
        snprintf(p->value, sizeof p->value, "httpd/unix-directory");
    }
    else
    {
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "getcontentlength";
        snprintf(p->value, sizeof p->value, "%llu",
                 have_stat ? (unsigned long long)st.st_size : 0ULL);

        p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "getcontenttype";
        snprintf(p->value, sizeof p->value, "%s", guess_content_type(local_path));
    }

    if (have_stat)
    {
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "getlastmodified";
        format_http_date(st.st_mtim.tv_sec, p->value, sizeof p->value);

        p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "creationdate";
        format_iso8601_date(st.st_mtim.tv_sec, p->value, sizeof p->value);
    }

    {
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "getetag";
        compute_etag(local_path, p->value, sizeof p->value);
    }

    /* Quota: fixed generous values so volume-level checks pass. */
    {
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "quota-available-bytes";
        snprintf(p->value, sizeof p->value, "%llu", 1024ULL * 1024 * 1024 * 1024);

        p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "quota-used-bytes";
        snprintf(p->value, sizeof p->value, "0");
    }

    {
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "supportedlock";
        p->is_supportedlock = 1;
    }

    {
        live_prop_t *p = &props[n++];
        memset(p, 0, sizeof *p);
        p->name = "lockdiscovery";
        p->is_lockdiscovery = 1;
        snprintf(p->fs_path, sizeof p->fs_path, "%s", local_path);
    }

    return n;
}

static void emit_prop_value(strbuf_t *sb, const live_prop_t *p, int name_only)
{
    sb_appendf(sb, "<D:%s", p->name);
    if (name_only)
    {
        sb_append(sb, "/>");
        return;
    }

    if (p->is_resourcetype)
    {
        if (p->is_dir)
            sb_append(sb, "><D:collection/></D:resourcetype>");
        else
            sb_append(sb, "/>");
        return;
    }
    if (p->is_supportedlock)
    {
        sb_append(sb,
                  ">"
                  "<D:lockentry><D:lockscope><D:exclusive/></D:lockscope>"
                  "<D:locktype><D:write/></D:locktype></D:lockentry>"
                  "<D:lockentry><D:lockscope><D:shared/></D:lockscope>"
                  "<D:locktype><D:write/></D:locktype></D:lockentry>"
                  "</D:supportedlock>");
        return;
    }
    if (p->is_lockdiscovery)
    {
        sb_append(sb, ">");
        build_lockdiscovery(sb, p->fs_path);
        sb_append(sb, "</D:lockdiscovery>");
        return;
    }

    if (p->value[0] == '\0')
    {
        sb_append(sb, "/>");
    }
    else
    {
        sb_append(sb, ">");
        sb_append_xml_escaped(sb, p->value);
        sb_appendf(sb, "</D:%s>", p->name);
    }
}

static void append_resource_xml(strbuf_t *sb, const char *href_path,
                                const char *local_path, const propfind_request_t *req)
{
    struct stat st;
    int exists = stat(local_path, &st) == 0;
    int is_dir = exists && S_ISDIR(st.st_mode);

    sb_append(sb, "<D:response><D:href>");
    sb_append_url_encoded(sb, href_path);
    if (is_dir && href_path[0] && href_path[strlen(href_path) - 1] != '/')
        sb_append(sb, "/");
    sb_append(sb, "</D:href>");

    if (!exists)
    {
        sb_append(sb, "<D:propstat><D:prop/><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat></D:response>");
        return;
    }

    live_prop_t available[MAX_LIVE_PROPS];
    int navail = collect_live_properties(href_path, local_path, is_dir, available);

    if (req->mode == PROPFIND_PROP)
    {
        /* Partition into found / missing. */
        int found_idx[MAX_LIVE_PROPS];
        int nfound = 0;
        char missing[MAX_REQ_PROPS][64];
        int nmissing = 0;

        for (int w = 0; w < req->prop_count; ++w)
        {
            int matched = -1;
            for (int a = 0; a < navail; ++a)
            {
                if (strcmp(available[a].name, req->props[w]) == 0)
                {
                    matched = a;
                    break;
                }
            }
            if (matched >= 0)
                found_idx[nfound++] = matched;
            else
            {
                snprintf(missing[nmissing], sizeof missing[nmissing], "%s", req->props[w]);
                nmissing++;
            }
        }

        if (nfound > 0)
        {
            sb_append(sb, "<D:propstat><D:prop>");
            for (int i = 0; i < nfound; ++i)
                emit_prop_value(sb, &available[found_idx[i]], 0);
            sb_append(sb, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>");
        }
        if (nmissing > 0)
        {
            sb_append(sb, "<D:propstat><D:prop>");
            for (int i = 0; i < nmissing; ++i)
                sb_appendf(sb, "<D:%s/>", missing[i]);
            sb_append(sb, "</D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>");
        }
    }
    else
    {
        int name_only = (req->mode == PROPFIND_PROPNAME);
        sb_append(sb, "<D:propstat><D:prop>");
        for (int a = 0; a < navail; ++a)
            emit_prop_value(sb, &available[a], name_only);
        sb_append(sb, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>");
    }

    sb_append(sb, "</D:response>");
}

static void append_recursive_contents(strbuf_t *sb, const char *parent_href,
                                       const char *local_path, const propfind_request_t *req)
{
    DIR *d = opendir(local_path);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child_href[2048];
        char child_local[2048];
        int ph_has_slash = parent_href[0] && parent_href[strlen(parent_href) - 1] == '/';
        snprintf(child_href, sizeof child_href, "%s%s%s",
                 parent_href, ph_has_slash ? "" : "/", ent->d_name);
        snprintf(child_local, sizeof child_local, "%s/%s", local_path, ent->d_name);

        append_resource_xml(sb, child_href, child_local, req);

        if (path_is_dir(child_local))
            append_recursive_contents(sb, child_href, child_local, req);
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/* HTML directory index                                              */
/* ------------------------------------------------------------------ */

static void append_html_head(strbuf_t *sb)
{
    sb_append(sb,
        "<!DOCTYPE html><html><head>"
        "<style>"
        "body{font-family:sans-serif;padding:20px;background:#f5f5f7;color:#1d1d1f;}"
        "ul{list-style:none;padding:0;}"
        "li{background:white;padding:12px;margin-bottom:6px;border-radius:6px;"
        "display:flex;flex-direction:column;justify-content:center;"
        "border:1px solid #d2d2d7;}"
        ".row{display:flex;align-items:center;justify-content:space-between;width:100%;}"
        "a{color:#0066cc;text-decoration:none;font-weight:500;}"
        "button{background:#0071e3;color:white;border:none;padding:6px 12px;"
        "border-radius:4px;cursor:pointer;font-size:13px;font-weight:bold;}"
        "button:hover{background:#0077ed;}"
        "#status-bar{margin:15px 0;font-weight:bold;color:#ff9500;font-size:16px;}"
        ".progress-container{width:100%;margin-top:10px;display:none;background:#e5e5ea;"
        "border-radius:4px;padding:10px;box-sizing:border-box;}"
        ".progress-row{display:flex;align-items:center;justify-content:space-between;"
        "margin:5px 0;font-size:12px;}"
        "progress{width:70%;height:14px;border-radius:6px;}"
        "</style>"
        "<script>"
        "function downloadSegment(url, start, end, index, onProg) {"
        "return new Promise((resolve, reject) => {"
        "const xhr = new XMLHttpRequest();"
        "xhr.open('GET', url, true);"
        "xhr.responseType = 'blob';"
        "xhr.setRequestHeader('Range', 'bytes=' + start + '-' + end);"
        "xhr.onprogress = (e) => { if (e.lengthComputable) onProg(index, e.loaded, e.total); };"
        "xhr.onload = () => { if (xhr.status === 206 || xhr.status === 200) resolve(xhr.response);"
        "else reject(new Error('Status ' + xhr.status)); };"
        "xhr.onerror = () => reject(new Error('Network error'));"
        "xhr.send();"
        "});"
        "}"
        "async function downloadParallel(fileUrl, saveName, elementId) {"
        "const status = document.getElementById('status-bar');"
        "const pContainer = document.getElementById('progress-' + elementId);"
        "status.innerText = 'Checking remote file headers...';"
        "try {"
        "const headRes = await fetch(fileUrl, { method: 'HEAD' });"
        "const totalSize = parseInt(headRes.headers.get('Content-Length'));"
        "const acceptRanges = headRes.headers.get('Accept-Ranges');"
        "if (!acceptRanges || isNaN(totalSize)) {"
        "status.innerText = 'Falling back to standard download...';"
        "window.location.href = fileUrl; return;"
        "}");

    sb_appendf(sb, "const threadCount = %d;\n", DOWNLOAD_SEGMENTS);

    sb_append(sb,
        "const chunkSize = Math.ceil(totalSize / threadCount);"
        "pContainer.style.display = 'block';"
        "status.innerText = 'Downloading segments in parallel...';"
        "const updateUI = (idx, loaded, total) => {"
        "const progBar = document.getElementById('bar-' + elementId + '-' + idx);"
        "const label = document.getElementById('lbl-' + elementId + '-' + idx);"
        "progBar.value = loaded; progBar.max = total;"
        "const pct = Math.round((loaded / total) * 100);"
        "label.innerText = 'Segment ' + (idx + 1) + ': ' + pct + '% (' +"
        "(loaded/1024/1024).toFixed(1) + 'MB / ' + (total/1024/1024).toFixed(1) + 'MB)';"
        "};"
        "const promises = [];"
        "for (let i = 0; i < threadCount; i++) {"
        "const start = i * chunkSize;"
        "const end = Math.min(start + chunkSize - 1, totalSize - 1);"
        "promises.push(downloadSegment(fileUrl, start, end, i, updateUI));"
        "}"
        "const blobs = await Promise.all(promises);"
        "status.innerText = 'Stitching fragments into local storage...';"
        "const finalBlob = new Blob(blobs, { type: 'application/octet-stream' });"
        "const link = document.createElement('a');"
        "link.href = URL.createObjectURL(finalBlob);"
        "link.download = saveName;"
        "link.click();"
        "status.innerText = 'Download Complete!';"
        "pContainer.style.display = 'none';"
        "} catch (err) {"
        "status.innerText = 'Error: ' + err.message;"
        "}"
        "}"
        "</script></head><body>");
}

/* Directory entry list sorting: dirs first, then case-insensitive name. */
typedef struct
{
    char name[256];
    int is_dir;
} dir_item_t;

static int dir_item_cmp(const void *a, const void *b)
{
    const dir_item_t *x = (const dir_item_t *)a;
    const dir_item_t *y = (const dir_item_t *)b;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* Build an HTML directory index for req_path (local_path on disk). */
static char *build_directory_html(const char *req_path, const char *local_path, size_t *out_len)
{
    strbuf_t sb;
    sb_init(&sb);
    append_html_head(&sb);
    sb_append(&sb, "<h2>File Index: ");
    sb_append_xml_escaped(&sb, req_path);
    sb_append(&sb, "</h2>");
    sb_append(&sb, "<div id='status-bar'>Ready</div><hr><ul>");

    if (strcmp(req_path, "/") != 0 && req_path[0])
    {
        char cur[2048];
        snprintf(cur, sizeof cur, "%s", req_path);
        size_t L = strlen(cur);
        if (L > 0 && cur[L - 1] == '/')
            cur[L - 1] = '\0';
        char parent[2048];
        path_parent(cur, parent, sizeof parent);
        size_t pl = strlen(parent);
        char parent_slash[2049];
        if (pl > 0 && parent[pl - 1] == '/')
            snprintf(parent_slash, sizeof parent_slash, "%s", parent);
        else
            snprintf(parent_slash, sizeof parent_slash, "%s/", parent);
        sb_append(&sb, "<li><div class='row'><a href=\"");
        sb_append_xml_escaped(&sb, parent_slash);
        sb_append(&sb, "\">.. (Parent Directory)</a></div></li>");
    }

    /* Collect entries. */
    dir_item_t *items = NULL;
    size_t ni = 0, ci = 0;
    DIR *d = opendir(local_path);
    if (d)
    {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL)
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (ni == ci)
            {
                ci = ci ? ci * 2 : 64;
                items = (dir_item_t *)realloc(items, ci * sizeof(dir_item_t));
            }
            snprintf(items[ni].name, sizeof items[ni].name, "%s", ent->d_name);
            char child[2048];
            snprintf(child, sizeof child, "%s/%s", local_path, ent->d_name);
            items[ni].is_dir = path_is_dir(child);
            ni++;
        }
        closedir(d);
    }

    if (items && ni > 1)
        qsort(items, ni, sizeof(dir_item_t), dir_item_cmp);

    for (size_t i = 0; i < ni; ++i)
    {
        const char *filename = items[i].name;
        char href[4096];
        int rp_has_slash = req_path[0] && req_path[strlen(req_path) - 1] == '/';
        snprintf(href, sizeof href, "%s%s%s", req_path, rp_has_slash ? "" : "/", filename);

        char el_id[32];
        snprintf(el_id, sizeof el_id, "file_%zu", i + 1);

        if (items[i].is_dir)
        {
            sb_append(&sb, "<li><div class='row'><a href=\"");
            sb_append_xml_escaped(&sb, href);
            sb_append(&sb, "/\">");
            sb_append_xml_escaped(&sb, filename);
            sb_append(&sb, "/</a></div></li>");
        }
        else
        {
            sb_append(&sb, "<li><div class='row'><a href=\"");
            sb_append_xml_escaped(&sb, href);
            sb_append(&sb, "\">");
            sb_append_xml_escaped(&sb, filename);
            sb_append(&sb, "</a><button onclick=\"downloadParallel('");
            sb_append_xml_escaped(&sb, href);
            sb_append(&sb, "', '");
            sb_append_xml_escaped(&sb, filename);
            sb_appendf(&sb, "', '%s')\">Fast Download</button></div>", el_id);
            sb_appendf(&sb, "<div class='progress-container' id='progress-%s'>", el_id);
            for (int t = 0; t < DOWNLOAD_SEGMENTS; ++t)
            {
                sb_appendf(&sb,
                    "<div class='progress-row'><span id='lbl-%s-%d'>Segment %d: 0%%</span>"
                    "<progress id='bar-%s-%d' value='0' max='100'></progress></div>",
                    el_id, t, t + 1, el_id, t);
            }
            sb_append(&sb, "</div></li>");
        }
    }

    free(items);
    sb_append(&sb, "</ul><hr></body></html>");

    *out_len = sb.len;
    return sb.data; /* caller frees */
}

/* ------------------------------------------------------------------ */
/* Response helpers                                                  */
/* ------------------------------------------------------------------ */

/* Add the NextCloud-emulation headers that make rclone (vendor=nextcloud) and
 * similar clients take their smarter, less chatty code path. Applied to every
 * response via the respond_* helpers and the hand-built responses below. */
static void add_nextcloud_headers(struct MHD_Response *resp)
{
    MHD_add_response_header(resp, "X-LFV", "1");
    MHD_add_response_header(resp, "OC-API-Version", "1.0");
}

/* Queue a response built from a malloc'd buffer (freed by MHD). */
static enum MHD_Result respond_buffer(struct MHD_Connection *conn, unsigned int status,
                                      void *buf, size_t len, const char *content_type)
{
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp)
    {
        free(buf);
        return MHD_NO;
    }
    if (content_type)
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, content_type);
    add_nextcloud_headers(resp);
    enum MHD_Result r = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return r;
}

/* Queue a response from a static/constant string (copied by MHD). */
static enum MHD_Result respond_text(struct MHD_Connection *conn, unsigned int status,
                                    const char *text, const char *content_type)
{
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(text), (void *)text, MHD_RESPMEM_MUST_COPY);
    if (!resp)
        return MHD_NO;
    if (content_type)
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, content_type);
    add_nextcloud_headers(resp);
    enum MHD_Result r = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return r;
}

static enum MHD_Result respond_empty(struct MHD_Connection *conn, unsigned int status)
{
    struct MHD_Response *resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    if (!resp)
        return MHD_NO;
    add_nextcloud_headers(resp);
    enum MHD_Result r = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return r;
}

/* ------------------------------------------------------------------ */
/* File serving (GET/HEAD) with range support                       */
/* ------------------------------------------------------------------ */

typedef struct
{
    int fd;
    uint64_t base;   /* starting offset of the served region */
} file_reader_t;

static ssize_t file_reader_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    file_reader_t *fr = (file_reader_t *)cls;
    off_t off = (off_t)(fr->base + pos);
    ssize_t total = 0;
    while ((size_t)total < max)
    {
        ssize_t n = pread(fr->fd, buf + total, max - (size_t)total, off + total);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return MHD_CONTENT_READER_END_WITH_ERROR;
        }
        if (n == 0)
            break; /* EOF */
        total += n;
    }
    if (total == 0)
        return MHD_CONTENT_READER_END_OF_STREAM;
    return total;
}

static void file_reader_free(void *cls)
{
    file_reader_t *fr = (file_reader_t *)cls;
    if (fr)
    {
        if (fr->fd >= 0)
            close(fr->fd);
        free(fr);
    }
}

/* Parse a single-range "bytes=start-end" header. Returns 1 if a valid range
 * was parsed into start/end (inclusive), 0 if no/invalid range. */
static int parse_range(const char *range, uint64_t file_size, uint64_t *start, uint64_t *end)
{
    if (!range)
        return 0;
    const char *eq = strchr(range, '=');
    if (!eq)
        return 0;
    const char *p = eq + 1;
    /* Only the first range spec is honored. */
    char first[128];
    size_t i = 0;
    while (p[i] && p[i] != ',' && i < sizeof first - 1)
    {
        first[i] = p[i];
        i++;
    }
    first[i] = '\0';

    char *dash = strchr(first, '-');
    if (!dash)
        return 0;

    if (dash == first)
    {
        /* suffix range: -N  => last N bytes */
        uint64_t n = strtoull(dash + 1, NULL, 10);
        if (n == 0)
            return 0;
        if (n > file_size)
            n = file_size;
        *start = file_size - n;
        *end = file_size - 1;
        return 1;
    }

    *start = strtoull(first, NULL, 10);
    if (*dash && *(dash + 1))
        *end = strtoull(dash + 1, NULL, 10);
    else
        *end = file_size - 1;

    if (*start > *end || *start >= file_size)
        return 0;
    if (*end >= file_size)
        *end = file_size - 1;
    return 1;
}

static enum MHD_Result serve_file(struct MHD_Connection *conn, const char *method,
                                  const char *local_path)
{
    struct stat st;
    if (stat(local_path, &st) != 0)
        return respond_text(conn, 500, "stat failed", "text/plain");

    uint64_t file_size = (uint64_t)st.st_size;
    const char *mime = guess_content_type(local_path);

    char etag[128], lastmod[64];
    compute_etag(local_path, etag, sizeof etag);
    format_http_date(st.st_mtim.tv_sec, lastmod, sizeof lastmod);

    const char *range_hdr =
        MHD_lookup_connection_value(conn, MHD_HEADER_KIND, MHD_HTTP_HEADER_RANGE);

    /* HEAD: headers only. */
    if (strcmp(method, "HEAD") == 0)
    {
        struct MHD_Response *resp =
            MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
        if (!resp)
            return MHD_NO;
        char clen[32];
        snprintf(clen, sizeof clen, "%llu", (unsigned long long)file_size);
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_LENGTH, clen);
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
        MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
        MHD_add_response_header(resp, MHD_HTTP_HEADER_ETAG, etag);
        MHD_add_response_header(resp, MHD_HTTP_HEADER_LAST_MODIFIED, lastmod);
        add_nextcloud_headers(resp);
        enum MHD_Result r = MHD_queue_response(conn, 200, resp);
        MHD_destroy_response(resp);
        return r;
    }

    int fd = open(local_path, O_RDONLY);
    if (fd < 0)
        return respond_text(conn, 500, "open failed", "text/plain");

    uint64_t start = 0, end = file_size ? file_size - 1 : 0;
    int is_range = parse_range(range_hdr, file_size, &start, &end);
    uint64_t content_len = file_size ? (end - start + 1) : 0;

    file_reader_t *fr = (file_reader_t *)malloc(sizeof *fr);
    if (!fr)
    {
        close(fd);
        return respond_text(conn, 500, "oom", "text/plain");
    }
    fr->fd = fd;
    fr->base = start;

    struct MHD_Response *resp = MHD_create_response_from_callback(
        content_len, 0x100000, file_reader_cb, fr, file_reader_free);
    if (!resp)
    {
        file_reader_free(fr);
        return MHD_NO;
    }

    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ETAG, etag);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_LAST_MODIFIED, lastmod);

    unsigned int status = 200;
    if (is_range && file_size)
    {
        char cr[96];
        snprintf(cr, sizeof cr, "bytes %llu-%llu/%llu",
                 (unsigned long long)start, (unsigned long long)end,
                 (unsigned long long)file_size);
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_RANGE, cr);
        status = 206;
    }

    add_nextcloud_headers(resp);
    enum MHD_Result r = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return r;
}

/* ------------------------------------------------------------------ */
/* Per-connection request context                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* PUT streaming state */
    int is_put;
    int put_fd;
    char target_path[2048]; /* body is written straight here (no temp file) */
    int target_existed;
    off_t allocated;        /* bytes preallocated so far (-1 = fallocate failed) */
    unsigned long long received;
    long long content_length; /* declared Content-Length, or -1 if unknown */
    char acc[PUT_FLUSH_THRESHOLD];
    size_t acc_len;
    int put_error;          /* set if a write failed mid-stream */
    int put_parent_missing;
    int put_aborted;        /* client dropped the connection mid-upload */
    int put_ready_to_commit;/* body fully written; kept only on clean termination */
    int put_length_required;/* no Content-Length and not chunked: unframed body */
    int put_oc_mtime;       /* X-OC-Mtime present and parseable */
    long long put_oc_secs;  /* parsed X-OC-Mtime seconds (unix time) */

    /* Buffered body for XML methods (PROPFIND/PROPPATCH/LOCK/etc.) */
    strbuf_t body;
} request_ctx_t;

/* Write a full buffer, retrying short writes. Returns 1 on success. */
static int write_all_fd(int fd, const char *data, size_t len)
{
    size_t written = 0;
    while (written < len)
    {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return 0;
        }
        written += (size_t)n;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* PUT handling (streaming)                                          */
/* ------------------------------------------------------------------ */

static int put_begin(request_ctx_t *ctx, struct MHD_Connection *conn, const char *path)
{
    snprintf(ctx->target_path, sizeof ctx->target_path, "%s", path);

    /* Record the declared Content-Length so put_finish() can verify the full
     * body actually arrived before keeping the target. -1 means the client
     * used chunked transfer (no length), in which case we fall back to the
     * MHD completion signal / termination code to decide. */
    ctx->content_length = -1;
    const char *clen = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                    MHD_HTTP_HEADER_CONTENT_LENGTH);
    if (clen)
    {
        char *end = NULL;
        long long v = strtoll(clen, &end, 10);
        if (end != clen && v >= 0)
            ctx->content_length = v;
    }

    /* ownCloud/Nextcloud modtime preservation: if the client sent X-OC-Mtime
     * (unix seconds), remember it so put_finish() can stamp the uploaded file
     * once the body is complete and echo "X-OC-Mtime: accepted". Parsed up
     * front here while the connection headers are readily available. */
    ctx->put_oc_mtime = 0;
    ctx->put_oc_secs = 0;
    const char *ocm = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "X-OC-Mtime");
    if (ocm)
    {
        char *end = NULL;
        long long secs = strtoll(ocm, &end, 10);
        if (end != ocm && secs >= 0)
        {
            ctx->put_oc_mtime = 1;
            ctx->put_oc_secs = secs;
        }
    }

    /* A PUT body must be framed, either by Content-Length or by chunked
     * Transfer-Encoding. If neither is present the body is delimited only by
     * connection close, so there is no way to tell a complete upload from a
     * truncated one -- MHD treats it as a zero-length body and would otherwise
     * let us commit an empty file over the target. Reject such requests with
     * 411 Length Required instead. */
    if (ctx->content_length < 0)
    {
        const char *te = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                      MHD_HTTP_HEADER_TRANSFER_ENCODING);
        int is_chunked = te && str_casestr(te, "chunked") != NULL;
        if (!is_chunked)
        {
            ctx->put_length_required = 1;
            return 0;
        }
    }

    char parent[2048];
    path_parent(ctx->target_path, parent, sizeof parent);
    if (parent[0] && !path_is_dir(parent))
    {
        ctx->put_parent_missing = 1;
        return 0;
    }

    ctx->target_existed = path_exists(ctx->target_path);

    /* Write the body straight to the target path (no temp file / rename). */
    ctx->put_fd = open(ctx->target_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ctx->put_fd < 0)
    {
        ctx->put_error = 1;
        return 0;
    }
    fchmod(ctx->put_fd, put_create_mode(ctx->target_path));
    ctx->allocated = 0;
    ctx->received = 0;
    ctx->acc_len = 0;
    return 1;
}

/* Reserve disk blocks for [offset, offset+len) WITHOUT zeroing the content.
 * Returns 0 on success, -1 if preallocation isn't available (caller then stops
 * trying and falls back to plain writes).
 *
 * On Linux (WSL) this uses the raw fallocate() syscall with mode 0, which only
 * marks blocks as reserved/unwritten -- it never zero-fills, and fails fast
 * with EOPNOTSUPP on filesystems that can't do it (unlike glibc's
 * posix_fallocate, which would emulate by writing zeros).
 *
 * On FreeBSD (PS5) there is no fallocate(); posix_fallocate() there is a direct
 * syscall that performs real block allocation and does NOT zero-fill, returning
 * an error on unsupported filesystems.
 *
 * On PS4 neither API is usable -- the libc headers declare posix_fallocate but
 * the symbol isn't provided by the SDK stub libraries -- so preallocation is
 * compiled out and reserve_space() always reports "unavailable", which makes
 * the caller fall back to plain writes. */
static int reserve_space(int fd, off_t offset, off_t len)
{
#if defined(PLATFORM_PS4)
    (void)fd;
    (void)offset;
    (void)len;
    return -1; /* no (linkable) preallocation API on PS4 */
#elif defined(__linux__)
    if (fallocate(fd, 0, offset, len) == 0)
        return 0;
    return -1;
#else
    if (posix_fallocate(fd, offset, len) == 0)
        return 0;
    return -1;
#endif
}

static int put_feed(request_ctx_t *ctx, const char *data, size_t len)
{
    if (ctx->put_fd < 0 || ctx->put_error)
        return 0;

    ctx->received += len;

    off_t alloc_chunk = (off_t)PUT_PREALLOC_CHUNK_BYTES;
    off_t alloc_min = (off_t)PUT_PREALLOC_MIN_BYTES;
    if (alloc_chunk > 0 && (off_t)ctx->received >= alloc_min && ctx->allocated >= 0)
    {
        while ((off_t)ctx->received > ctx->allocated - (off_t)PUT_FLUSH_THRESHOLD)
        {
#ifdef DEBUG
            /* Time each reservation so we can tell a true (near-instant) block
             * reservation / fast failure apart from a stalling zero-fill. */
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int rc = reserve_space(ctx->put_fd, ctx->allocated, alloc_chunk);
            int saved_errno = errno; /* capture before clock_gettime */
            clock_gettime(CLOCK_MONOTONIC, &t1);

            long long elapsed_us =
                (long long)(t1.tv_sec - t0.tv_sec) * 1000000LL +
                (t1.tv_nsec - t0.tv_nsec) / 1000LL;

            DBG_LOG("prealloc: offset=%lld chunk=%lld bytes rc=%d elapsed=%lld us (%.3f ms)",
                    (long long)ctx->allocated, (long long)alloc_chunk, rc,
                    elapsed_us, (double)elapsed_us / 1000.0);
#else
            int rc = reserve_space(ctx->put_fd, ctx->allocated, alloc_chunk);
#endif
            if (rc != 0)
            {
#ifdef DEBUG
                DBG_LOG("prealloc: disabled for this upload (reserve_space failed, errno=%d: %s)",
                        saved_errno, strerror(saved_errno));
#endif
                ctx->allocated = -1;
                break;
            }
            ctx->allocated += alloc_chunk;
        }
    }

    size_t off = 0;
    while (off < len)
    {
        size_t space = PUT_FLUSH_THRESHOLD - ctx->acc_len;
        size_t take = (len - off < space) ? (len - off) : space;
        memcpy(ctx->acc + ctx->acc_len, data + off, take);
        ctx->acc_len += take;
        off += take;
        if (ctx->acc_len == PUT_FLUSH_THRESHOLD)
        {
            if (!write_all_fd(ctx->put_fd, ctx->acc, ctx->acc_len))
            {
                ctx->put_error = 1;
                return 0;
            }
            ctx->acc_len = 0;
        }
    }
    return 1;
}

/* Finish a PUT: flush the accumulator, trim the preallocated tail, fsync and
 * close the target file, and validate the received length.
 *
 * The body is written straight to the target path (no temp file / rename), so
 * on any failure here -- write error or a short body versus Content-Length --
 * the partially written target is deleted. On success put_ready_to_commit is
 * set; the connection must still terminate cleanly for the write to be kept.
 * If MHD reports a non-clean termination, request_completed() deletes the
 * target (see there) -- that is how a connection cut before the acknowledgement
 * is treated as an error.
 *
 * Returns the HTTP status to send back now. */
static unsigned int put_finish(request_ctx_t *ctx)
{
    if (ctx->put_length_required)
        return 411;

    if (ctx->put_parent_missing)
        return 409;

    int ok = !ctx->put_error && ctx->put_fd >= 0;

    if (ok && ctx->acc_len > 0)
    {
        if (!write_all_fd(ctx->put_fd, ctx->acc, ctx->acc_len))
            ok = 0;
        ctx->acc_len = 0;
    }

    if (ok && ctx->allocated > 0 &&
        ftruncate(ctx->put_fd, (off_t)ctx->received) != 0)
        ok = 0;

    if (ok && fsync(ctx->put_fd) != 0)
        ok = 0;
    if (ctx->put_fd >= 0 && close(ctx->put_fd) != 0)
        ok = 0;
    ctx->put_fd = -1;

    if (!ok)
    {
        remove(ctx->target_path);
        ctx->target_path[0] = '\0'; /* already deleted; don't double-remove */
        return 500;
    }

    /* Length guard: if the client declared a Content-Length but we received
     * fewer bytes, the body was truncated -- delete the partial target. */
    if (ctx->content_length >= 0 &&
        ctx->received != (unsigned long long)ctx->content_length)
    {
        ctx->put_aborted = 1;
        remove(ctx->target_path);
        ctx->target_path[0] = '\0'; /* already deleted; don't double-remove */
        return 400;
    }

    /* Apply the client's requested modification time (X-OC-Mtime) now that the
     * body is complete. Best-effort: failure to set it doesn't fail the PUT.
     * The access handler echoes "X-OC-Mtime: accepted" when put_oc_mtime is
     * set so rclone (vendor=owncloud) skips a follow-up time-setting request.
     *
     * Use utimes() (struct timeval[2]) rather than utimensat()/UTIME_OMIT:
     * utimes is the common denominator across WSL, PS5 and PS4 (the PS4 libc
     * headers lack utimensat and UTIME_OMIT). Since utimes can't omit atime, we
     * read and preserve the current atime, falling back to atime=mtime if the
     * stat fails. */
    if (ctx->put_oc_mtime)
    {
        struct timeval times[2];
        struct stat cur;

        if (stat(ctx->target_path, &cur) == 0)
        {
            times[0].tv_sec = cur.st_atim.tv_sec;             /* preserve atime */
            times[0].tv_usec = (long)(cur.st_atim.tv_nsec / 1000);
        }
        else
        {
            times[0].tv_sec = (time_t)ctx->put_oc_secs;
            times[0].tv_usec = 0;
        }

        times[1].tv_sec = (time_t)ctx->put_oc_secs;           /* set mtime */
        times[1].tv_usec = 0;

        utimes(ctx->target_path, times);
    }

    /* Body fully written to the target. It is kept only if the request also
     * terminates cleanly; request_completed() removes it otherwise. */
    ctx->put_ready_to_commit = 1;
    return ctx->target_existed ? 204 : 201;
}

/* ------------------------------------------------------------------ */
/* WebDAV method handlers (operate on a fully-buffered body)         */
/* ------------------------------------------------------------------ */

static enum MHD_Result handle_options(struct MHD_Connection *conn)
{
    struct MHD_Response *resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    if (!resp)
        return MHD_NO;
    MHD_add_response_header(resp, "Accept-Ranges", "bytes");
    MHD_add_response_header(resp, "Allow",
        "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, COPY, MOVE, LOCK, UNLOCK");
    MHD_add_response_header(resp, "DAV", "1, 2");
    add_nextcloud_headers(resp);
    enum MHD_Result r = MHD_queue_response(conn, 200, resp);
    MHD_destroy_response(resp);
    return r;
}

static enum MHD_Result handle_propfind(struct MHD_Connection *conn, const char *req_path,
                                       const char *local_path, const char *body)
{
    if (!path_exists(local_path))
        return respond_text(conn, 404, "Not Found", "text/plain");

    const char *depth = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Depth");
    if (!depth)
        depth = "infinity";
    if (strcmp(depth, "0") != 0 && strcmp(depth, "1") != 0 && strcmp(depth, "infinity") != 0)
        return respond_text(conn, 400, "Invalid Depth header.", "text/plain");

    propfind_request_t req;
    parse_propfind(body, &req);

    strbuf_t sb;
    sb_init(&sb);
    sb_append(&sb, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    sb_append(&sb, "<D:multistatus xmlns:D=\"DAV:\">");

    append_resource_xml(&sb, req_path, local_path, &req);

    if (path_is_dir(local_path))
    {
        if (strcmp(depth, "1") == 0)
        {
            DIR *d = opendir(local_path);
            if (d)
            {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL)
                {
                    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                        continue;
                    char child_href[2048], child_local[2048];
                    int rp_slash = req_path[0] && req_path[strlen(req_path) - 1] == '/';
                    snprintf(child_href, sizeof child_href, "%s%s%s",
                             req_path, rp_slash ? "" : "/", ent->d_name);
                    snprintf(child_local, sizeof child_local, "%s/%s", local_path, ent->d_name);
                    append_resource_xml(&sb, child_href, child_local, &req);
                }
                closedir(d);
            }
        }
        else if (strcmp(depth, "infinity") == 0)
        {
            append_recursive_contents(&sb, req_path, local_path, &req);
        }
    }

    sb_append(&sb, "</D:multistatus>");

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(sb.len, sb.data, MHD_RESPMEM_MUST_FREE);
    if (!resp)
    {
        sb_free(&sb);
        return MHD_NO;
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/xml; charset=utf-8");
    MHD_add_response_header(resp, "DAV", "1, 2");
    MHD_add_response_header(resp, "Accept-Ranges", "bytes");
    add_nextcloud_headers(resp);
    enum MHD_Result r = MHD_queue_response(conn, 207, resp);
    MHD_destroy_response(resp);
    return r;
}

static enum MHD_Result handle_proppatch(struct MHD_Connection *conn, const char *req_path,
                                        const char *body)
{
    /* We accept PROPPATCH but store nothing; echo success for each requested
     * property. If the body has no recognizable prop children, 400. */
    propfind_request_t pr;
    pr.mode = PROPFIND_PROP;
    pr.prop_count = 0;
    if (body && *body)
        collect_prop_children(body, &pr);

    if (pr.prop_count == 0)
        return respond_text(conn, 400, "Malformed or empty XML propertyupdate body.", "text/plain");

    strbuf_t sb;
    sb_init(&sb);
    sb_append(&sb, "<?xml version=\"1.0\"?>");
    sb_append(&sb, "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
    sb_append_url_encoded(&sb, req_path);
    sb_append(&sb, "</D:href><D:propstat><D:prop>");
    for (int i = 0; i < pr.prop_count; ++i)
        sb_appendf(&sb, "<D:%s/>", pr.props[i]);
    sb_append(&sb, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>");

    return respond_buffer(conn, 207, sb.data, sb.len, "text/xml; charset=utf-8");
}

static enum MHD_Result handle_mkcol(struct MHD_Connection *conn, const char *local_path)
{
    if (mkdir(local_path, 0777) == 0)
        return respond_empty(conn, 201);
    return respond_empty(conn, 405);
}

static enum MHD_Result handle_delete(struct MHD_Connection *conn, const char *req_path,
                                     const char *local_path)
{
    if (!path_exists(local_path))
    {
        strbuf_t sb;
        sb_init(&sb);
        sb_append(&sb, "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
        sb_append_url_encoded(&sb, req_path);
        sb_append(&sb, "</D:href><D:status>HTTP/1.1 404 Not Found</D:status></D:response></D:multistatus>");
        return respond_buffer(conn, 207, sb.data, sb.len, "application/xml; charset=\"utf-8\"");
    }

    char fail_path[2048];
    int fail_errno = 0;
    if (delete_recursive_strict(local_path, fail_path, sizeof fail_path, &fail_errno) != 0)
    {
        strbuf_t sb;
        sb_init(&sb);
        sb_append(&sb, "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
        sb_append_url_encoded(&sb, fail_path);
        sb_append(&sb, "</D:href><D:status>HTTP/1.1 ");
        if (fail_errno == EACCES || fail_errno == EPERM)
            sb_append(&sb, "403 Forbidden");
        else if (fail_errno == EAGAIN || fail_errno == EBUSY)
            sb_append(&sb, "423 Locked");
        else if (fail_errno == EROFS)
            sb_append(&sb, "405 Method Not Allowed");
        else
            sb_append(&sb, "500 Internal Server Error");
        sb_append(&sb, "</D:status></D:response></D:multistatus>");
        return respond_buffer(conn, 207, sb.data, sb.len, "application/xml; charset=\"utf-8\"");
    }

    return respond_empty(conn, 204);
}

static enum MHD_Result handle_copy(struct MHD_Connection *conn, const char *local_path)
{
    const char *dest_hdr = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Destination");
    if (!dest_hdr)
        return respond_text(conn, 400, "Missing 'Destination' header.", "text/plain");

    char *dest = parse_destination_path(dest_hdr);
    if (!dest)
        return respond_text(conn, 500, "oom", "text/plain");

    enum MHD_Result ret;
    if (!path_exists(local_path))
    {
        ret = respond_text(conn, 404, "Source resource not found.", "text/plain");
        goto out;
    }

    const char *overwrite = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Overwrite");
    int dest_exists = path_exists(dest);
    if (dest_exists && overwrite && strcmp(overwrite, "F") == 0)
    {
        ret = respond_text(conn, 412, "Destination exists and Overwrite is False.", "text/plain");
        goto out;
    }

    char dparent[2048];
    path_parent(dest, dparent, sizeof dparent);
    if (!path_exists(dparent))
    {
        ret = respond_text(conn, 409, "Conflict: Destination parent collection does not exist.", "text/plain");
        goto out;
    }

    const char *depth = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Depth");
    int recursive = !(depth && strcmp(depth, "0") == 0);

    if (dest_exists)
        safe_remove_all(dest);

    if (!recursive && path_is_dir(local_path))
    {
        if (mkdir(dest, 0777) != 0 && errno != EEXIST)
        {
            ret = respond_text(conn, 500, "Internal server error copying resource.", "text/plain");
            goto out;
        }
    }
    else if (copy_recursive(local_path, dest, 1) != 0)
    {
        ret = respond_text(conn, 500, "Internal server error copying resource.", "text/plain");
        goto out;
    }

    ret = respond_empty(conn, dest_exists ? 204 : 201);
out:
    free(dest);
    return ret;
}

static enum MHD_Result handle_move(struct MHD_Connection *conn, const char *local_path)
{
    const char *dest_hdr = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Destination");
    if (!dest_hdr)
        return respond_text(conn, 400, "Missing 'Destination' header.", "text/plain");

    char *dest = parse_destination_path(dest_hdr);
    if (!dest)
        return respond_text(conn, 500, "oom", "text/plain");

    enum MHD_Result ret;
    if (!path_exists(local_path))
    {
        ret = respond_text(conn, 404, "Source resource not found.", "text/plain");
        goto out;
    }

    const char *overwrite = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Overwrite");
    int dest_exists = path_exists(dest);
    if (dest_exists && overwrite && strcmp(overwrite, "F") == 0)
    {
        ret = respond_text(conn, 412, "Destination exists and Overwrite is False.", "text/plain");
        goto out;
    }

    char dparent[2048];
    path_parent(dest, dparent, sizeof dparent);
    if (!path_exists(dparent))
    {
        ret = respond_text(conn, 409, "Conflict: Destination parent collection does not exist.", "text/plain");
        goto out;
    }

    if (dest_exists)
        safe_remove_all(dest);

    if (rename(local_path, dest) == 0)
    {
        ret = respond_empty(conn, dest_exists ? 204 : 201);
        goto out;
    }

    if (errno == EXDEV)
    {
        /* Cross-device: copy then remove source. */
        if (copy_recursive(local_path, dest, 1) == 0)
        {
            safe_remove_all(local_path);
            ret = respond_empty(conn, dest_exists ? 204 : 201);
            goto out;
        }
        ret = respond_text(conn, 500, "Cross-device move fallback failed.", "text/plain");
        goto out;
    }

    ret = respond_text(conn, 500, "Internal server error moving file resource.", "text/plain");
out:
    free(dest);
    return ret;
}

static enum MHD_Result handle_lock(struct MHD_Connection *conn, const char *req_path,
                                   const char *body)
{
    lockinfo_t info;
    parse_lockinfo(body, &info);

    const char *if_hdr = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "If");

    pthread_mutex_lock(&g_lock_mutex);
    purge_expired_locks_locked();

    time_t new_expiry = time(NULL) + LOCK_TIMEOUT_SECONDS;
    webdav_lock_t *existing = find_lock_by_path_locked(req_path);

    char raw_token[160]; /* holds a UUID (36 chars); sized to match token field */
    char scope_used[16];
    long timeout_secs;

    if (existing)
    {
        int token_matches = if_hdr && strstr(if_hdr, existing->token) != NULL;
        if (token_matches)
        {
            existing->expiry = new_expiry;
            snprintf(scope_used, sizeof scope_used, "%s", existing->scope);
            timeout_secs = lock_seconds_remaining(existing);
            const char *raw = existing->token;
            const char *pfx = "opaquelocktoken:";
            if (strncmp(raw, pfx, strlen(pfx)) == 0)
                raw += strlen(pfx);
            snprintf(raw_token, sizeof raw_token, "%s", raw);

            char full_token[160];
            snprintf(full_token, sizeof full_token, "%s", existing->token);
            pthread_mutex_unlock(&g_lock_mutex);

            strbuf_t sb;
            sb_init(&sb);
            sb_append(&sb, "<?xml version=\"1.0\"?>");
            sb_append(&sb, "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>");
            sb_append(&sb, "<D:locktype><D:write/></D:locktype>");
            sb_appendf(&sb, "<D:lockscope><D:%s/></D:lockscope>",
                       strcmp(scope_used, "shared") == 0 ? "shared" : "exclusive");
            sb_append(&sb, "<D:depth>0</D:depth>");
            sb_appendf(&sb, "<D:timeout>Second-%ld</D:timeout>", timeout_secs);
            sb_append(&sb, "<D:locktoken><D:href>");
            sb_append_url_encoded(&sb, full_token);
            sb_append(&sb, "</D:href></D:locktoken><D:lockroot><D:href>");
            sb_append_url_encoded(&sb, req_path);
            sb_append(&sb, "</D:href></D:lockroot></D:activelock></D:lockdiscovery></D:prop>");

            struct MHD_Response *resp =
                MHD_create_response_from_buffer(sb.len, sb.data, MHD_RESPMEM_MUST_FREE);
            char lt[176];
            snprintf(lt, sizeof lt, "<%s>", full_token);
            MHD_add_response_header(resp, "Lock-Token", lt);
            MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/xml; charset=utf-8");
            add_nextcloud_headers(resp);
            enum MHD_Result r = MHD_queue_response(conn, 200, resp);
            MHD_destroy_response(resp);
            return r;
        }

        if (strcmp(existing->scope, "exclusive") == 0 || strcmp(info.scope, "exclusive") == 0)
        {
            pthread_mutex_unlock(&g_lock_mutex);
            return respond_text(conn, 423, "Resource already locked.", "text/plain");
        }
    }

    /* Create a new lock. */
    generate_uuid(raw_token);
    char full_token[160];
    snprintf(full_token, sizeof full_token, "opaquelocktoken:%s", raw_token);

    webdav_lock_t *lk = alloc_lock_locked();
    if (!lk)
    {
        pthread_mutex_unlock(&g_lock_mutex);
        return respond_text(conn, 507, "Lock table full.", "text/plain");
    }
    memset(lk, 0, sizeof *lk);
    lk->in_use = 1;
    snprintf(lk->path, sizeof lk->path, "%s", req_path);
    snprintf(lk->token, sizeof lk->token, "%s", full_token);
    snprintf(lk->scope, sizeof lk->scope, "%s", info.scope);
    snprintf(lk->owner, sizeof lk->owner, "%s", info.owner);
    lk->depth = 0;
    lk->expiry = new_expiry;
    snprintf(scope_used, sizeof scope_used, "%s", info.scope);
    pthread_mutex_unlock(&g_lock_mutex);

    strbuf_t sb;
    sb_init(&sb);
    sb_append(&sb, "<?xml version=\"1.0\"?>");
    sb_append(&sb, "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>");
    sb_append(&sb, "<D:locktype><D:write/></D:locktype>");
    sb_appendf(&sb, "<D:lockscope><D:%s/></D:lockscope>",
               strcmp(scope_used, "shared") == 0 ? "shared" : "exclusive");
    sb_append(&sb, "<D:depth>0</D:depth>");
    sb_appendf(&sb, "<D:timeout>Second-%d</D:timeout>", LOCK_TIMEOUT_SECONDS);
    sb_append(&sb, "<D:locktoken><D:href>");
    sb_append_url_encoded(&sb, full_token);
    sb_append(&sb, "</D:href></D:locktoken><D:lockroot><D:href>");
    sb_append_url_encoded(&sb, req_path);
    sb_append(&sb, "</D:href></D:lockroot></D:activelock></D:lockdiscovery></D:prop>");

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(sb.len, sb.data, MHD_RESPMEM_MUST_FREE);
    char lt[176];
    snprintf(lt, sizeof lt, "<%s>", full_token);
    MHD_add_response_header(resp, "Lock-Token", lt);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/xml; charset=utf-8");
    add_nextcloud_headers(resp);
    enum MHD_Result r = MHD_queue_response(conn, 200, resp);
    MHD_destroy_response(resp);
    return r;
}

static enum MHD_Result handle_unlock(struct MHD_Connection *conn, const char *req_path)
{
    const char *lt = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Lock-Token");
    if (!lt)
        return respond_text(conn, 400, "Missing 'Lock-Token' verification header.", "text/plain");

    char clean[160];
    snprintf(clean, sizeof clean, "%s", lt);
    size_t L = strlen(clean);
    if (L >= 2 && clean[0] == '<' && clean[L - 1] == '>')
    {
        memmove(clean, clean + 1, L - 2);
        clean[L - 2] = '\0';
    }

    pthread_mutex_lock(&g_lock_mutex);
    purge_expired_locks_locked();
    webdav_lock_t *lk = find_lock_by_token_locked(clean);
    if (!lk || strcmp(lk->path, req_path) != 0)
    {
        pthread_mutex_unlock(&g_lock_mutex);
        return respond_text(conn, 412, "Lock token does not match or resource is not locked.", "text/plain");
    }
    lk->in_use = 0;
    pthread_mutex_unlock(&g_lock_mutex);

    return respond_empty(conn, 204);
}

/* ------------------------------------------------------------------ */
/* Request logging                                                   */
/* ------------------------------------------------------------------ */

/* Request logging. Entirely compiled out of release builds -- the LOG_REQUEST()
 * macro gates the call sites, so neither these functions nor the work they do
 * exist unless DEBUG is defined. */
#ifdef DEBUG
static enum MHD_Result log_header_cb(void *cls, enum MHD_ValueKind kind,
                                     const char *key, const char *value)
{
    (void)kind;
    strbuf_t *sb = (strbuf_t *)cls;
    sb_appendf(sb, "%s: %s\n", key, value ? value : "");
    return MHD_YES;
}

static void log_request(struct MHD_Connection *conn, const char *method,
                        const char *url, unsigned int status)
{
    strbuf_t sb;
    sb_init(&sb);
    sb_append(&sb, "================================\n");
    sb_appendf(&sb, "%s %s\n", method, url);
    MHD_get_connection_values(conn, MHD_HEADER_KIND, log_header_cb, &sb);
    sb_append(&sb, "--------------------------------\n");
    sb_appendf(&sb, "status: %u\n", status);
    dbglogger_log("%s", sb.data ? sb.data : "");
    sb_free(&sb);
}
#endif

/* ------------------------------------------------------------------ */
/* Main access handler                                               */
/* ------------------------------------------------------------------ */

static enum MHD_Result access_handler(void *cls, struct MHD_Connection *conn,
                                       const char *url, const char *method,
                                       const char *version, const char *upload_data,
                                       size_t *upload_data_size, void **con_cls)
{
    (void)cls;
    (void)version;

    /* First call for this request: allocate context. */
    if (*con_cls == NULL)
    {
        request_ctx_t *ctx = (request_ctx_t *)calloc(1, sizeof *ctx);
        if (!ctx)
            return MHD_NO;
        ctx->put_fd = -1;
        sb_init(&ctx->body);
        *con_cls = ctx;

        /* For PUT, open the target file up front so we can stream the body. */
        if (strcmp(method, "PUT") == 0)
        {
            ctx->is_put = 1;
            char *local = url_decode(url);
            if (local)
            {
                strip_trailing_slashes(local);
                put_begin(ctx, conn, local);
                free(local);
            }
        }
        return MHD_YES; /* no response yet; wait for body (if any) */
    }

    request_ctx_t *ctx = (request_ctx_t *)*con_cls;

    /* Decode the URL path to a local filesystem path. Collection requests from
     * some clients carry a trailing slash (e.g. "/parent/New Folder/"); strip
     * it so filesystem calls (stat/rename/parent lookups) behave correctly. */
    char *local_path = url_decode(url);
    if (!local_path)
        return MHD_NO;
    strip_trailing_slashes(local_path);

    enum MHD_Result ret = MHD_NO;
    unsigned int logged_status = 0;

    /* ---- PUT: consume streamed body across callbacks ---- */
    if (ctx->is_put)
    {
        if (*upload_data_size != 0)
        {
            put_feed(ctx, upload_data, *upload_data_size);
            *upload_data_size = 0;
            free(local_path);
            return MHD_YES; /* more body may follow */
        }

        /* Body complete: prepare (flush/validate) but defer the commit. */
        unsigned int status = put_finish(ctx);
        logged_status = status;

        if (status == 409)
        {
            ret = respond_text(conn, 409, "Parent collection does not exist.", "text/plain");
        }
        else if (status == 411)
        {
            ret = respond_text(conn, 411,
                               "Length Required: PUT needs Content-Length or chunked encoding.",
                               "text/plain");
        }
        else if (status == 400)
        {
            ret = respond_text(conn, 400,
                               "Incomplete upload: body shorter than Content-Length.",
                               "text/plain");
        }
        else if (status == 500)
        {
            ret = respond_text(conn, 500, "Write failed.", "text/plain");
        }
        else if (status == 201)
        {
            struct MHD_Response *resp = MHD_create_response_from_buffer(
                strlen("Created"), (void *)"Created", MHD_RESPMEM_MUST_COPY);
            MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain");
            if (ctx->put_oc_mtime)
                MHD_add_response_header(resp, "X-OC-Mtime", "accepted");
            add_nextcloud_headers(resp);
            ret = MHD_queue_response(conn, 201, resp);
            MHD_destroy_response(resp);
        }
        else /* 204 */
        {
            struct MHD_Response *resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
            if (ctx->put_oc_mtime)
                MHD_add_response_header(resp, "X-OC-Mtime", "accepted");
            add_nextcloud_headers(resp);
            ret = MHD_queue_response(conn, 204, resp);
            MHD_destroy_response(resp);
        }

        LOG_REQUEST(conn, method, url, logged_status);
        free(local_path);
        return ret;
    }

    /* ---- Other methods that carry a body: buffer it first ---- */
    int body_method = (strcmp(method, "PROPFIND") == 0 ||
                       strcmp(method, "PROPPATCH") == 0 ||
                       strcmp(method, "LOCK") == 0 ||
                       strcmp(method, "POST") == 0);

    if (body_method && *upload_data_size != 0)
    {
        sb_append_len(&ctx->body, upload_data, *upload_data_size);
        *upload_data_size = 0;
        free(local_path);
        return MHD_YES; /* accumulate more body */
    }

    const char *body = ctx->body.data ? ctx->body.data : "";

    /* ---- Dispatch by method ---- */
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0)
    {
        /* Special service endpoints. */
        if (strcmp(url, "/stop") == 0)
        {
            /* Only signal here. Tearing the daemon down from inside its own
             * worker thread deadlocks (MHD can't join itself); the Start()
             * loop notices stop_server and calls MHD_stop_daemon safely. */
            stop_server = 1;
            ret = respond_empty(conn, 200);
            logged_status = 200;
            goto done;
        }
        if (strcmp(url, "/version") == 0)
        {
            char ver[32];
            snprintf(ver, sizeof ver, "%.2f", (double)APP_VERSION);
            ret = respond_text(conn, 200, ver, "text/html");
            logged_status = 200;
            goto done;
        }

        struct stat st;
        if (stat(local_path, &st) != 0)
        {
            ret = respond_text(conn, 404, "<h1>Not Found</h1>", "text/html");
            logged_status = 404;
        }
        else if (S_ISDIR(st.st_mode))
        {
            size_t len = 0;
            char *html = build_directory_html(url, local_path, &len);
            if (!html)
            {
                ret = respond_text(conn, 500, "oom", "text/plain");
                logged_status = 500;
            }
            else if (strcmp(method, "HEAD") == 0)
            {
                free(html);
                ret = respond_empty(conn, 200);
                logged_status = 200;
            }
            else
            {
                ret = respond_buffer(conn, 200, html, len, "text/html");
                logged_status = 200;
            }
        }
        else
        {
            ret = serve_file(conn, method, local_path);
            logged_status = 200;
        }
    }
    else if (strcmp(method, "OPTIONS") == 0)
    {
        ret = handle_options(conn);
        logged_status = 200;
    }
    else if (strcmp(method, "PROPFIND") == 0)
    {
        ret = handle_propfind(conn, url, local_path, body);
        logged_status = 207;
    }
    else if (strcmp(method, "PROPPATCH") == 0)
    {
        ret = handle_proppatch(conn, url, body);
        logged_status = 207;
    }
    else if (strcmp(method, "MKCOL") == 0)
    {
        ret = handle_mkcol(conn, local_path);
        logged_status = 201;
    }
    else if (strcmp(method, "DELETE") == 0)
    {
        ret = handle_delete(conn, url, local_path);
        logged_status = 204;
    }
    else if (strcmp(method, "COPY") == 0)
    {
        ret = handle_copy(conn, local_path);
        logged_status = 201;
    }
    else if (strcmp(method, "MOVE") == 0)
    {
        ret = handle_move(conn, local_path);
        logged_status = 201;
    }
    else if (strcmp(method, "LOCK") == 0)
    {
        ret = handle_lock(conn, url, body);
        logged_status = 200;
    }
    else if (strcmp(method, "UNLOCK") == 0)
    {
        ret = handle_unlock(conn, url);
        logged_status = 204;
    }
    else
    {
        ret = respond_text(conn, 405, "Method Not Allowed", "text/plain");
        logged_status = 405;
    }

done:
    LOG_REQUEST(conn, method, url, logged_status);
    free(local_path);
    return ret;
}

/* Clean up per-request context. */
static void request_completed(void *cls, struct MHD_Connection *conn,
                              void **con_cls, enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)conn;
    request_ctx_t *ctx = (request_ctx_t *)*con_cls;
    if (!ctx)
        return;

    if (ctx->is_put)
    {
        /* This is the single authoritative keep/discard point for a PUT.
         *
         * The body is written straight to the target path, so by the time we
         * get here the target already holds whatever bytes arrived. MHD only
         * reports MHD_REQUEST_TERMINATED_COMPLETED_OK when the whole request --
         * including the body -- was received and the response was handed off
         * cleanly. For a chunked / no-Content-Length upload this is the ONLY
         * reliable "the body really finished" signal: if the socket is cut
         * before the terminating chunk (or before the acknowledgement), MHD
         * never makes the handler's body-done pass and instead lands here with
         * a non-OK termination code.
         *
         *   - clean termination + a finished upload -> keep the target
         *   - anything else                          -> delete the target
         *
         * Deleting on a non-clean termination is how a connection cut before
         * the acknowledgement is turned into an error: the half-written (or
         * even fully-written-but-unacknowledged) target is removed rather than
         * left in place. */
        int keep = (toe == MHD_REQUEST_TERMINATED_COMPLETED_OK) && ctx->put_ready_to_commit;

        if (!keep)
            ctx->put_aborted = 1;

        /* Close any still-open fd (upload aborted mid-stream). */
        if (ctx->put_fd >= 0)
        {
            close(ctx->put_fd);
            ctx->put_fd = -1;
        }

        /* Discard the target unless the upload finished and terminated
         * cleanly. put_finish() clears target_path after its own error-path
         * deletes, so this won't double-remove (and won't clobber a file a
         * later request may have recreated at the same path). */
        if (!keep && ctx->target_path[0] != '\0')
            remove(ctx->target_path);
    }

    sb_free(&ctx->body);
    free(ctx);
    *con_cls = NULL;
}

/* ------------------------------------------------------------------ */
/* Server lifecycle                                                  */
/* ------------------------------------------------------------------ */

static struct MHD_Daemon *start_daemon(void)
{
    unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG;
#ifdef MHD_USE_THREAD_PER_CONNECTION
    flags |= MHD_USE_THREAD_PER_CONNECTION;
#endif

    return MHD_start_daemon(
        flags,
        (uint16_t)http_server_port,
        NULL, NULL,
        &access_handler, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)120,
        MHD_OPTION_END);
}

void WebDAVServer_Start(void)
{
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    while (!stop_server)
    {
        if (!in_rest_mode)
        {
            Util_Notify("Starting WebDav Server %.2f on port %d", (double)APP_VERSION, http_server_port);

            g_daemon = start_daemon();
            if (!g_daemon)
            {
                Util_Notify("Failed to start WebDAV daemon on port %d", http_server_port);
                return;
            }

            /* Block until stop is requested; MHD serves on its own threads. */
            while (!stop_server && !in_rest_mode)
                sleep(1);

            if (g_daemon)
            {
                MHD_stop_daemon(g_daemon);
                g_daemon = NULL;
            }
        }
        else
        {
            sleep(2);
        }
    }
}

void WebDAVServer_Stop(void)
{
    /* Signal only; the Start() loop performs the actual MHD_stop_daemon so the
     * daemon is never torn down from within one of its own worker threads. */
    stop_server = 1;
}

int WebDAVServer_IsStarted(void)
{
    /* Probe the local /version endpoint with a short-lived TCP connection. */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)http_server_port);
    addr.sin_addr.s_addr = htonl(0x7F000001); /* 127.0.0.1 */

    int connected = connect(sock, (struct sockaddr *)&addr, sizeof addr) == 0;
    if (!connected)
    {
        close(sock);
        return 0;
    }

    const char *req = "GET /version HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    if (write(sock, req, strlen(req)) < 0)
    {
        close(sock);
        return 0;
    }

    char buf[64];
    ssize_t n = read(sock, buf, sizeof buf - 1);
    close(sock);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    /* Any HTTP response at all means a server is already listening. */
    return strstr(buf, "HTTP/") != NULL ? 1 : 0;
}

void WebDAVServer_SetRestMode(int toggle)
{
    in_rest_mode = toggle;
}
