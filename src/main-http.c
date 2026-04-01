/**
 * \file main-http.c
 * \brief HTTP server frontend for AI-controlled Angband play.
 *
 * Provides a headless 80x24 terminal with an embedded HTTP server.
 * GET /state returns game state as JSON.
 * POST /command accepts {type, direction, itemIndex, ...} to inject commands.
 *
 * Usage:
 *   angband -mhttp [-uName] [-- --port 3000 --seed 123]
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *   a) the GNU General Public License as published by the Free Software
 *      Foundation, version 2, or
 *   b) the "Angband License"
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "angband.h"
#include "main.h"
#include "ui-game.h"
#include "ui-term.h"
#include "init.h"
#include "cave.h"
#include "cmd-core.h"
#include "game-world.h"
#include "mon-predicate.h"
#include "mon-util.h"
#include "monster.h"
#include "obj-desc.h"
#include "obj-gear.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "player.h"
#include "player-calcs.h"
#include "player-timed.h"
#include "z-rand.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* ── Configuration ── */
static int http_port = 3000;
static bool http_running = false;
static int http_server_fd = -1;

/* Global flag checked by ui-game.c play_game() */
bool http_headless = false;

/* Forward declarations */
static void handle_get_state(int fd);
static void send_response(int fd, int code, const char *content_type, const char *body, size_t body_len);
static int json_get_int(const char *json, const char *key, int def);
static cmd_code ts_to_c_cmd(int ts_type);

/* ── Dummy Terminal (identical to main-borg.c pattern) ── */

typedef struct { term t; } term_data_http;
static term_data_http td_http;

static void term_init_http(term *t) { }
static void term_nuke_http(term *t) { }
static bool http_init_done = false;

static errr term_xtra_event_http(int v)
{
    if (!http_init_done) {
        /* Same as borg: inject SPACE to dismiss prompts during init */
        Term_keypress(' ', 0);
    }
    /* v=1 means "wait for event". In HTTP mode we don't need to wait
     * since input comes via cmd_get_hook. Return 1 to indicate no event. */
    return 1;
}

static errr term_xtra_clear_http(int v) { return 0; }
static errr term_xtra_fresh_http(int v) {
    /* Send pending HTTP response after command execution */
    extern int http_pending_fd;
    if (http_pending_fd >= 0) {
        handle_get_state(http_pending_fd);
        close(http_pending_fd);
        http_pending_fd = -1;
    }
    /* Prevent CPU spinning during idle refresh cycles */
    if (http_init_done) {
        usleep(1000); /* 1ms sleep to prevent busy loop */
    }
    return 0;
}
static errr term_xtra_noise_http(int v) { return 0; }
static errr term_xtra_shape_http(int v) { return 0; }
static errr term_xtra_alive_http(int v) { return 0; }
static errr term_xtra_flush_http(int v) { return 0; }
static errr term_xtra_delay_http(int v) { return 0; }
static errr term_xtra_react_http(int v) { return 0; }

typedef struct { int key; errr (*func)(int v); } xtra_func;
static xtra_func xtras_http[] = {
    { TERM_XTRA_CLEAR, term_xtra_clear_http },
    { TERM_XTRA_NOISE, term_xtra_noise_http },
    { TERM_XTRA_FRESH, term_xtra_fresh_http },
    { TERM_XTRA_SHAPE, term_xtra_shape_http },
    { TERM_XTRA_ALIVE, term_xtra_alive_http },
    { TERM_XTRA_EVENT, term_xtra_event_http },
    { TERM_XTRA_FLUSH, term_xtra_flush_http },
    { TERM_XTRA_DELAY, term_xtra_delay_http },
    { TERM_XTRA_REACT, term_xtra_react_http },
    { 0, NULL },
};

static errr term_xtra_http(int n, int v)
{
    for (int i = 0; xtras_http[i].func; i++) {
        if (xtras_http[i].key == n) {
            return xtras_http[i].func(v);
        }
    }
    return 0;
}
static errr term_curs_http(int x, int y) { return 0; }
static errr term_wipe_http(int x, int y, int n) { return 0; }
static errr term_text_http(int x, int y, int n, int a, const wchar_t *s) { return 0; }

static void term_data_link_http(void)
{
    term *t = &td_http.t;
    term_init(t, 80, 24, 256);
    t->init_hook = term_init_http;
    t->nuke_hook = term_nuke_http;
    t->xtra_hook = term_xtra_http;
    t->curs_hook = term_curs_http;
    t->wipe_hook = term_wipe_http;
    t->text_hook = term_text_http;
    t->data = &td_http;
    Term_activate(t);
    angband_term[0] = t;
}

/* ── JSON Serialization Helpers ── */

static void json_str(char *buf, size_t sz, size_t *pos, const char *key, const char *val) {
    *pos += snprintf(buf + *pos, sz - *pos, "\"%s\":\"%s\",", key, val);
}
static void json_int(char *buf, size_t sz, size_t *pos, const char *key, int val) {
    *pos += snprintf(buf + *pos, sz - *pos, "\"%s\":%d,", key, val);
}
static void json_bool(char *buf, size_t sz, size_t *pos, const char *key, bool val) {
    *pos += snprintf(buf + *pos, sz - *pos, "\"%s\":%s,", key, val ? "true" : "false");
}

/* ── Serialize Game State ── */

static size_t serialize_state(char *buf, size_t sz)
{
    size_t pos = 0;
    struct player *p = player;

    pos += snprintf(buf + pos, sz - pos, "{");

    /* Player basics */
    pos += snprintf(buf + pos, sz - pos, "\"player\":{");
    json_str(buf, sz, &pos, "race", p->race ? p->race->name : "?");
    json_str(buf, sz, &pos, "class", p->class ? p->class->name : "?");
    json_int(buf, sz, &pos, "level", p->lev);
    json_int(buf, sz, &pos, "hp", p->chp);
    json_int(buf, sz, &pos, "maxHp", p->mhp);
    json_int(buf, sz, &pos, "sp", p->csp);
    json_int(buf, sz, &pos, "maxSp", p->msp);
    json_int(buf, sz, &pos, "exp", p->exp);
    json_int(buf, sz, &pos, "gold", p->au);
    json_int(buf, sz, &pos, "x", p->grid.x);
    json_int(buf, sz, &pos, "y", p->grid.y);
    json_int(buf, sz, &pos, "depth", p->depth);
    json_int(buf, sz, &pos, "maxDepth", p->max_depth);
    json_int(buf, sz, &pos, "food", p->food);
    json_int(buf, sz, &pos, "speed", p->state.speed);
    json_int(buf, sz, &pos, "ac", p->state.ac + p->state.to_a);
    json_int(buf, sz, &pos, "toH", p->state.to_h);
    json_int(buf, sz, &pos, "toD", p->state.to_d);
    json_int(buf, sz, &pos, "numBlows", p->state.num_blows);
    json_bool(buf, sz, &pos, "dead", p->is_dead);
    json_int(buf, sz, &pos, "wordRecall", p->word_recall);

    /* Stats */
    pos += snprintf(buf + pos, sz - pos, "\"stats\":{");
    json_int(buf, sz, &pos, "str", p->state.stat_use[STAT_STR]);
    json_int(buf, sz, &pos, "int", p->state.stat_use[STAT_INT]);
    json_int(buf, sz, &pos, "wis", p->state.stat_use[STAT_WIS]);
    json_int(buf, sz, &pos, "dex", p->state.stat_use[STAT_DEX]);
    json_int(buf, sz, &pos, "con", p->state.stat_use[STAT_CON]);
    /* Remove trailing comma */
    if (buf[pos-1] == ',') pos--;
    pos += snprintf(buf + pos, sz - pos, "},");

    /* Timed effects */
    pos += snprintf(buf + pos, sz - pos, "\"timed\":{");
    if (p->timed) {
        json_int(buf, sz, &pos, "fast", p->timed[TMD_FAST]);
        json_int(buf, sz, &pos, "slow", p->timed[TMD_SLOW]);
        json_int(buf, sz, &pos, "blind", p->timed[TMD_BLIND]);
        json_int(buf, sz, &pos, "confused", p->timed[TMD_CONFUSED]);
        json_int(buf, sz, &pos, "poisoned", p->timed[TMD_POISONED]);
        json_int(buf, sz, &pos, "afraid", p->timed[TMD_AFRAID]);
        json_int(buf, sz, &pos, "paralyzed", p->timed[TMD_PARALYZED]);
        json_int(buf, sz, &pos, "cut", p->timed[TMD_CUT]);
        json_int(buf, sz, &pos, "stun", p->timed[TMD_STUN]);
    }
    if (buf[pos-1] == ',') pos--;
    pos += snprintf(buf + pos, sz - pos, "},");

    /* Inventory (truncated to avoid buffer overflow) */
    pos += snprintf(buf + pos, sz - pos, "\"inventory\":[");
    {
        int slot = 0;
        for (struct object *obj = p->gear; obj && slot < 40 && pos < sz - 200; obj = obj->next) {
            if (object_is_equipped(p->body, obj)) continue; /* skip equipped */
            char name[80];
            object_desc(name, sizeof(name), obj, ODESC_PREFIX | ODESC_FULL, p);
            pos += snprintf(buf + pos, sz - pos,
                "{\"slot\":%d,\"name\":\"%s\",\"tval\":%d,\"sval\":%d,\"qty\":%d},",
                slot++, name, obj->tval, obj->sval, obj->number);
        }
        if (buf[pos-1] == ',') pos--;
    }
    pos += snprintf(buf + pos, sz - pos, "],");

    /* Equipment */
    pos += snprintf(buf + pos, sz - pos, "\"equipment\":[");
    {
        for (int i = 0; i < p->body.count && pos < sz - 200; i++) {
            struct object *obj = p->body.slots[i].obj;
            if (!obj) continue;
            char name[80];
            object_desc(name, sizeof(name), obj, ODESC_PREFIX | ODESC_FULL, p);
            pos += snprintf(buf + pos, sz - pos,
                "{\"slot\":%d,\"name\":\"%s\",\"tval\":%d,\"weight\":%d,\"ac\":%d,\"toH\":%d,\"toD\":%d,\"toA\":%d},",
                i, name, obj->tval, obj->weight, obj->ac, obj->to_h, obj->to_d, obj->to_a);
        }
        if (buf[pos-1] == ',') pos--;
    }
    pos += snprintf(buf + pos, sz - pos, "],");

    if (buf[pos-1] == ',') pos--;
    pos += snprintf(buf + pos, sz - pos, "},"); /* end player */

    /* Dungeon depth + turn */
    json_int(buf, sz, &pos, "depth", p->depth);
    json_int(buf, sz, &pos, "turn", turn);
    json_bool(buf, sz, &pos, "dead", p->is_dead);

    /* Visible monsters */
    pos += snprintf(buf + pos, sz - pos, "\"monsters\":[");
    if (cave) {
        for (int i = 1; i < cave->mon_max && pos < sz - 200; i++) {
            struct monster *mon = cave_monster(cave, i);
            if (!mon || !mon->race) continue;
            if (!monster_is_visible(mon)) continue;
            int dist = distance(p->grid, mon->grid);
            pos += snprintf(buf + pos, sz - pos,
                "{\"name\":\"%s\",\"x\":%d,\"y\":%d,\"hp\":%d,\"maxhp\":%d,"
                "\"speed\":%d,\"level\":%d,\"distance\":%d,\"isUnique\":%s},",
                mon->race->name, mon->grid.x, mon->grid.y,
                mon->hp, mon->maxhp, mon->mspeed, mon->race->level, dist,
                rf_has(mon->race->flags, RF_UNIQUE) ? "true" : "false");
        }
        if (buf[pos-1] == ',') pos--;
    }
    pos += snprintf(buf + pos, sz - pos, "],");

    /* Map tiles (visible only) */
    pos += snprintf(buf + pos, sz - pos, "\"map\":{\"tiles\":[");
    if (cave) {
        int count = 0;
        for (int y = 0; y < cave->height && pos < sz - 100; y++) {
            for (int x = 0; x < cave->width && pos < sz - 100; x++) {
                struct loc grid = loc(x, y);
                if (!square_isknown(cave, grid)) continue;
                int feat = square(cave, grid)->feat;
                bool has_obj = square_object(cave, grid) != NULL;
                pos += snprintf(buf + pos, sz - pos,
                    "{\"x\":%d,\"y\":%d,\"feat\":%d,\"hasObj\":%s},",
                    x, y, feat, has_obj ? "true" : "false");
                if (++count > 5000) goto done_tiles; /* safety limit */
            }
        }
    }
done_tiles:
    if (buf[pos-1] == ',') pos--;
    pos += snprintf(buf + pos, sz - pos, "]}");

    if (buf[pos-1] == ',') pos--;
    pos += snprintf(buf + pos, sz - pos, "}");

    return pos;
}

/* ── HTTP Server ── */

static void send_response(int fd, int code, const char *content_type, const char *body, size_t body_len)
{
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, content_type, body_len);
    send(fd, header, hlen, 0);
    if (body_len > 0) send(fd, body, body_len, 0);
}

static void handle_get_state(int fd)
{
    static char buf[512 * 1024]; /* 512KB buffer */
    size_t len = serialize_state(buf, sizeof(buf));
    send_response(fd, 200, "application/json", buf, len);
}

/* Simple JSON number parser */
static int json_get_int(const char *json, const char *key, int def)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoi(p);
}

/* HTTP command hook: replaces cmd_get_hook.
 * Blocks waiting for an HTTP request, then injects the command.
 * This runs inside the normal game loop (play_game → cmd_get_hook → run_game_loop). */
static errr http_cmd_get(cmd_context ctx) {
    (void)ctx;
    if (http_server_fd < 0) return 1;

    /* Send the current state as response to the previous request if pending */
    /* (handled in http_handle_and_inject) */

    /* Wait for next HTTP request (blocks) */
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(http_server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) return 1;

    /* Read request */
    char buf[8192];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(client_fd); return 0; }
    buf[n] = '\0';

    char method[8], path[256];
    sscanf(buf, "%7s %255s", method, path);
    const char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;

    if (strcmp(method, "GET") == 0 && strcmp(path, "/state") == 0) {
        handle_get_state(client_fd);
        close(client_fd);
        return 0; /* No command injected, game loop will call us again */
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/command") == 0) {
        int type = json_get_int(body ? body : "{}", "type", -1);
        int dir = json_get_int(body ? body : "{}", "direction", 5);
        int item_idx = json_get_int(body ? body : "{}", "itemIndex", -1);

        if (type >= 0) {
            struct command cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.code = ts_to_c_cmd(type);
            cmd.context = CTX_GAME;
            if (dir != 5) cmd_set_arg_direction(&cmd, "direction", dir);
            if (item_idx >= 0) {
                int i = 0;
                for (struct object *obj = player->gear; obj; obj = obj->next) {
                    if (object_is_equipped(player->body, obj)) continue;
                    if (i == item_idx) {
                        cmd_set_arg_item(&cmd, "item", obj);
                        break;
                    }
                    i++;
                }
            }
            cmdq_push_copy(&cmd);
        }

        /* Store client_fd so we can respond after the command executes */
        /* For simplicity, respond with state immediately before execution */
        /* The game loop will process the command on next iteration */

        /* We need to respond AFTER the command executes. Use a global. */
        extern int http_pending_fd;
        http_pending_fd = client_fd;
        return 0; /* Command is in queue, game loop will execute it */
    }

    /* Unknown request */
    const char *html = "<h1>C Angband HTTP</h1>";
    send_response(client_fd, 200, "text/html", html, strlen(html));
    close(client_fd);
    return 0;
}

/* Map TS Angband command codes to C Angband command codes */
static cmd_code ts_to_c_cmd(int ts_type)
{
    switch (ts_type) {
        case 10: return CMD_WALK;       /* TS CMD.WALK */
        case 11: return CMD_TUNNEL;     /* TS CMD.TUNNEL */
        case 12: return CMD_OPEN;       /* TS CMD.OPEN */
        case 3:  return CMD_CLOSE;      /* TS CMD.CLOSE */
        case 14: return CMD_READ_SCROLL;/* TS CMD.READ */
        case 15: return CMD_QUAFF;      /* TS CMD.QUAFF */
        case 16: return CMD_WIELD;      /* TS CMD.EQUIP */
        case 17: return CMD_PICKUP;     /* TS CMD.PICKUP */
        case 22: return CMD_REST;       /* TS CMD.REST */
        case 24: return CMD_GO_UP;      /* TS CMD.GO_UP */
        case 25: return CMD_GO_DOWN;    /* TS CMD.GO_DOWN */
        case 19: return CMD_EAT;        /* TS CMD.EAT */
        case 20: return CMD_CAST;       /* TS CMD.CAST */
        default: return (cmd_code)ts_type; /* passthrough */
    }
}

static void handle_post_command(int fd, const char *body)
{
    int type = json_get_int(body, "type", -1);
    int dir = json_get_int(body, "direction", 5);
    int item_idx = json_get_int(body, "itemIndex", -1);

    if (type < 0) {
        const char *err = "{\"error\":\"missing type\"}";
        send_response(fd, 400, "application/json", err, strlen(err));
        return;
    }

    /* Build command — translate TS codes to C codes */
    struct command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.code = ts_to_c_cmd(type);
    cmd.context = CTX_GAME;

    /* Set direction if provided */
    if (dir != 5) {
        cmd_set_arg_direction(&cmd, "direction", dir);
    }

    /* Set item if provided */
    if (item_idx >= 0) {
        int i = 0;
        for (struct object *obj = player->gear; obj; obj = obj->next) {
            if (object_is_equipped(player->body, obj)) continue;
            if (i == item_idx) {
                cmd_set_arg_item(&cmd, "item", obj);
                break;
            }
            i++;
        }
    }

    /* Inject command */
    cmdq_push_copy(&cmd);

    /* Process the command: run game loop until it needs more input.
     * We use the same pattern as the normal game: pre_turn_refresh + run_game_loop.
     * To prevent blocking, we set cmd_get_hook to a no-op during processing. */
    {
        /* Process the command through the full game loop.
         * The game loop calls cmd_get_hook when it needs more input.
         * We replace it with a function that signals "stop" after
         * one command is consumed, causing the loop to return. */
        {
            extern errr (*cmd_get_hook)(cmd_context);
            extern bool http_cmd_done;
            errr (*saved_hook)(cmd_context) = cmd_get_hook;
            http_cmd_done = false;
            cmd_get_hook = http_cmd_get;
            run_game_loop();
            cmd_get_hook = saved_hook;
        }
    }

    /* Return updated state */
    handle_get_state(fd);
}

static void handle_request(int fd)
{
    char buf[8192];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse method and path */
    char method[8], path[256];
    sscanf(buf, "%7s %255s", method, path);

    /* Find body (after \r\n\r\n) */
    const char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;

    if (strcmp(method, "GET") == 0 && strcmp(path, "/state") == 0) {
        handle_get_state(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/command") == 0) {
        handle_post_command(fd, body ? body : "{}");
    } else if (strcmp(method, "OPTIONS") == 0) {
        /* CORS preflight */
        const char *ok = "";
        send_response(fd, 200, "text/plain", ok, 0);
    } else {
        const char *html = "<h1>C Angband HTTP Server</h1><p>GET /state | POST /command</p>";
        send_response(fd, 200, "text/html", html, strlen(html));
    }
}

static int start_http_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 5) < 0) {
        perror("listen"); close(fd); return -1;
    }

    fprintf(stderr, "[C-Angband HTTP] Listening on port %d\n", port);
    fprintf(stderr, "[C-Angband HTTP] GET /state | POST /command\n");
    return fd;
}

/* ── Main HTTP game loop ── */

int http_pending_fd = -1;

void http_game_loop(void)
{
    http_init_done = true;
    fprintf(stderr, "[C-Angband HTTP] Character ready: %s %s CL%d\n",
        player->race ? player->race->name : "?",
        player->class ? player->class->name : "?",
        player->lev);

    http_server_fd = start_http_server(http_port);
    if (http_server_fd < 0) {
        fprintf(stderr, "Failed to start HTTP server on port %d\n", http_port);
        return;
    }

    /* Replace cmd_get_hook with our HTTP-blocking version.
     * The normal game loop (play_game's while loop) will call
     * cmd_get_hook → http_cmd_get → blocks on accept() → injects cmd.
     * Then run_game_loop() processes the command and monsters.
     * Then the loop calls cmd_get_hook again → blocks for next request. */
    {
        extern errr (*cmd_get_hook)(cmd_context);
        cmd_get_hook = http_cmd_get;
    }

    /* Now let the normal game loop run. It will:
     * 1. Call cmd_get_hook (our http_cmd_get) which blocks on accept()
     * 2. When HTTP POST /command arrives, inject cmd and return
     * 3. run_game_loop() processes the command
     * 4. After processing, we need to send the response
     * We use term_xtra_fresh to send pending responses. */

    fprintf(stderr, "[C-Angband HTTP] Entering game loop (HTTP-driven)\n");
    /* Don't return - let play_game()'s while loop take over */
}

/* ── Frontend Init ── */

const char help_http[] = "HTTP server frontend for AI play";

errr init_http(int argc, char *argv[])
{
    /* Parse args after -- */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            http_port = atoi(argv[++i]);
        }
    }

    /* Set up dummy terminal */
    term_data_link_http();

    /* Set headless flag — ui-game.c checks this in play_game() */
    http_headless = true;

    /* Force borg_auto_birth to pick Human/Warrior by setting
     * borg_remote=true temporarily. The remote-mode path in
     * borg_auto_birth searches for Human+Warrior by name. */

    fprintf(stderr, "[C-Angband HTTP] Frontend initialized, port=%d\n", http_port);

    return 0;
}
