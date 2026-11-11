/*
 * anvil_log_throttle.h - the C interface of anvil-log-throttle, the standard
 * plugin that writes a repeating line of the game's logs a few times and then
 * only counts it (anvil#172). Its service anvil-log-throttle.messages gives
 * the table of the messages of the throttled logs (the `logs` setting,
 * error.log by default) with their counts as they happen, rather than when
 * the next report reaches the file a minute later. Its script errors panel
 * reads the same table (docs/script-errors-panel.md).
 *
 *   - A message is its log, the game's source file and line (the C++ code
 *     that wrote it, "[triggerimplementation.cpp:7256]" in error.log, not a
 *     mod's file) and the text. Its count is `written` (the lines that reached
 *     the log) plus `counted` (the lines the throttle only counted). Once the
 *     game has closed the log, or after the next report, error.log gives the
 *     same count: the plain lines plus the repeats of the reports.
 *   - The table is kept only from keep() on (the panel setting calls it at
 *     the plugin's load), so a game that never asks for it pays nothing for
 *     it. Lines before keep() are not in it. It is kept until the process
 *     ends; nothing leaves it.
 *   - At most 32768 messages; the lines of a message after that count only in
 *     `untracked`.
 *   - Text is UTF-8: the game's Windows-1252 converted, or its bytes as they
 *     are when they are already valid UTF-8 (Mei).
 *   - Threads: any. The game writes its logs from any of its threads, and
 *     every call takes the table's lock for a copy; a call does not wait for
 *     the game otherwise. Every string the service returns stays valid until
 *     the process ends.
 *
 * Depend on the plugin in plugin.toml ([[dependencies]] id =
 * "anvil-log-throttle", optional = true when the plugin also works without
 * it) and look the service up with get_service; NULL when the plugin is
 * disabled or older than 1.20.0. With a `logs` setting that names no log
 * the table stays empty.
 *
 * Plain C11. Within version 1 functions are only appended to the end of the
 * service and fields to the end of the structs, behind `size` and
 * `message_size`.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_LOG_THROTTLE_H
#define ANVIL_LOG_THROTTLE_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_LOG_THROTTLE_ID "anvil-log-throttle"
#define ANVIL_LOG_MESSAGES_SERVICE "anvil-log-throttle.messages"
#define ANVIL_LOG_MESSAGES_SERVICE_VERSION 1

typedef struct AnvilLogTotals {
    uint32_t size;    /* the caller sets sizeof(AnvilLogTotals); the service fills as far as both know */
    uint32_t keeping; /* 1 once keep() has started the table, else 0 and the rest is 0 */
    uint64_t version; /* grows with every line the table takes: the same value, nothing changed */
    uint64_t since_ms; /* GetTickCount64() when the table started */
    uint64_t messages; /* messages in the table */
    uint64_t written;  /* lines of the throttled logs since then that reached the log */
    uint64_t counted;  /* lines the throttle counted instead */
    uint64_t untracked; /* of those, lines of messages the full table does not take */
} AnvilLogTotals;

typedef struct AnvilLogMessage {
    uint64_t index;   /* its position in the table, from 0, in the order first seen */
    uint64_t version; /* the table's version at its last line */
    uint64_t written; /* lines that reached the log */
    uint64_t counted; /* lines the throttle counted instead; above 0 once it throttles the message */
    const char *log;  /* the log's file name, "error.log" */
    const char *source; /* the game's C++ source file name, "triggerimplementation.cpp" */
    int32_t source_line;
    uint32_t reserved;
    const char *text; /* UTF-8, without the line end */
} AnvilLogMessage;

typedef struct AnvilLogMessagesService {
    uint32_t size; /* sizeof(AnvilLogMessagesService) as known by the plugin */

    /* Starts the table, if it is not kept yet. ANVIL_OK either way. */
    AnvilResult (*keep)(void);
    /* The table's totals. ANVIL_ERROR_INVALID_ARGUMENT for NULL or a size
     * below that of version 1.0. */
    AnvilResult (*totals)(AnvilLogTotals *out);
    /* Copies the messages from position `first` on whose version is above
     * `changed_after` (0: every one), at most `capacity`, to `out`, and sets
     * `*count` to how many and `*next` to the position after the last one it
     * looked at: the table's size once it got to the end, so a loop of calls
     * from `*next` reads the whole table. A reader that keeps the version of
     * its last totals() and passes it as `changed_after` gets only what
     * changed since. Pass message_size = sizeof(AnvilLogMessage).
     * ANVIL_ERROR_INVALID_ARGUMENT for NULL pointers (out may be NULL when
     * capacity is 0) or a message_size below that of version 1.0. */
    AnvilResult (*read)(uint64_t first, uint64_t changed_after, AnvilLogMessage *out, uint32_t capacity,
                        uint32_t message_size, uint32_t *count, uint64_t *next);
} AnvilLogMessagesService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_LOG_MESSAGES_HAS(service, field) \
    ((service)->size >= offsetof(AnvilLogMessagesService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_LOG_THROTTLE_H */
