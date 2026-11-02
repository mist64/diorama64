#include "sm64_pack_stamp.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include "sha256_portable.h"
#endif
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STAMP_MAX_BYTES (1024 * 1024)

static int fail(char *err, int cap, const char *fmt, ...) {
    if (err && cap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, (size_t)cap, fmt, ap);
        va_end(ap);
    }
    return 0;
}
/* A regular file (never a link) opened for reading; -1 otherwise. */
static int open_regular(const char *path, struct stat *st) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { return -1; }
    if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode)) { close(fd); return -1; }
    return fd;
}
static int sha256_fd(int fd, unsigned char out[CC_SHA256_DIGEST_LENGTH]) {
    static __thread unsigned char buf[1 << 16];
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    for (;;) {
        const ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { return 0; }
        if (r == 0) { break; }
        CC_SHA256_Update(&c, buf, (CC_LONG)r);
    }
    CC_SHA256_Final(out, &c);
    return 1;
}
static int hex_nibble(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int ends_with(const char *s, size_t n, const char *suf) { const size_t b = strlen(suf); return n > b && !memcmp(s + n - b, suf, b); }
static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int sm64_pack_stamp_load(const char *dir, sm64_pack_stamp *out, char *err, int errcap) {
    if (!out) { return fail(err, errcap, "no output"); }
    memset(out, 0, sizeof *out);
    if (err && errcap > 0) { err[0] = '\0'; }
    char path[2048];
    if (!dir || snprintf(path, sizeof path, "%s/%s", dir, SM64_PACK_STAMP_FILE) >= (int)sizeof path) { return fail(err, errcap, "path too long"); }
    struct stat st;
    const int sfd = open_regular(path, &st);
    if (sfd < 0) { return fail(err, errcap, "no staging stamp (stage it from the Mac)"); }
    if (st.st_size <= 0 || st.st_size > STAMP_MAX_BYTES) { close(sfd); return fail(err, errcap, "stamp size %lld", (long long)st.st_size); }
    char *text = malloc((size_t)st.st_size + 1);
    ssize_t got = 0;
    while (text && got < st.st_size) {
        const ssize_t r = read(sfd, text + got, (size_t)(st.st_size - got));
        if (r <= 0) { break; }
        got += r;
    }
    close(sfd);
    if (!text || got != st.st_size) { free(text); return fail(err, errcap, "cannot read the stamp"); }
    text[got] = '\0';
    if (memchr(text, '\0', (size_t)got)) { free(text); return fail(err, errcap, "stamp holds a NUL byte"); }
    typedef struct { char name[SM64_PACK_STAMP_NAME_MAX]; unsigned char sha[CC_SHA256_DIGEST_LENGTH]; } entry;
    int lines = 0;
    for (const char *p = text; *p; p++) { lines += *p == '\n'; }
    if (lines == 0 || lines > SM64_PACK_STAMP_MAX_FILES || text[got - 1] != '\n') { free(text); return fail(err, errcap, "malformed stamp"); }
    entry *e = calloc((size_t)lines, sizeof *e);
    if (!e) { free(text); return fail(err, errcap, "out of memory"); }
    int n = 0, ok = 1;
    for (char *line = text; ok && *line; ) {
        char *nl = strchr(line, '\n');
        const size_t len = (size_t)(nl - line);
        if (len < 64 + 2 + 1 || line[64] != ' ' || line[65] != ' ') { ok = fail(err, errcap, "malformed stamp line %d", n + 1); break; }
        for (int i = 0; i < 32 && ok; i++) {
            const int hi = hex_nibble(line[2 * i]), lo = hex_nibble(line[2 * i + 1]);
            if (hi < 0 || lo < 0) { ok = fail(err, errcap, "malformed hash on stamp line %d", n + 1); }
            e[n].sha[i] = (unsigned char)(hi << 4 | lo);
        }
        const char *name = line + 66;
        const size_t nlen = len - 66;
        if (ok && (nlen >= SM64_PACK_STAMP_NAME_MAX || name[0] == '.' || memchr(name, '/', nlen) ||
                   !(ends_with(name, nlen, ".tex") || ends_with(name, nlen, ".bin")))) {
            ok = fail(err, errcap, "bad file name on stamp line %d", n + 1);
        }
        if (ok) { memcpy(e[n].name, name, nlen); e[n].name[nlen] = '\0'; n++; }
        line = nl + 1;
    }
    free(text);
    if (!ok) { free(e); return 0; }
    qsort(e, (size_t)n, sizeof *e, cmp_name);
    for (int i = 1; i < n; i++) {
        if (!strcmp(e[i - 1].name, e[i].name)) { fail(err, errcap, "%s listed twice", e[i].name); free(e); return 0; }
    }
    double bytes = 0;
    int tex = 0, bins = 0;
    for (int i = 0; i < n; i++) {
        char f[2048 + SM64_PACK_STAMP_NAME_MAX];
        snprintf(f, sizeof f, "%s/%s", dir, e[i].name);
        struct stat fs;
        const int fd = open_regular(f, &fs);
        if (fd < 0) { ok = fail(err, errcap, "%s missing or not a regular file", e[i].name); break; }
        unsigned char sha[CC_SHA256_DIGEST_LENGTH];
        const int hashed = sha256_fd(fd, sha);
        close(fd);
        if (!hashed || memcmp(sha, e[i].sha, sizeof sha) != 0) { ok = fail(err, errcap, "%s does not match its staged sha256", e[i].name); break; }
        bytes += (double)fs.st_size;
        const size_t l = strlen(e[i].name);
        tex += ends_with(e[i].name, l, ".tex");
        bins += ends_with(e[i].name, l, ".bin");
    }
    if (!ok) { free(e); return 0; }
    out->names = malloc((size_t)n * sizeof *out->names);
    if (!out->names) { free(e); return fail(err, errcap, "out of memory"); }
    for (int i = 0; i < n; i++) { memcpy(out->names[i], e[i].name, SM64_PACK_STAMP_NAME_MAX); }
    free(e);
    out->n = n; out->tex = tex; out->bins = bins; out->mb = bytes / 1048576.0;
    return 1;
}
int sm64_pack_stamp_lists(const sm64_pack_stamp *s, const char *file) {
    if (!s || !s->names || !file) { return 0; }
    return bsearch(file, s->names, (size_t)s->n, sizeof *s->names, cmp_name) != NULL;
}
void sm64_pack_stamp_free(sm64_pack_stamp *s) {
    if (!s) { return; }
    free(s->names);
    memset(s, 0, sizeof *s);
}
