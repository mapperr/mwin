#ifndef MWIN_CONFIG_H
#define MWIN_CONFIG_H

/* Ctrl-key used as the mwin command prefix. */
#define COMMAND_KEY 'o'

/* One screen row is reserved for the window list when non-zero. */
#define SHOW_STATUS 1

/* Hard limit keeps allocations and the command interface predictable. */
#define MAX_WINDOWS 32

/* Maximum scrollback rows per window; MWIN_SCROLLBACK overrides this. */
#define DEFAULT_SCROLLBACK 2000

/* Advertise only capabilities mwin intentionally emulates. */
#define CHILD_TERM "screen-256color"

#endif
