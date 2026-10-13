/*
 * rastertoricohddst - CUPS raster filter for Ricoh DDST GDI printers
 * (Aficio SP 100 / SP 111 / SP 112 / SP 204 families).
 *
 * Reads CUPS raster (1 bit per pixel, black = 1) and writes the PJL + JBIG
 * stream these printers expect. Everything happens in memory: no temporary
 * files, no external tools, and pages are sent as soon as they are encoded.
 *
 * Usage (as called by CUPS):
 *   rastertoricohddst job-id user title copies options [file]
 *
 * Based on the protocol reverse-engineered in pstoricohddst-gdi by
 * Serge V Shistarev (madlynx) and contributors.
 *
 * License: GPL-2.0-or-later
 */

#include <cups/cups.h>
#include <cups/raster.h>
#include <jbig.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define EOL "\r\n"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Divisor for the DOTCOUNT value reported to the printer (toner estimate).
 * Matches the behaviour of the last maintained shell filter, which divided
 * the black-pixel count by 100. */
#define DOTCOUNT_DIVISOR 100

/* Sanity limits for raster headers. */
#define MIN_RES 72
#define MAX_RES 2400
#define MAX_PAGE_PT 2016 /* 28 inches, twice US Legal */

static volatile sig_atomic_t canceled = 0;

static void on_term(int sig)
{
    (void)sig;
    canceled = 1;
}

/* Growable buffer that receives the JBIG bitstream. */
struct buf {
    unsigned char *data;
    size_t len, cap;
};

static void buf_append(unsigned char *start, size_t len, void *ctx)
{
    struct buf *b = ctx;

    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 256 * 1024;
        while (cap < b->len + len)
            cap *= 2;
        unsigned char *p = realloc(b->data, cap);
        if (!p) {
            fputs("ERROR: Out of memory while compressing page\n", stderr);
            exit(1);
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, start, len);
    b->len += len;
}

/* Paper names understood by the printer, matched by size in points. */
static const struct {
    const char *name;
    int w, h;
} paper_sizes[] = {
    { "A4", 595, 842 },
    { "A5", 420, 595 },
    { "A6", 297, 420 },
    { "LETTER", 612, 792 },
    { "LEGAL", 612, 1008 },
    { "B5", 516, 729 },
    { "B6", 363, 516 },
    { "MONARCH", 279, 540 },
};

static void paper_name(const cups_page_header2_t *h, const char *fallback,
                       char *out, size_t outlen)
{
    const size_t n = sizeof(paper_sizes) / sizeof(paper_sizes[0]);
    const char *src = h->cupsPageSizeName[0] ? h->cupsPageSizeName : fallback;

    if (src && !strncasecmp(src, "Env", 3))
        src += 3;  /* EnvMonarch -> MONARCH */
    for (size_t i = 0; src && i < n; i++) {
        if (!strcasecmp(src, paper_sizes[i].name)) {
            snprintf(out, outlen, "%s", paper_sizes[i].name);
            return;
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (abs((int)h->PageSize[0] - paper_sizes[i].w) <= 2 &&
            abs((int)h->PageSize[1] - paper_sizes[i].h) <= 2) {
            snprintf(out, outlen, "%s", paper_sizes[i].name);
            return;
        }
    }

    /* Unknown size: send the name upper-cased, like the old filter. The
     * name comes from the job (options or raster header), so only letters,
     * digits and a little punctuation are kept: anything else, notably CR/LF,
     * could inject PJL commands. */
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < outlen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c == '.' || c == '-')
            out[j++] = (char)toupper(c);
    }
    out[j] = '\0';
    if (!out[0])
        snprintf(out, outlen, "A4");
}

/* Keep PJL string values printable ASCII so they cannot break the stream. */
static void pjl_clean(const char *in, char *out, size_t outlen)
{
    size_t j = 0;

    for (; in && *in && j + 1 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        out[j++] = (c >= 0x20 && c < 0x7f && c != '"') ? (char)c : '_';
    }
    out[j] = '\0';
    if (!out[0])
        snprintf(out, outlen, "Document");
}

static size_t count_black(const unsigned char *p, size_t n)
{
    size_t dots = 0;

    for (size_t i = 0; i < n; i++)
        dots += (size_t)__builtin_popcount(p[i]);
    return dots;
}

/*
 * Halftoning for 8-bit gray input ("Photo" mode).
 *
 * A clustered-dot screen at 45 degrees: the dots grow from the centre, so the
 * printer draws a few solid dots rather than many isolated pixels, which a
 * laser reproduces much more evenly. The screen cell is SCREEN_N pixels
 * square and holds two dots, giving 600 / (10 / sqrt 2) = 85 lines per inch
 * and about 50 gray levels.
 *
 * Toner spreads on paper, so dark grays print darker than requested; the
 * tone curve (darkness ^ DOT_GAIN_GAMMA) lightens them to compensate.
 */
#define SCREEN_N 10
#define DOT_GAIN_GAMMA 1.4

static unsigned char screen[SCREEN_N][SCREEN_N];
static unsigned char tone[256];

static void halftone_init(void)
{
    double spot[SCREEN_N * SCREEN_N];
    int order[SCREEN_N * SCREEN_N];
    const int n = SCREEN_N * SCREEN_N;

    for (int y = 0; y < SCREEN_N; y++) {
        for (int x = 0; x < SCREEN_N; x++) {
            double u = 2 * M_PI * (x + y + 1) / SCREEN_N;
            double v = 2 * M_PI * (x - y) / SCREEN_N;
            spot[y * SCREEN_N + x] = cos(u) + cos(v);
            order[y * SCREEN_N + x] = y * SCREEN_N + x;
        }
    }
    /* Rank pixels by spot value, highest (dot centre) first. Insertion sort
     * is stable, so equal values keep their positional order. */
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        while (j >= 0 && spot[order[j]] < spot[k]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    /* Thresholds stay within 1..254, so pure black and white pass through
     * unchanged and text stays crisp. */
    for (int r = 0; r < n; r++)
        screen[order[r] / SCREEN_N][order[r] % SCREEN_N] =
            (unsigned char)(1 + r * 253 / (n - 1));

    for (int d = 0; d < 256; d++)
        tone[d] = (unsigned char)(255.0 * pow(d / 255.0, DOT_GAIN_GAMMA) + 0.5);
}

/* Convert one 8-bit row to 1 bit per pixel (1 = black). (px, py) is the
 * row's position on the page, so the screen lines up across pages. */
static void halftone_row(const unsigned char *in, unsigned width, int black_is_255,
                         unsigned px, unsigned py, unsigned char *out)
{
    const unsigned char *thr = screen[py % SCREEN_N];

    memset(out, 0, (width + 7) / 8);
    for (unsigned x = 0; x < width; x++) {
        unsigned char d = black_is_255 ? in[x] : (unsigned char)(255 - in[x]);
        if (tone[d] > thr[(px + x) % SCREEN_N] || d == 255)
            out[x / 8] |= (unsigned char)(0x80 >> (x % 8));
    }
}

static int write_all(const void *data, size_t len)
{
    return fwrite(data, 1, len, stdout) == len ? 0 : -1;
}

int main(int argc, char *argv[])
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job-id user title copies options [file]\n",
                argv[0]);
        return 1;
    }

    int fd = 0;
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        fprintf(stderr, "ERROR: Unable to open raster file \"%s\": %s\n",
                argv[6], strerror(errno));
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv[5], 0, &options);
    const char *opt_size = cupsGetOption("PageSize", num_options, options);
    const char *opt_slot = cupsGetOption("InputSlot", num_options, options);
    const char *opt_type = cupsGetOption("MediaType", num_options, options);

    char user[64], title[64], slot[32], mtype[32];
    pjl_clean(argv[2], user, sizeof(user));
    pjl_clean(argv[3], title, sizeof(title));
    pjl_clean(opt_slot && strcasecmp(opt_slot, "Auto") ? opt_slot : "TRAY1",
              slot, sizeof(slot));
    pjl_clean(opt_type && strcasecmp(opt_type, "Auto") ? opt_type
                                                       : "PLAINRECYCLE",
              mtype, sizeof(mtype));

    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y/%m/%d %H:%M:%S", localtime(&now));

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t h;
    unsigned char *bitmap = NULL, *row = NULL, *bits = NULL;
    size_t bitmap_cap = 0, row_cap = 0, bits_cap = 0;
    int halftone_ready = 0;
    struct buf jbig = { 0 };
    int page = 0, status = 0;

    while (!canceled && cupsRasterReadHeader2(ras, &h)) {
        int gray = h.cupsBitsPerPixel == 8 &&
                   (h.cupsColorSpace == CUPS_CSPACE_W || h.cupsColorSpace == CUPS_CSPACE_SW ||
                    h.cupsColorSpace == CUPS_CSPACE_K);
        if (h.cupsBitsPerPixel != 1 && !gray) {
            fprintf(stderr, "ERROR: Unsupported raster format (%u bits per "
                    "pixel, color space %u); the PPD must request 1-bit or "
                    "8-bit gray output\n", h.cupsBitsPerPixel, h.cupsColorSpace);
            status = 1;
            break;
        }
        if (gray && !halftone_ready) {
            halftone_init();
            halftone_ready = 1;
        }
        if (h.cupsWidth == 0 || h.cupsHeight == 0)
            continue;

        /* The raster can come straight from a user (CUPS accepts raw
         * raster jobs), so never trust its geometry. */
        unsigned xres = h.HWResolution[0], yres = h.HWResolution[1];
        if (xres < MIN_RES || xres > MAX_RES || yres < MIN_RES || yres > MAX_RES ||
            h.cupsWidth > MAX_PAGE_PT * xres / 72 ||
            h.cupsHeight > MAX_PAGE_PT * yres / 72 ||
            h.cupsBytesPerLine != (h.cupsWidth * h.cupsBitsPerPixel + 7) / 8) {
            fprintf(stderr, "ERROR: Invalid raster page header (%ux%u pixels, "
                    "%ux%u dpi, %u bytes per line)\n", h.cupsWidth, h.cupsHeight,
                    xres, yres, h.cupsBytesPerLine);
            status = 1;
            break;
        }

        page++;
        fprintf(stderr, "INFO: Processing page %d\n", page);

        /* gstoraster crops the raster to the PPD's imageable area, but the
         * printer expects a bitmap of the whole sheet (as Ghostscript's
         * pbmraw device produced for the old filter). Place the raster on a
         * full-page canvas at the position of the imageable area. */
        /* All arithmetic in double: header fields are 32-bit and would
         * overflow when multiplied by the resolution. */
        unsigned page_w = h.cupsWidth, page_h = h.cupsHeight, ox = 0, oy = 0;
        if (h.PageSize[0] <= MAX_PAGE_PT && h.PageSize[1] <= MAX_PAGE_PT) {
            double pw = h.PageSize[0] * (double)xres / 72.0 + 0.5;
            double ph = h.PageSize[1] * (double)yres / 72.0 + 0.5;
            if (pw >= h.cupsWidth && ph >= h.cupsHeight) {
                page_w = (unsigned)pw;
                page_h = (unsigned)ph;
            }
        }
        double fx = h.ImagingBoundingBox[0] * (double)xres / 72.0 + 0.5;
        double fy = h.ImagingBoundingBox[3] && h.ImagingBoundingBox[3] < h.PageSize[1]
                        ? ((double)h.PageSize[1] - h.ImagingBoundingBox[3]) * yres / 72.0 + 0.5
                        : 0.0;
        ox = fx > page_w - h.cupsWidth ? page_w - h.cupsWidth : (unsigned)fx;
        oy = fy > page_h - h.cupsHeight ? page_h - h.cupsHeight : (unsigned)fy;

        size_t bpl = (page_w + 7) / 8;
        size_t size = bpl * page_h;
        if (size > bitmap_cap) {
            free(bitmap);
            if (!(bitmap = malloc(size))) {
                fputs("ERROR: Out of memory for page bitmap\n", stderr);
                status = 1;
                break;
            }
            bitmap_cap = size;
        }
        if (h.cupsBytesPerLine > row_cap) {
            free(row);
            if (!(row = malloc(h.cupsBytesPerLine))) {
                fputs("ERROR: Out of memory for raster line\n", stderr);
                status = 1;
                break;
            }
            row_cap = h.cupsBytesPerLine;
        }
        size_t src_bytes = (h.cupsWidth + 7) / 8;
        if (gray && src_bytes > bits_cap) {
            free(bits);
            if (!(bits = malloc(src_bytes))) {
                fputs("ERROR: Out of memory for raster line\n", stderr);
                status = 1;
                break;
            }
            bits_cap = src_bytes;
        }
        memset(bitmap, 0, size);

        int invert = h.cupsColorSpace == CUPS_CSPACE_W ||
                     h.cupsColorSpace == CUPS_CSPACE_SW;
        unsigned char tail_mask = h.cupsWidth % 8 ? (unsigned char)(0xff << (8 - h.cupsWidth % 8))
                                                  : 0xff;
        unsigned shift = ox % 8;

        for (unsigned y = 0; y < h.cupsHeight && !canceled; y++) {
            /* A truncated or canceled page is still finished (blank from
             * here on) so the printer is not left in the middle of a job. */
            if (cupsRasterReadPixels(ras, row, h.cupsBytesPerLine) == 0)
                break;
            unsigned char *src = row;
            if (gray) {
                halftone_row(row, h.cupsWidth, h.cupsColorSpace == CUPS_CSPACE_K,
                             ox, oy + y, bits);
                src = bits;
            } else if (invert) {
                for (size_t i = 0; i < src_bytes; i++)
                    row[i] = (unsigned char)~row[i];
            }
            src[src_bytes - 1] &= tail_mask;

            unsigned char *dst = bitmap + (size_t)(oy + y) * bpl + ox / 8;
            if (shift == 0) {
                memcpy(dst, src, src_bytes);
            } else {
                for (size_t i = 0; i < src_bytes; i++) {
                    dst[i] |= (unsigned char)(src[i] >> shift);
                    if (ox / 8 + i + 1 < bpl)
                        dst[i + 1] |= (unsigned char)(src[i] << (8 - shift));
                }
            }
        }

        size_t dots = count_black(bitmap, size) / DOTCOUNT_DIVISOR;

        /* Same parameters as "pbmtojbg -p 72 -o 3 -m 0 -q", which the
         * printer is known to accept. */
        struct jbg_enc_state s;
        unsigned char *planes[1] = { bitmap };
        jbig.len = 0;
        jbg_enc_init(&s, page_w, page_h, 1, planes, buf_append, &jbig);
        jbg_enc_layers(&s, 0);
        jbg_enc_options(&s, JBG_ILEAVE | JBG_SMID, JBG_LRLTWO | JBG_TPBON, 0, 0, -1);
        jbg_enc_out(&s);
        jbg_enc_free(&s);

        if (page == 1)
            printf("\033%%-12345X@PJL" EOL
                   "@PJL SET TIMESTAMP=%s" EOL
                   "@PJL SET FILENAME=%s" EOL
                   "@PJL SET COMPRESS=JBIG" EOL
                   "@PJL SET USERNAME=%s" EOL
                   "@PJL SET COVER=OFF" EOL
                   "@PJL SET HOLD=OFF" EOL,
                   stamp, title, user);

        char paper[64];
        paper_name(&h, opt_size, paper, sizeof(paper));

        printf("@PJL SET PAGESTATUS=START" EOL
               "@PJL SET COPIES=1" EOL
               "@PJL SET MEDIASOURCE=%s" EOL
               "@PJL SET MEDIATYPE=%s" EOL
               "@PJL SET PAPER=%s" EOL
               "@PJL SET PAPERWIDTH=%u" EOL
               "@PJL SET PAPERLENGTH=%u" EOL
               "@PJL SET RESOLUTION=%u" EOL
               "@PJL SET IMAGELEN=%zu" EOL,
               slot, mtype, paper, page_w, page_h, xres, jbig.len);
        if (write_all(jbig.data, jbig.len) < 0) {
            fputs("ERROR: Unable to write print data\n", stderr);
            status = 1;
            break;
        }
        printf("@PJL SET DOTCOUNT=%zu" EOL
               "@PJL SET PAGESTATUS=END" EOL,
               dots);
        fflush(stdout);

        fprintf(stderr, "PAGE: %d 1\n", page);
    }

    if (page > 0) {
        printf("@PJL EOJ" EOL "\033%%-12345X\n");
        fflush(stdout);
    }

    if (canceled)
        fputs("INFO: Job canceled\n", stderr);
    else if (status == 0)
        fprintf(stderr, "INFO: Ready to print (%d page%s)\n", page,
                page == 1 ? "" : "s");

    cupsRasterClose(ras);
    cupsFreeOptions(num_options, options);
    free(bitmap);
    free(row);
    free(bits);
    free(jbig.data);
    if (fd)
        close(fd);
    return status;
}
