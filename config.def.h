#ifndef MINWIN_CONFIG_H
#define MINWIN_CONFIG_H

/* Ctrl-key used as the minwin command prefix. */
#define COMMAND_KEY 'a'

/* One screen row is reserved for the window list when non-zero. */
#define SHOW_STATUS 1

/* Hard limit keeps allocations and the command interface predictable. */
#define MAX_WINDOWS 32

/* Advertise only capabilities minwin intentionally emulates. */
#define CHILD_TERM "screen-256color"

/* Command started by prefix + m. */
#define SPLIT_COMMAND "mtm"

#endif
