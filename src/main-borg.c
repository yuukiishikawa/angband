/**
 * \file main-borg.c
 * \brief Headless frontend for automated borg play.
 *
 * Provides a dummy 80x24 terminal that requires no real TTY.
 * The borg is activated directly from play_game() via
 * borg_headless_activate(). During borg initialization, any
 * message prompts are dismissed by injecting SPACE keys.
 *
 * Remote mode: when --remote host:port is passed after --, the borg
 * connects to a TS Angband TCP server instead of running the C game.
 * Screen data is received via the FRAME protocol, and keypresses are
 * sent back via KEY messages.
 *
 * Usage:
 *   angband -mborg -n [-uName]
 *   angband -mborg -n -- --remote localhost:9876
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband License":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "main.h"
#include "ui-game.h"
#include "ui-term.h"
#include "init.h"           /* z_info */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

#ifdef USE_BORG_FRONTEND

/* Flag indicating headless borg mode is active */
bool borg_headless = false;

/*
 * Phase tracking for key injection:
 * false = init phase (inject SPACE to dismiss prompts)
 * true  = borg active (no injection, inkey_hack handles input)
 */
bool borg_headless_ready = false;

/*
 * Remote mode: connect to TS Angband TCP server instead of running
 * the C game engine. The borg AI still runs locally.
 */
bool borg_remote = false;
int  borg_remote_sock = -1;

/* Line buffer for reading from the TCP socket */
static char remote_line_buf[4096];
static int  remote_line_pos = 0;

/*
 * Dummy terminal data
 */
typedef struct term_data {
    term t;
} term_data;

static term_data td;

/*
 * Terminal hooks — minimal stubs for headless operation
 */
static void term_init_borg(term *t) { }
static void term_nuke_borg(term *t) { }

static errr term_xtra_clear_borg(int v) { return 0; }
static errr term_xtra_fresh_borg(int v) { return 0; }
static errr term_xtra_noise_borg(int v) { return 0; }
static errr term_xtra_shape_borg(int v) { return 0; }
static errr term_xtra_alive_borg(int v) { return 0; }
static errr term_xtra_flush_borg(int v) { return 0; }
static errr term_xtra_react_borg(int v) { return 0; }

/* Skip all delays for maximum speed */
static errr term_xtra_delay_borg(int v) { return 0; }

/*
 * Event handler — inject SPACE to dismiss message prompts
 * during borg initialization. After borg is active, do nothing
 * (inkey_hack handles all input, and Term_inkey with wait=false
 * must find no keys to avoid triggering "user abort").
 */
static errr term_xtra_event_borg(int v)
{
    if (!borg_headless_ready) {
        /* Init phase: inject SPACE to dismiss "-- more --"
         * and message flush prompts */
        Term_keypress(' ', 0);
    }
    /* After borg is active: do nothing. The borg's inkey_hack
     * handles all input. Injecting keys here would be picked up
     * by the user-abort check and terminate the borg. */
    return 0;
}

typedef struct {
    int key;
    errr (*func)(int v);
} term_xtra_func;

static term_xtra_func xtras[] = {
    { TERM_XTRA_CLEAR, term_xtra_clear_borg },
    { TERM_XTRA_NOISE, term_xtra_noise_borg },
    { TERM_XTRA_FRESH, term_xtra_fresh_borg },
    { TERM_XTRA_SHAPE, term_xtra_shape_borg },
    { TERM_XTRA_ALIVE, term_xtra_alive_borg },
    { TERM_XTRA_EVENT, term_xtra_event_borg },
    { TERM_XTRA_FLUSH, term_xtra_flush_borg },
    { TERM_XTRA_DELAY, term_xtra_delay_borg },
    { TERM_XTRA_REACT, term_xtra_react_borg },
    { 0, NULL },
};

static errr term_xtra_borg(int n, int v)
{
    int i;
    for (i = 0; xtras[i].func; i++) {
        if (xtras[i].key == n) {
            return xtras[i].func(v);
        }
    }
    return 0;
}

static errr term_curs_borg(int x, int y) { return 0; }
static errr term_wipe_borg(int x, int y, int n) { return 0; }

static errr term_text_borg(int x, int y, int n, int a, const wchar_t *s)
{
    return 0;
}

/*
 * Initialize the dummy terminal
 */
static void term_data_link(int i)
{
    term *t = &td.t;

    term_init(t, 80, 24, 256);

    t->init_hook = term_init_borg;
    t->nuke_hook = term_nuke_borg;
    t->xtra_hook = term_xtra_borg;
    t->curs_hook = term_curs_borg;
    t->wipe_hook = term_wipe_borg;
    t->text_hook = term_text_borg;

    t->data = &td;

    Term_activate(t);

    angband_term[i] = t;
}

/* ── TCP helper functions for remote mode ── */

/**
 * Connect to a remote TS Angband server.
 * Returns socket fd on success, -1 on failure.
 */
static int connect_to_ts(const char *host, int port)
{
    struct sockaddr_in addr;
    struct hostent *he;
    int sock;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Remote borg: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    /* Try numeric address first */
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        /* Fall back to DNS lookup */
        he = gethostbyname(host);
        if (!he) {
            fprintf(stderr, "Remote borg: cannot resolve host '%s'\n", host);
            close(sock);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Remote borg: connect(%s:%d) failed: %s\n",
                host, port, strerror(errno));
        close(sock);
        return -1;
    }

    fprintf(stderr, "Remote borg: connected to %s:%d\n", host, port);
    return sock;
}

/**
 * Read one line from the TCP socket (blocking).
 * Returns the line (without newline) in a static buffer, or NULL on EOF/error.
 */
static char *recv_line(int sock)
{
    static char result[4096];

    while (1) {
        /* Check if we already have a complete line in the buffer */
        char *nl = memchr(remote_line_buf, '\n', remote_line_pos);
        if (nl) {
            int len = nl - remote_line_buf;
            if (len > 0 && remote_line_buf[len - 1] == '\r')
                len--;
            memcpy(result, remote_line_buf, len);
            result[len] = '\0';

            /* Shift remaining data */
            int consumed = (nl - remote_line_buf) + 1;
            remote_line_pos -= consumed;
            if (remote_line_pos > 0)
                memmove(remote_line_buf, nl + 1, remote_line_pos);

            return result;
        }

        /* Need more data */
        if (remote_line_pos >= (int)sizeof(remote_line_buf) - 1) {
            fprintf(stderr, "Remote borg: line buffer overflow\n");
            return NULL;
        }

        ssize_t n = recv(sock, remote_line_buf + remote_line_pos,
                         sizeof(remote_line_buf) - remote_line_pos - 1, 0);
        if (n <= 0) {
            if (n == 0)
                fprintf(stderr, "Remote borg: server disconnected\n");
            else
                fprintf(stderr, "Remote borg: recv() error: %s\n", strerror(errno));
            return NULL;
        }
        remote_line_pos += n;
    }
}

/**
 * Send a KEY message to the TS server.
 */
void borg_remote_send_key(int sock, keycode_t key, int mods)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "KEY %d %d\n", (int)key, mods);
    send(sock, buf, strlen(buf), 0);
}

/**
 * Decode a hex character pair to a byte value.
 */
static int hex_to_byte(char hi, char lo)
{
    int val = 0;
    if (hi >= '0' && hi <= '9') val = (hi - '0') << 4;
    else if (hi >= 'a' && hi <= 'f') val = (hi - 'a' + 10) << 4;
    else if (hi >= 'A' && hi <= 'F') val = (hi - 'A' + 10) << 4;
    if (lo >= '0' && lo <= '9') val |= (lo - '0');
    else if (lo >= 'a' && lo <= 'f') val |= (lo - 'a' + 10);
    else if (lo >= 'A' && lo <= 'F') val |= (lo - 'A' + 10);
    return val;
}

/**
 * Decode a single hex digit.
 */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/**
 * Parse a STAT line and update the player structure.
 *
 * Format: "STAT hp=15 mhp=15 sp=0 msp=0 lev=1 depth=0 speed=110 dead=0"
 * or:     "STAT str=16 int=8 wis=10 dex=14 con=15"
 */
static void parse_stat_line(const char *line)
{
    /* Skip "STAT " prefix */
    const char *p = line + 5;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ') p++;
        if (!*p) break;

        /* Parse key=value pairs */
        char key[32];
        int val;
        int i = 0;
        while (*p && *p != '=' && i < 31)
            key[i++] = *p++;
        key[i] = '\0';
        if (*p == '=') p++;
        val = atoi(p);
        while (*p && *p != ' ') p++;

        /* Apply to player struct */
        if (streq(key, "hp"))    player->chp = val;
        else if (streq(key, "mhp"))   player->mhp = val;
        else if (streq(key, "sp"))    player->csp = val;
        else if (streq(key, "msp"))   player->msp = val;
        else if (streq(key, "lev"))   player->lev = val;
        else if (streq(key, "depth")) player->depth = val;
        else if (streq(key, "speed")) player->state.speed = val;
        else if (streq(key, "dead"))  player->is_dead = (val != 0);
        else if (streq(key, "str"))   player->stat_cur[0] = val;
        else if (streq(key, "int"))   player->stat_cur[1] = val;
        else if (streq(key, "wis"))   player->stat_cur[2] = val;
        else if (streq(key, "dex"))   player->stat_cur[3] = val;
        else if (streq(key, "con"))   player->stat_cur[4] = val;
        else if (streq(key, "wx") && Term)  Term->offset_x = val;
        else if (streq(key, "wy") && Term)  Term->offset_y = val;
        else if (streq(key, "px"))   player->grid.x = val;
        else if (streq(key, "py"))   player->grid.y = val;
    }
}

/**
 * Remote inventory data buffer.
 * Populated from INVEN protocol lines in recv_screen_frame().
 * Read by borg_cheat_inven() in borg-inventory.c when borg_remote is true.
 */
#define REMOTE_INVEN_MAX 40

struct remote_inven_entry {
    int slot;
    int tval, sval, qty;
    int to_h, to_d, to_a;
    int dd, ds, ac, weight;
    char name[80];
    bool valid;
};

/* Global buffer shared with borg-inventory.c */
struct remote_inven_entry borg_remote_inven[REMOTE_INVEN_MAX];
int borg_remote_inven_count = 0;

static void parse_inven_line(const char *line)
{
    if (borg_remote_inven_count >= REMOTE_INVEN_MAX) return;

    struct remote_inven_entry *e = &borg_remote_inven[borg_remote_inven_count];
    memset(e, 0, sizeof(*e));

    if (sscanf(line, "INVEN %d %d %d %d %d %d %d %d %d %d %d %79s",
               &e->slot, &e->tval, &e->sval, &e->qty,
               &e->to_h, &e->to_d, &e->to_a,
               &e->dd, &e->ds, &e->ac, &e->weight, e->name) < 12)
        return;

    /* Replace underscores with spaces in name */
    for (int i = 0; e->name[i]; i++)
        if (e->name[i] == '_') e->name[i] = ' ';

    e->valid = true;
    borg_remote_inven_count++;
}

/**
 * Receive a complete FRAME from the TS server and populate Term->scr.
 *
 * Protocol:
 *   FRAME
 *   ROW <y> <hex_chars> <hex_attrs>
 *   ... (24 rows)
 *   CURSOR <x> <y>
 *   STAT key=val key=val ...
 *   INVEN <slot> <tval> <sval> <qty> ...
 *   END
 *
 * Returns 0 on success, -1 on error/disconnect.
 */
int recv_screen_frame(int sock)
{
    char *line;

    /* Read until we get FRAME */
    while (1) {
        line = recv_line(sock);
        if (!line) return -1;

        if (streq(line, "FRAME"))
            break;

        /* Handle DEAD message */
        if (prefix(line, "DEAD")) {
            player->is_dead = true;
            return -1;
        }
    }

    /* Clear INVEN buffer for this frame */
    borg_remote_inven_count = 0;

    /* Read ROW, CURSOR, STAT, INVEN, END lines */
    while (1) {
        line = recv_line(sock);
        if (!line) return -1;

        if (streq(line, "END"))
            break;

        if (prefix(line, "ROW ")) {
            /* Parse: ROW <y> <hex_chars> <hex_attrs> */
            int y;
            char hex_chars[256];
            char hex_attrs[128];

            if (sscanf(line, "ROW %d %240s %120s", &y, hex_chars, hex_attrs) != 3)
                continue;
            if (y < 0 || y >= 24) continue;

            /* Ensure Term->scr exists */
            if (!Term || !Term->scr) continue;

            int cols = strlen(hex_chars) / 2;
            if (cols > 80) cols = 80;

            for (int x = 0; x < cols; x++) {
                int ch = hex_to_byte(hex_chars[x * 2], hex_chars[x * 2 + 1]);
                int attr = (x < (int)strlen(hex_attrs)) ? hex_digit(hex_attrs[x]) : 1;

                Term->scr->c[y][x] = (wchar_t)ch;
                Term->scr->a[y][x] = attr;
            }
        }
        else if (prefix(line, "CURSOR ")) {
            int cx, cy;
            if (sscanf(line, "CURSOR %d %d", &cx, &cy) == 2) {
                if (Term && Term->scr) {
                    Term->scr->cx = cx;
                    Term->scr->cy = cy;
                }
            }
        }
        else if (prefix(line, "STAT ")) {
            parse_stat_line(line);
        }
        else if (prefix(line, "INVEN ")) {
            parse_inven_line(line);
        }
    }

    return 0;
}

const char help_borg[] = "Headless borg mode, no subopts";

errr init_borg_mode(int argc, char *argv[])
{
    /* Set headless flag */
    borg_headless = true;

    /* Use a borg-specific savefile name so panic saves work
     * (empty savefile would overwrite the panic directory itself) */
    savefile_set_name("borg", false, false);

    /* Set up the dummy terminal */
    term_data_link(0);

    /* Check for --remote flag in argv (after --) */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--remote", 8) == 0) {
            const char *hostport;
            char host[256];
            int port = 9876;

            /* --remote host:port or --remote=host:port */
            if (argv[i][8] == '=') {
                hostport = argv[i] + 9;
            } else if (i + 1 < argc) {
                hostport = argv[++i];
            } else {
                fprintf(stderr, "Remote borg: --remote requires host:port argument\n");
                return 1;
            }

            /* Parse host:port */
            const char *colon = strchr(hostport, ':');
            if (colon) {
                int hlen = colon - hostport;
                if (hlen > 255) hlen = 255;
                strncpy(host, hostport, hlen);
                host[hlen] = '\0';
                port = atoi(colon + 1);
            } else {
                strncpy(host, hostport, 255);
                host[255] = '\0';
            }

            borg_remote_sock = connect_to_ts(host, port);
            if (borg_remote_sock < 0) {
                return 1;
            }
            borg_remote = true;
            /* Set sidebar_mode to SIDEBAR_NONE so COL_MAP=0, ROW_MAP=1
             * matching the TS screen renderer layout */
            td.t.sidebar_mode = SIDEBAR_NONE;
            fprintf(stderr, "Remote borg: remote mode enabled (%s:%d)\n", host, port);
            break;
        }
    }

    return 0;
}

#else /* USE_BORG_FRONTEND */

/* Provide stub when not compiled */
bool borg_headless = false;

#endif /* USE_BORG_FRONTEND */
