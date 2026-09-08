# minwin

`minwin` is a deliberately small fullscreen terminal window manager. It runs
several independent PTYs inside one terminal, displays one at a time, and
leaves persistence to [abduco](https://www.brain-dump.org/projects/abduco/)
and splits to [mtm](https://github.com/deadpixi/mtm).

```text
foot -> abduco -> minwin -> shell / editor / mtm
```

It is not a replacement for tmux. There are no panes, layouts, plugins,
configuration language, server protocol, or permanent scrollback.

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

## Use

Start a disposable workspace:

```sh
minwin
```

Start or attach to a persistent workspace:

```sh
abduco -A main minwin
```

Run a command in the first window:

```sh
minwin ssh server.example.org
```

The default command prefix is `Ctrl-a`:

| Key | Action |
| --- | --- |
| `Ctrl-a c` | create a shell window |
| `Ctrl-a m` | create a window running `mtm` |
| `Ctrl-a n` | select next window |
| `Ctrl-a p` | select previous window |
| `Ctrl-a 1` … `9`, `0` | select window 1 … 10 |
| `Ctrl-a x` | close the current window and its process group |
| `Ctrl-a l` | redraw everything |
| `Ctrl-a ?` | show the key summary |
| `Ctrl-a Ctrl-a` | send a literal `Ctrl-a` to the application |

The prefix can be changed at runtime. `^G`, `g`, and the decimal byte value
are accepted:

```sh
minwin -c '^G'
```

`abduco` uses `Ctrl-\\` to detach by default. When `mtm` runs in a minwin
window, its own default prefix remains `Ctrl-g`.

## Environment

- `SHELL`: shell created by `Ctrl-a c`; falls back to the login shell and then
  `/bin/sh`.
- `MINWIN_MTM`: split command created by `Ctrl-a m`; defaults to `mtm`.
- `TERM`: read for the hosting terminal. Children receive the value configured
  as `CHILD_TERM`, `screen-256color` by default.

## Intentional limits

The built-in VT parser covers the sequences normally needed by shells,
full-screen editors, `fzf`, pagers, and ncurses applications. It supports the
primary and alternate screens, scroll regions, insert/delete operations,
256-color and RGB SGR attributes, Unicode cells, bracketed paste, common mouse
modes, and cursor reports.

It deliberately does not yet provide:

- persistent scrollback or copy mode;
- window names edited by minwin (OSC titles are displayed automatically);
- multiple attached clients controlled by minwin itself;
- passthrough for OSC 8 hyperlinks, OSC 52 clipboard access, sixel, kitty
  graphics, or kitty's keyboard protocol;
- perfect grapheme-cluster shaping.

Use `abduco` for persistence. This also means a session can survive closing
the outer terminal without adding a daemon or socket protocol to minwin.

## Tests

```sh
make test
```

The self-test exercises the VT model. The integration test runs minwin under a
real pseudo-terminal, creates and closes windows, and verifies clean shutdown.
