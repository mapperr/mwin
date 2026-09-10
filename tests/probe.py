#!/usr/bin/env python3
"""Deterministic child process used by mwin's PTY integration tests."""

import hashlib
import os
import signal
import sys
import termios
import tty


def write(data):
    os.write(sys.stdout.fileno(), data)


def title(text):
    write(b"\033]2;" + text.encode("ascii") + b"\a")


def wait_forever():
    while True:
        signal.pause()


def mode_env():
    size = os.get_terminal_size(sys.stdout.fileno())
    text = "ENV-%dx%d-%s-%s" % (
        size.lines,
        size.columns,
        os.environ.get("TERM", ""),
        os.environ.get("MWIN", ""),
    )
    title(text)
    write(text.encode("ascii"))
    wait_forever()


def mode_input():
    expected = int(os.environ["MWIN_PROBE_BYTES"])
    tty.setraw(sys.stdin.fileno(), when=termios.TCSANOW)
    title("READY")
    write(b"READY")
    received = bytearray()
    while len(received) < expected:
        chunk = os.read(sys.stdin.fileno(), expected - len(received))
        if not chunk:
            return
        received.extend(chunk)
    result = "HEX-" + received.hex()
    title(result)
    write(b"\r\n" + result.encode("ascii"))
    wait_forever()


def mode_lines():
    pid = os.getpid()
    tty.setraw(sys.stdin.fileno(), when=termios.TCSANOW)
    title("LINES-%d" % pid)
    for number in range(12):
        write(("L%02d\r\n" % number).encode("ascii"))
    write(b"LIVE")

    def more_output(_signum, _frame):
        write(b"\r\nNEW")

    signal.signal(signal.SIGUSR1, more_output)
    while True:
        byte = os.read(sys.stdin.fileno(), 1)
        if not byte:
            return
        title("HEX-" + byte.hex())


def mode_wrap():
    title("WRAP")
    write(b"abcdefghij\r\nKLMN")
    wait_forever()


def mode_capture():
    title("CAPTURE")
    write(b"H0\r\nH1\r\nabcdefghij\r\nKLMN\r\nEND")
    wait_forever()


def mode_editor():
    with open(sys.argv[2], "rb") as source:
        data = source.read()
    marker = os.environ.get("MWIN_PROBE_MARKER")
    if marker:
        with open(marker, "w", encoding="utf-8") as output:
            output.write(sys.argv[2])
    digest = hashlib.sha256(data).hexdigest()[:16]
    title("EDITOR-" + digest)
    write(b"EDITOR")
    wait_forever()


def mode_host_modes():
    tty.setraw(sys.stdin.fileno(), when=termios.TCSANOW)
    write(b"\033[?1h\033=\033[?2004h\033[?1004h\033[?1002h\033[?1006h")
    title("MODES-ON")
    if os.read(sys.stdin.fileno(), 1):
        write(b"\033[?1l\033>\033[?2004l\033[?1004l\033[?1002l\033[?1006l")
        title("MODES-OFF")
    wait_forever()


def mode_exit():
    title("DONE")
    write(b"DONE")


def mode_alternate():
    tty.setraw(sys.stdin.fileno(), when=termios.TCSANOW)
    title("PRIMARY")
    write(b"PRIMARY")
    while True:
        byte = os.read(sys.stdin.fileno(), 1)
        if not byte:
            return
        if byte == b"a":
            write(b"\033[?1049h\033[2J\033[H")
            for number in range(12):
                write(("A%02d\r\n" % number).encode("ascii"))
            write(b"ALTERNATE")
            title("ALT")
        elif byte == b"p":
            write(b"\033[?1049l")
            title("PRIMARY")


def mode_group():
    marker = os.environ["MWIN_PROBE_MARKER"]

    def stopped(_signum, _frame):
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        descriptor = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        os.write(descriptor, ("%d\n" % os.getpid()).encode("ascii"))
        os.close(descriptor)
        os._exit(0)

    signal.signal(signal.SIGHUP, stopped)
    signal.signal(signal.SIGTERM, stopped)
    child = os.fork()
    if child == 0:
        wait_forever()
    title("GROUP-%d-%d" % (os.getpid(), child))
    write(b"GROUP-READY")
    wait_forever()


def main():
    modes = {
        "env": mode_env,
        "exit": mode_exit,
        "input": mode_input,
        "lines": mode_lines,
        "wrap": mode_wrap,
        "alternate": mode_alternate,
        "capture": mode_capture,
        "editor": mode_editor,
        "group": mode_group,
        "host-modes": mode_host_modes,
    }
    if (len(sys.argv) < 2 or sys.argv[1] not in modes or
            len(sys.argv) != (3 if sys.argv[1] == "editor" else 2)):
        raise SystemExit("usage: probe.py %s" % "|".join(sorted(modes)))
    modes[sys.argv[1]]()


if __name__ == "__main__":
    main()
