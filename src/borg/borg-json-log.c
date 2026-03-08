/**
 * \file borg-json-log.c
 * \brief JSON logging for borg AI analysis
 *
 * Outputs BorgLogEntry[] JSON compatible with angband-ts borg logs.
 * Log entries are accumulated in memory during play, then flushed to a
 * timestamped JSON file on death or manual stop.
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
 * Copyright (c) 2007-9 Andi Sidwell, Chris Carr, Ed Graham, Erik Osheim
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

#include "borg-json-log.h"

#ifdef ALLOW_BORG

#include "../game-world.h"
#include "../init.h"
#include "../player.h"
#include "../z-virt.h"

#include "borg-cave.h"
#include "borg-cave-view.h"
#include "borg-danger.h"
#include "borg-flow-kill.h"
#include "borg-io.h"
#include "borg-trait.h"
#include "borg.h"

/* Current strategy name — set by borg_think_dungeon branches */
const char *borg_json_strategy = "Unknown";

/* Per-turn action detail — set before borg_json_log_turn() */
static int  pending_key = 0;
static char pending_msg[256] = "";

/*
 * A single log entry matching the BorgLogEntry TypeScript interface
 */
typedef struct {
    int32_t turn;
    int depth;
    int hp;
    int mhp;
    int sp;
    int msp;
    int danger;
    int monster_count;
    int gold;
    int player_level;
    float exploration;
    char strategy[32];
    int key_code;
    int goal_type;
    int food;
    int pos_x;
    int pos_y;
    char msg[128];
} borg_json_entry;

/* Dynamic array of log entries */
static borg_json_entry *log_entries;
static int log_count;
static int log_capacity;

/* Stats tracked across the run */
static int lowest_hp;

/* CSV action log file */
static FILE *action_log_fp = NULL;

/*
 * Calculate the exploration ratio for the current dungeon level.
 * Returns a float 0.0–1.0 representing the fraction of grids
 * that have been observed (BORG_MARK flag set).
 */
static float calc_exploration_ratio(void)
{
    int marked = 0;
    int total  = 0;
    int y, x;

    for (y = 1; y < AUTO_MAX_Y - 1; y++) {
        for (x = 1; x < AUTO_MAX_X - 1; x++) {
            total++;
            if (borg_grids[y][x].info & BORG_MARK)
                marked++;
        }
    }

    if (total == 0)
        return 0.0f;

    return (float)marked / (float)total;
}

/*
 * Set the key code for current turn's log entry.
 */
void borg_json_log_set_key(int key_code)
{
    pending_key = key_code;
}

/*
 * Set the game message for current turn's log entry.
 */
void borg_json_log_set_msg(const char *msg)
{
    if (msg && msg[0])
        my_strcpy(pending_msg, msg, sizeof(pending_msg));
    else
        pending_msg[0] = '\0';
}

/*
 * Open a CSV action log file for C-vs-TS comparison.
 * mode_name: "local" or "remote"
 */
void borg_action_log_open(const char *mode_name)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/borg_action_%s.csv", mode_name);
    action_log_fp = fopen(path, "w");
    if (action_log_fp)
        fprintf(action_log_fp,
            "turn,depth,lev,hp,mhp,sp,msp,kills,gold,food,goal,strategy,"
            "key,key_ch,x,y,msg\n");
}

/*
 * Write one CSV line for the current turn.
 */
void borg_action_log_write(int turn, int key_code)
{
    if (!action_log_fp)
        return;

    char ch = (key_code >= 32 && key_code < 127 && key_code != ',') ? (char)key_code : '?';
    /* Escape commas/quotes in msg */
    char safe_msg[256];
    int j = 0;
    for (int i = 0; pending_msg[i] && j < 250; i++) {
        if (pending_msg[i] == '"') {
            safe_msg[j++] = '\'';
        } else if (pending_msg[i] == ',') {
            safe_msg[j++] = ';';
        } else {
            safe_msg[j++] = pending_msg[i];
        }
    }
    safe_msg[j] = '\0';

    fprintf(action_log_fp,
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%c,%d,%d,%s\n",
        turn,
        borg.trait[BI_CDEPTH],
        borg.trait[BI_CLEVEL],
        borg.trait[BI_CURHP],
        borg.trait[BI_MAXHP],
        borg.trait[BI_CURSP],
        borg.trait[BI_MAXSP],
        borg_kills_cnt,
        borg.trait[BI_GOLD],
        borg.trait[BI_FOOD],
        borg.goal.type,
        borg_json_strategy,
        key_code, ch,
        borg.c.x, borg.c.y,
        safe_msg);
    fflush(action_log_fp);
}

/*
 * Close the CSV action log.
 */
void borg_action_log_close(void)
{
    if (action_log_fp) {
        fclose(action_log_fp);
        action_log_fp = NULL;
    }
}

/*
 * Initialize the JSON log system.
 * Called once from borg_init().
 */
void borg_json_log_init(void)
{
    log_capacity = 8192;
    log_entries  = mem_zalloc(sizeof(borg_json_entry) * log_capacity);
    log_count    = 0;
    lowest_hp    = 9999;
}

/*
 * Record one turn of borg state.
 * Called every turn from internal_borg_inkey() after borg_think().
 */
void borg_json_log_turn(void)
{
    borg_json_entry *e;

    /* Don't record if not initialized */
    if (!log_entries)
        return;

    /* Grow buffer if needed, cap at 1M entries */
    if (log_count >= log_capacity) {
        if (log_capacity >= 1000000)
            return;
        log_capacity *= 2;
        if (log_capacity > 1000000)
            log_capacity = 1000000;
        log_entries = mem_realloc(log_entries,
            sizeof(borg_json_entry) * log_capacity);
    }

    e = &log_entries[log_count++];

    e->turn          = borg_t;
    e->depth         = borg.trait[BI_CDEPTH];
    e->hp            = borg.trait[BI_CURHP];
    e->mhp           = borg.trait[BI_MAXHP];
    e->sp            = borg.trait[BI_CURSP];
    e->msp           = borg.trait[BI_MAXSP];
    e->danger        = borg_danger(borg.c.y, borg.c.x, 1, true, false);
    e->monster_count = borg_kills_cnt;
    e->gold          = borg.trait[BI_GOLD];
    e->player_level  = borg.trait[BI_CLEVEL];
    e->exploration   = calc_exploration_ratio();

    my_strcpy(e->strategy, borg_json_strategy, sizeof(e->strategy));

    /* Action details */
    e->key_code  = pending_key;
    e->goal_type = borg.goal.type;
    e->food      = borg.trait[BI_FOOD];
    e->pos_x     = borg.c.x;
    e->pos_y     = borg.c.y;
    my_strcpy(e->msg, pending_msg, sizeof(e->msg));

    /* Track lowest HP */
    if (e->hp < lowest_hp)
        lowest_hp = e->hp;

    /* Write CSV action log line before resetting state */
    borg_action_log_write((int)e->turn, e->key_code);

    /* Reset per-turn state */
    borg_json_strategy = "Unknown";
    pending_key = 0;
    pending_msg[0] = '\0';
}

/*
 * Flush all accumulated log entries to a JSON file and print a summary.
 * Called on death or borg stop.
 */
void borg_json_log_finish(void)
{
    char      filename[128];
    char      filepath[1024];
    ang_file *f;
    time_t    now;
    int       i;
    int       max_depth = 0;

    if (!log_entries || log_count == 0) {
        borg_note("# JSON log: no entries to write");
        return;
    }

    /* Build timestamp filename */
    (void)time(&now);
    strftime(filename, sizeof(filename),
        "borg-log-%Y-%m-%dT%H-%M-%S.json", localtime(&now));

    /* Write to the archive directory */
    path_build(filepath, sizeof(filepath), ANGBAND_DIR_ARCHIVE, filename);

    f = file_open(filepath, MODE_WRITE, FTYPE_TEXT);
    if (!f) {
        borg_note(format("# JSON log: failed to open %s", filepath));
        return;
    }

    /* Write JSON array */
    file_put(f, "[\n");

    for (i = 0; i < log_count; i++) {
        borg_json_entry *e = &log_entries[i];

        /* Escape message for JSON */
        char json_msg[256];
        {
            int ji = 0;
            for (int mi = 0; e->msg[mi] && ji < 250; mi++) {
                if (e->msg[mi] == '"' || e->msg[mi] == '\\') {
                    json_msg[ji++] = '\\';
                }
                json_msg[ji++] = e->msg[mi];
            }
            json_msg[ji] = '\0';
        }

        file_putf(f,
            "  {\"turn\":%d,\"depth\":%d,\"strategy\":\"%s\","
            "\"key\":%d,\"goal\":%d,\"food\":%d,"
            "\"x\":%d,\"y\":%d,\"msg\":\"%s\","
            "\"hp\":%d,\"mhp\":%d,\"sp\":%d,\"msp\":%d,"
            "\"danger\":%d,\"monsterCount\":%d,"
            "\"exploration\":%.2f,\"gold\":%d,\"playerLevel\":%d}",
            (int)e->turn, e->depth, e->strategy,
            e->key_code, e->goal_type, e->food,
            e->pos_x, e->pos_y, json_msg,
            e->hp, e->mhp, e->sp, e->msp,
            e->danger, e->monster_count,
            e->exploration, e->gold, e->player_level);

        if (i < log_count - 1)
            file_put(f, ",\n");
        else
            file_put(f, "\n");

        /* Track max depth */
        if (e->depth > max_depth)
            max_depth = e->depth;
    }

    file_put(f, "]\n");
    file_close(f);

    /* Print summary to borg log */
    borg_note(format("# JSON log: %d entries written to %s", log_count,
        filename));
    borg_note(format("# JSON log summary: max_depth=%d lowest_hp=%d "
        "final_level=%d",
        max_depth, lowest_hp,
        log_count > 0 ? log_entries[log_count - 1].player_level : 0));

    /* Free memory */
    mem_free(log_entries);
    log_entries  = NULL;
    log_count    = 0;
    log_capacity = 0;

    /* Close CSV action log if open */
    borg_action_log_close();
}

#endif
