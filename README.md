# mwin

`mwin` is a deliberately small fullscreen terminal window manager. It runs
several independent PTYs inside one terminal, displays one at a time, and
leaves persistence to [abduco](https://www.brain-dump.org/projects/abduco/)
and optional split panes to [mtm](https://github.com/deadpixi/mtm).

```text
foot -> abduco -> mwin -> shell / editor / mtm
```

It is not a replacement for tmux. There are no pane layouts, plugins,
configuration language, server protocol, copy mode, or search mode.

## Build

Only a C99 compiler and a POSIX-like system with PTYs are required. No curses
or terminal-emulation library is used.

```sh
make
sudo make install
```

On Void Linux:

```sh
sudo xbps-install -S base-devel abduco mtm
make
sudo make install
```

Copy `config.def.h` to `config.h` before building to change compile-time
defaults. `make` creates the copy automatically when it is absent.

An existing `config.h` remains authoritative across upgrades. Options added by
newer releases use built-in fallback values when they are absent, so older
configuration files still compile. To adopt `Ctrl-o` in an upgraded tree, set
`COMMAND_KEY` to `'o'`; fresh builds already use it.

## Use

Start a disposable workspace:

```sh
mwin
```

Start or attach to a persistent workspace:

```sh
abduco -A main mwin
```

Run a command in the first window:

```sh
mwin ssh server.example.org
mwin mtm
```

`mtm` has no special binding: start it like any other program whenever a
window needs split panes.

`mwin` forwards a standalone `Esc` immediately. If an editor still reacts to
`Esc` slowly only when it is running inside `mtm`, that delay comes from
ncurses' escape-sequence disambiguation. A small value keeps special keys
working while making mode changes nearly immediate:

```sh
ESCDELAY=25 mtm
```

## Window keys

The default command prefix is `Ctrl-o`:

| Key | Action |
| --- | --- |
| `Ctrl-o m` | create a shell window |
| `Ctrl-o o` | return to the previously visited window |
| `Ctrl-o Ctrl-n` | select the next window |
| `Ctrl-o Ctrl-p` | select the previous window |
| `Ctrl-o 1` … `9`, `0` | select window 1 … 10 |
| `Ctrl-o Ctrl-x` | close the current window and its process group |
| `Ctrl-o Ctrl-l` | redraw everything |
| `Ctrl-o Ctrl-u` | enter scrollback and move half a page backward |
| `Ctrl-o Ctrl-e` | open the retained scrollback in `$EDITOR` |
| `Ctrl-o ?` | show the key summary |
| `Ctrl-o Ctrl-o` | send a literal `Ctrl-o` to the application |

After the prefix is pressed, the status line shows that mwin is waiting for a
command key. The notice follows a prefix selected with `-c`; it is absent when
the status line is disabled with `-s`.

Repeated `Ctrl-o o` commands toggle between the two most recently visited
windows. Creating a window counts as visiting it; closing a remembered window
clears that reference safely.

The prefix can still be changed at runtime. `^G`, `g`, and decimal byte values
are accepted:

```sh
mwin -c '^G'
```

`abduco` uses `Ctrl-\\` to detach by default.

## Scrollback

Each window has an independent in-memory history. While browsing it:

| Key | Action |
| --- | --- |
| `Ctrl-u` | half a page backward |
| `Ctrl-d` | half a page forward |
| `Ctrl-b` | one page backward |
| `Ctrl-f` | one page forward |
| `y` | one row upward/older |
| `e` | one row downward/newer |
| `g` | oldest retained row |
| `G` | live terminal |
| `Esc` | leave scrollback and return to the live terminal |

Window commands remain available with the usual prefix while browsing. The
scrollback position belongs to the window, so switching away and back restores
the same view.

`Ctrl-o Ctrl-e` writes a plain-text snapshot of the retained history and live
screen, joins soft-wrapped rows, and opens it in a new window using
`${EDITOR:-vi}`. The temporary file is removed when that window closes.

New output continues to be parsed while browsing and the visible position
remains anchored. The history records only rows that leave the top of the
primary screen; alternate-screen redraws from editors and pagers do not fill
it. Soft-wrapped primary-screen and historical lines are reflowed when the
hosting terminal is resized. Hard newlines remain separate, cursor position is
preserved, and rows displaced by a height reduction move into scrollback.
Reflowed rows count toward the configured history limit; with
`MWIN_SCROLLBACK=0`, content that no longer fits on the live screen cannot be
retained.

Mwin reproduces soft-wrapped boundaries as real autowraps in the hosting
terminal. Consequently, selecting a wrapped logical line with foot's
`Ctrl-Shift-c` (or the equivalent shortcut in another terminal) copies it
without inserting a newline at the visual wrap point. Explicit newlines remain
newlines.

Rows are stored compactly as UTF-8 plus style changes. They are allocated only
as output scrolls. The only limit is the number of rows:

```sh
MWIN_SCROLLBACK=10000 mwin
MWIN_SCROLLBACK=0 mwin       # disable history
```

The default is 2000 rows per window and can be changed in `config.h`.

## Environment

- `SHELL`: shell created by `Ctrl-o m`; falls back to the login shell and then
  `/bin/sh`.
- `EDITOR`: command used by `Ctrl-o Ctrl-e`; defaults to `vi`.
- `MWIN_SCROLLBACK`: maximum retained rows per window; defaults to 2000 and
  accepts `0` to disable history.
- `TERM`: describes the hosting terminal. Children receive the value configured
  as `CHILD_TERM`, `screen-256color` by default.
- `MWIN`: set to `1` in child processes.

## Intentional limits

The built-in VT parser covers the sequences normally needed by shells,
full-screen editors, `fzf`, pagers, and ncurses applications. It supports the
primary and alternate screens, scroll regions, insert/delete operations,
256-color and RGB SGR attributes, Unicode cells, bracketed paste, common mouse
modes, and cursor reports.

It deliberately does not provide:

- scrollback search, text selection, or copy mode;
- window names edited by mwin (OSC titles are displayed automatically);
- multiple attached clients controlled by mwin itself;
- passthrough for OSC 8 hyperlinks, OSC 52 clipboard access, sixel, kitty
  graphics, or kitty's keyboard protocol;
- perfect grapheme-cluster shaping.

Use `abduco` for persistence. The window and scrollback state then survive
closing the outer terminal without adding a daemon or socket protocol to mwin.

## Tests

```sh
make test
```

Python 3 is required only for the integration tests, not to build or run mwin.
The tests drive the real executable through a pseudo-terminal and reconstruct
the visible host screen. They exercise every documented key, alternate
prefixes, exact child dimensions and environment, input forwarding, scrollback,
editor snapshots and temporary-file cleanup, resize reflow, soft-wrap copy
semantics, alternate-screen isolation, process groups, and clean terminal
restoration. A separate model test covers compact history and representative VT
parsing details without adding test code to the installed binary.

An optional coverage report can be produced when `gcov` is installed:

```sh
make coverage
```
