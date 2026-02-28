/**
 * \file borg-json-log.h
 * \brief JSON logging for borg AI analysis
 *
 * Outputs BorgLogEntry[] JSON compatible with angband-ts borg logs.
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

#ifndef BORG_JSON_LOG_H
#define BORG_JSON_LOG_H

/*
 * must be included before ALLOW_BORG to avoid empty compilation unit
 */
#include "../angband.h"

#ifdef ALLOW_BORG

/* Current strategy name, set by borg_think_dungeon branches */
extern const char *borg_json_strategy;

extern void borg_json_log_init(void);
extern void borg_json_log_turn(void);
extern void borg_json_log_finish(void);

#endif
#endif
