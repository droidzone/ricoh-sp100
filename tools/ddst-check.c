/*
 * ddst-check - verify a Ricoh DDST print stream offline (no printer needed).
 *
 * usage: ddst-check stream.prn [reference-page.pbm ...]
 *
 * For every page it checks that:
 *   - the PJL framing is complete (UEL header, PAGESTATUS START/END, EOJ),
 *   - the JBIG data decodes and matches PAPERWIDTH x PAPERLENGTH,
 *   - re-encoding the bitmap with the printer's known-good JBIG options
 *     ("pbmtojbg -p 72 -o 3 -m 0 -q") gives byte-identical data,
 *   - if reference PBM files are given (e.g. rendered by Ghostscript's
 *     pbmraw device, like the old shell filter did), the bitmap has the same
 *     size, is positioned identically (it matches the reference better than
 *     any copy shifted by one pixel) and differs in fewer than 3% of pixels.
 *     Some difference is normal: two Ghostscript runs round glyph edges
 *     slightly differently.
 *
 * Exit status is 0 when every check passes.
 *
 * License: GPL-2.0-or-later
 */

#include <jbig.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pixel(const unsigned char *img, size_t bpl, unsigned long x,
                 unsigned long y)
{
    return (img[y * bpl + x / 8] >> (7 - x % 8)) & 1;
}

/* Pixels that differ between img shifted by (dx, dy) and ref, ignoring a
 * one-pixel border. */
static size_t count_diff(const unsigned char *img, const unsigned char *ref,
                         unsigned long w, unsigned long h, int dx, int dy)
{
    size_t bpl = (w + 7) / 8, diff = 0;

    for (unsigned long y = 1; y + 1 < h; y++)
        for (unsigned long x = 1; x + 1 < w; x++)
            diff += (size_t)(pixel(img, bpl, x + dx, y + dy) != pixel(ref, bpl, x, y));
    return diff;
}

struct buf {
    unsigned char *data;
    size_t len, cap;
};

static void buf_append(unsigned char *start, size_t len, void *ctx)
{
    struct buf *b = ctx;

    if (b->len + len > b->cap) {
        b->cap = (b->len + len) * 2;
        if (!(b->data = realloc(b->data, b->cap))) {
            perror("realloc");
            exit(2);
        }
    }
    memcpy(b->data + b->len, start, len);
    b->len += len;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    unsigned char *data = malloc(n > 0 ? (size_t)n : 1);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return data;
}

/* Raw PBM ("P4"), skipping comments. Returns pointer to the pixel data. */
static const unsigned char *pbm_parse(const unsigned char *d, size_t len,
                                      unsigned long *w, unsigned long *h)
{
    unsigned long v[2];
    size_t pos = 2;

    if (len < 3 || d[0] != 'P' || d[1] != '4')
        return NULL;
    for (int i = 0; i < 2; i++) {
        for (;;) {
            while (pos < len && isspace(d[pos]))
                pos++;
            if (pos < len && d[pos] == '#') {
                while (pos < len && d[pos] != '\n')
                    pos++;
                continue;
            }
            break;
        }
        v[i] = 0;
        while (pos < len && isdigit(d[pos]))
            v[i] = v[i] * 10 + (unsigned long)(d[pos++] - '0');
    }
    pos++;  /* single whitespace before the raster */
    *w = v[0];
    *h = v[1];
    if (pos + (*w + 7) / 8 * *h > len)
        return NULL;
    return d + pos;
}

static int starts_with(const unsigned char *p, const unsigned char *end,
                       const char *s)
{
    size_t n = strlen(s);
    return (size_t)(end - p) >= n && !memcmp(p, s, n);
}

static unsigned long pjl_value(const unsigned char *p, const unsigned char *end,
                               const char *key)
{
    size_t n = strlen(key);
    return (size_t)(end - p) > n ? strtoul((const char *)p + n, NULL, 10) : 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s stream.prn [reference-page.pbm ...]\n", argv[0]);
        return 2;
    }

    size_t len;
    unsigned char *data = read_file(argv[1], &len);
    if (!data)
        return 2;

    const unsigned char *p = data, *end = data + len;
    int fail = 0, page = 0, in_page = 0, saw_eoj = 0;
    unsigned long width = 0, height = 0;

    if (!starts_with(p, end, "\033%-12345X@PJL\r\n")) {
        puts("FAIL: stream does not start with UEL + @PJL");
        fail = 1;
    }

    while (p < end) {
        const unsigned char *nl = memchr(p, '\n', (size_t)(end - p));
        const unsigned char *next = nl ? nl + 1 : end;

        if (starts_with(p, end, "@PJL SET PAGESTATUS=START")) {
            if (in_page) {
                printf("FAIL: page %d not ended before next START\n", page);
                fail = 1;
            }
            in_page = 1;
            page++;
        } else if (starts_with(p, end, "@PJL SET PAGESTATUS=END")) {
            in_page = 0;
        } else if (starts_with(p, end, "@PJL SET PAPERWIDTH=")) {
            width = pjl_value(p, end, "@PJL SET PAPERWIDTH=");
        } else if (starts_with(p, end, "@PJL SET PAPERLENGTH=")) {
            height = pjl_value(p, end, "@PJL SET PAPERLENGTH=");
        } else if (starts_with(p, end, "@PJL EOJ")) {
            saw_eoj = 1;
        } else if (starts_with(p, end, "@PJL SET IMAGELEN=")) {
            size_t n = pjl_value(p, end, "@PJL SET IMAGELEN=");
            if (n > (size_t)(end - next)) {
                printf("FAIL: page %d: IMAGELEN %zu exceeds stream\n", page, n);
                fail = 1;
                break;
            }

            /* Decode. */
            struct jbg_dec_state d;
            size_t used = 0;
            jbg_dec_init(&d);
            int r = jbg_dec_in(&d, (unsigned char *)next, n, &used);
            if (r != JBG_EOK) {
                printf("FAIL: page %d: JBIG decode error: %s\n", page, jbg_strerror(r));
                fail = 1;
                jbg_dec_free(&d);
                next += n;
                p = next;
                continue;
            }
            unsigned long w = jbg_dec_getwidth(&d), h = jbg_dec_getheight(&d);
            unsigned char *img = jbg_dec_getimage(&d, 0);
            int ok = 1;

            printf("page %d: %lux%lu, %zu bytes JBIG", page, w, h, n);
            if (w != width || h != height) {
                printf(", FAIL: PJL says %lux%lu", width, height);
                ok = 0;
            }

            /* Re-encode with the known-good options and compare. */
            struct buf ref = { 0 };
            struct jbg_enc_state s;
            unsigned char *planes[1] = { img };
            jbg_enc_init(&s, w, h, 1, planes, buf_append, &ref);
            jbg_enc_layers(&s, 0);
            jbg_enc_options(&s, JBG_ILEAVE | JBG_SMID, JBG_LRLTWO | JBG_TPBON, 0, 0, -1);
            jbg_enc_out(&s);
            jbg_enc_free(&s);
            if (ref.len == n && !memcmp(ref.data, next, n)) {
                printf(", encoding OK");
            } else {
                printf(", FAIL: encoding differs from pbmtojbg -p 72 -o 3 -m 0 -q");
                ok = 0;
            }
            free(ref.data);

            /* Compare with a reference rendering. */
            if (page + 1 < argc) {
                size_t rlen;
                unsigned char *rdata = read_file(argv[page + 1], &rlen);
                unsigned long rw = 0, rh = 0;
                const unsigned char *rpix = rdata ? pbm_parse(rdata, rlen, &rw, &rh) : NULL;
                if (!rpix) {
                    printf(", FAIL: cannot read reference %s", argv[page + 1]);
                    ok = 0;
                } else if (rw != w || rh != h) {
                    printf(", FAIL: reference is %lux%lu", rw, rh);
                    ok = 0;
                } else {
                    static const int shifts[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    size_t diff = count_diff(img, rpix, w, h, 0, 0);
                    double pct = 100.0 * (double)diff / ((double)w * (double)h);
                    int aligned = 1;
                    for (int i = 0; i < 4; i++)
                        if (count_diff(img, rpix, w, h, shifts[i][0], shifts[i][1]) < diff)
                            aligned = 0;
                    printf(", vs reference %.3f%% pixels differ, %s", pct,
                           aligned ? "aligned" : "MISALIGNED");
                    if (pct >= 3.0 || !aligned) {
                        printf(" FAIL");
                        ok = 0;
                    }
                }
                free(rdata);
            }
            putchar('\n');
            fail |= !ok;
            jbg_dec_free(&d);
            next += n;
        }
        p = next;
    }

    if (in_page) {
        printf("FAIL: page %d has no PAGESTATUS=END\n", page);
        fail = 1;
    }
    if (!saw_eoj) {
        puts("FAIL: no @PJL EOJ");
        fail = 1;
    }
    if (page == 0) {
        puts("FAIL: no pages");
        fail = 1;
    }
    if (argc > 2 && argc - 2 != page) {
        printf("FAIL: %d reference pages given, stream has %d\n", argc - 2, page);
        fail = 1;
    }
    puts(fail ? "FAIL" : "PASS");
    free(data);
    return fail;
}
