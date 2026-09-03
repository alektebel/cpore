#include "cpore/cpore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>

/* Play the cell stage from the terminal.
 *
 *   ./build/cpore_play --seed 23           (kitty graphics; needs WezTerm/kitty)
 *   ./build/cpore_play --blocks            (ANSI half-block fallback, any term)
 *
 * You drive the four control dims of the action vector - steer x/y, a
 * flagella burst and an electric zap - and leave the design head zero, so the
 * scripted designer auto-evolves your parts each time the DNA meter fills a
 * segment.
 *
 * Default output is the real RGBA frame - the same pixels that become the PNG
 * stills - transmitted with the kitty graphics protocol so it shows at full
 * fidelity inside a supporting terminal. --blocks packs the frame into ANSI
 * truecolour half-blocks (one char = two stacked pixels) for terminals without
 * inline images.
 *
 * Controls: WASD or arrows to steer, space to burst, J to zap, P pause, Q quit.
 */

static struct termios g_saved_termios;
static int g_raw = 0;

static void restore_terminal(void)
{
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_raw = 0;
    }
    fputs("\x1b_Ga=d\x1b\\", stdout);        /* wipe any leftover images */
    fputs("\x1b[?25h\x1b[0m\x1b[?1049l", stdout);
    fflush(stdout);
}

static void on_signal(int sig) { (void)sig; restore_terminal(); _exit(0); }

static void raw_mode(void)
{
    tcgetattr(STDIN_FILENO, &g_saved_termios);
    struct termios t = g_saved_termios;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    g_raw = 1;
    atexit(restore_terminal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- base64, for the kitty graphics payload ---- */
static char *b64_encode(const uint8_t *in, size_t n, char *out)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        *out++ = T[(v >> 18) & 63]; *out++ = T[(v >> 12) & 63];
        *out++ = T[(v >> 6) & 63];  *out++ = T[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = in[i] << 16;
        *out++ = T[(v >> 18) & 63]; *out++ = T[(v >> 12) & 63];
        *out++ = '='; *out++ = '=';
    } else if (n - i == 2) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        *out++ = T[(v >> 18) & 63]; *out++ = T[(v >> 12) & 63];
        *out++ = T[(v >> 6) & 63];  *out++ = '=';
    }
    return out;
}

/* Emit one RGB frame as a kitty graphics image placed in a cols x rows cell
 * box at the cursor. Chunked into 4096-char base64 units as the protocol
 * requires; id toggles 1<->2 so we can free the previous frame's memory. */
static char *emit_kitty(char *o, const uint8_t *rgb, int W, int H,
                        int cols, int rows, int id, int prev, char *b64)
{
    char *end = b64_encode(rgb, (size_t)W * H * 3, b64);
    size_t total = (size_t)(end - b64);
    size_t pos = 0;
    int first = 1;
    while (pos < total) {
        size_t chunk = total - pos;
        if (chunk > 4096) chunk = 4096;
        int more = (pos + chunk < total) ? 1 : 0;
        if (first) {
            o += sprintf(o, "\x1b_Gf=24,s=%d,v=%d,a=T,c=%d,r=%d,i=%d,q=2,m=%d;",
                         W, H, cols, rows, id, more);
            first = 0;
        } else {
            o += sprintf(o, "\x1b_Gm=%d;", more);
        }
        memcpy(o, b64 + pos, chunk); o += chunk;
        o += sprintf(o, "\x1b\\");
        pos += chunk;
    }
    if (prev)                                 /* free the frame we just hid */
        o += sprintf(o, "\x1b_Ga=d,d=I,i=%d,q=2\x1b\\", prev);
    return o;
}

/* Emit one RGBA frame as ANSI truecolour half-blocks: fg = top pixel,
 * bg = bottom pixel of each vertically-stacked pair. */
static char *emit_blocks(char *o, const uint8_t *fb, int W, int rows)
{
    o += sprintf(o, "\x1b[H");
    for (int y = 0; y < rows; y++) {
        const uint8_t *top = fb + (size_t)(2 * y)     * W * 4;
        const uint8_t *bot = fb + (size_t)(2 * y + 1) * W * 4;
        for (int x = 0; x < W; x++) {
            const uint8_t *tp = top + x * 4, *bp = bot + x * 4;
            o += sprintf(o, "\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um\xe2\x96\x80",
                         tp[0], tp[1], tp[2], bp[0], bp[1], bp[2]);
        }
        o += sprintf(o, "\x1b[0m\r\n");
    }
    return o;
}

int main(int argc, char **argv)
{
    uint32_t seed = 23;
    int blocks = 0, force_kitty = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--blocks")) blocks = 1;
        else if (!strcmp(argv[i], "--kitty"))  force_kitty = 1;
        else { printf("usage: cpore_play [--seed N] [--blocks] [--kitty]\n"); return 1; }
    }

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int TC = ws.ws_col ? ws.ws_col : 100;
    int TR = ws.ws_row ? ws.ws_row : 40;
    int have_pixels = (ws.ws_xpixel != 0 && ws.ws_ypixel != 0);
    /* assume a typical cell if the terminal reports columns but not pixels */
    if (ws.ws_xpixel == 0) ws.ws_xpixel = (unsigned short)(TC * 8);
    if (ws.ws_ypixel == 0) ws.ws_ypixel = (unsigned short)(TR * 16);
    /* kitty needs the pixel geometry to size the frame; if the terminal does
     * not report it (many VTE builds), inline images usually will not work
     * either, so fall back to half-blocks unless --kitty forces it. */
    if (!blocks && !force_kitty && !have_pixels) {
        fprintf(stderr,
            "this terminal did not report a pixel size, so it likely cannot show\n"
            "inline images. Run inside WezTerm/kitty, or use --blocks. Falling back.\n");
        sleep_ms(1500);
        blocks = 1;
    }

    int cols = TC, rows = blocks ? TR - 3 : TR - 2;
    if (blocks) { if (cols > 220) cols = 220; if (rows > 90) rows = 90; }
    if (cols < 20 || rows < 10) { fprintf(stderr, "terminal too small\n"); return 1; }

    /* render resolution */
    int W, H;
    if (blocks) {
        W = cols; H = rows * 2;
    } else {
        int cellw = ws.ws_xpixel / TC, cellh = ws.ws_ypixel / TR;
        W = cols * cellw; H = rows * cellh;
        if (W > 1000) { H = H * 1000 / W; W = 1000; }   /* cap payload; kitty scales to fit */
        if (W < 16 || H < 16) { W = 640; H = 360; }
    }

    uint8_t *fb  = (uint8_t *)malloc((size_t)W * H * 4);
    uint8_t *rgb = blocks ? NULL : (uint8_t *)malloc((size_t)W * H * 3);
    char *b64    = blocks ? NULL : (char *)malloc((size_t)W * H * 3 * 4 / 3 + 8);
    CpWorld *w   = (CpWorld *)malloc(sizeof(CpWorld));
    size_t outcap = blocks
        ? (size_t)cols * rows * 48 + rows * 8 + 4096
        : (size_t)W * H * 3 * 4 / 3 + ((size_t)W * H * 3 / 4096 + 4) * 32 + 4096;
    char *out = (char *)malloc(outcap);
    if (!fb || !w || !out || (!blocks && (!rgb || !b64))) {
        fprintf(stderr, "oom\n"); return 1;
    }

    cp_world_reset(w, seed, NULL);
    raw_mode();

    float vx = 0.0f, vy = 0.0f;
    int running = 1, paused = 0;
    int id = 1, prev = 0;

    while (running) {
        int burst = 0, zap = 0;

        unsigned char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = buf[i];
            if (c == 0x1b && i + 2 < n && buf[i + 1] == '[') {
                switch (buf[i + 2]) {
                    case 'A': vy = -1.0f; break;
                    case 'B': vy =  1.0f; break;
                    case 'C': vx =  1.0f; break;
                    case 'D': vx = -1.0f; break;
                }
                i += 2;
                continue;
            }
            switch (c) {
                case 'w': case 'W': vy = -1.0f; break;
                case 's': case 'S': vy =  1.0f; break;
                case 'a': case 'A': vx = -1.0f; break;
                case 'd': case 'D': vx =  1.0f; break;
                case ' ':           burst = 1;  break;
                case 'j': case 'J':
                case 'k': case 'K': zap = 1;    break;
                case 'p': case 'P': paused = !paused; break;
                case 'q': case 'Q': running = 0; break;
            }
        }

        if (!paused && w->status == CP_RUN) {
            float act[CP_ACT_DIM];
            memset(act, 0, sizeof(act));
            act[0] = vx; act[1] = vy;
            act[2] = burst ? 1.0f : 0.0f;
            act[3] = zap   ? 1.0f : 0.0f;
            cp_world_step(w, act);
            vx *= 0.80f; vy *= 0.80f;
        }

        cp_render_styled(w, fb, W, H, CP_VIS_ABYSS);

        char *o = out;
        if (blocks) {
            o = emit_blocks(o, fb, W, rows);
        } else {
            for (int i = 0, k = 0; i < W * H; i++, k += 3) {
                rgb[k]   = fb[i * 4];
                rgb[k+1] = fb[i * 4 + 1];
                rgb[k+2] = fb[i * 4 + 2];
            }
            o += sprintf(o, "\x1b[H");
            o = emit_kitty(o, rgb, W, H, cols, rows, id, prev, b64);
            prev = id; id = (id == 1) ? 2 : 1;
            o += sprintf(o, "\x1b[%d;1H", rows + 1);   /* status below the image */
        }

        static const char *st[] = { "ALIVE", "DIED", "EVOLVED!", "TIMEOUT" };
        o += sprintf(o,
            "\x1b[0m\x1b[K gen %d/%d  dna %.0f/%.0f  hp %.0f/%.0f  eat %d  meat %d  "
            "kills %d  zaps %d  [%s]%s\r\n"
            "\x1b[K WASD/arrows steer  SPACE burst  J zap  P pause  Q quit",
            w->generation + 1, CP_GENERATIONS, (double)w->dna, (double)CP_DNA_GOAL,
            (double)w->player.hp, (double)w->player.hp_max,
            w->ate_plant, w->ate_meat, w->kills, w->discharges,
            st[w->status], paused ? " PAUSED" : "");
        fwrite(out, 1, (size_t)(o - out), stdout);
        fflush(stdout);

        sleep_ms(55);
    }

    restore_terminal();
    static const char *st[] = { "still alive", "died", "EVOLVED to the next stage",
                                "ran out of time" };
    printf("seed=%u  final: gen %d, dna %.0f/%.0f, %s\n",
           seed, w->generation + 1, (double)w->dna, (double)CP_DNA_GOAL, st[w->status]);
    free(fb); free(rgb); free(b64); free(w); free(out);
    return 0;
}
