#!/usr/bin/env python3
"""Black-box tests for mwin's command line and terminal user interface."""

import errno
import fcntl
import hashlib
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest


ROOT = Path(__file__).resolve().parent.parent
MWIN = ROOT / "mwin"
PROBE = ROOT / "tests" / "probe.py"
CTRL_O = b"\x0f"
VERSION = next(line.split("=", 1)[1].strip()
               for line in (ROOT / "Makefile").read_text().splitlines()
               if line.startswith("VERSION ="))


class HostScreen:
    """Small model of the subset of VT output emitted by mwin itself."""

    def __init__(self, rows, columns):
        self.state = "ground"
        self.sequence = bytearray()
        self.autowrap = True
        self.resize(rows, columns)

    def resize(self, rows, columns):
        self.rows = rows
        self.columns = columns
        self.cells = [[" "] * columns for _ in range(rows)]
        self.wrapped = [False] * rows
        self.row = 0
        self.column = 0
        self.wrap_pending = False

    def line(self, row):
        return "".join(self.cells[row]).rstrip()

    def lines(self, first=0, last=None):
        if last is None:
            last = self.rows - 1
        return [self.line(row) for row in range(first, last + 1)]

    def contains(self, text):
        return any(text in line for line in self.lines())

    def logical_text(self, first, last):
        result = []
        for row in range(first, last + 1):
            result.append(self.line(row))
            if row < last and not self.wrapped[row]:
                result.append("\n")
        return "".join(result)

    def _linefeed(self):
        if self.row + 1 < self.rows:
            self.row += 1
            return
        self.cells.pop(0)
        self.cells.append([" "] * self.columns)
        self.wrapped.pop(0)
        self.wrapped.append(False)

    def _put(self, byte):
        if self.wrap_pending:
            if self.autowrap:
                self.wrapped[self.row] = True
                self._linefeed()
                self.column = 0
            self.wrap_pending = False
        self.cells[self.row][self.column] = chr(byte)
        if self.column + 1 == self.columns:
            self.wrap_pending = True
        else:
            self.column += 1

    @staticmethod
    def _number(text, fallback):
        if not text:
            return fallback
        try:
            return int(text)
        except ValueError:
            return fallback

    def _csi(self, final, parameters):
        private = parameters.startswith("?")
        if private:
            parameters = parameters[1:]
        fields = parameters.split(";") if parameters else []
        if final in ("H", "f"):
            row = self._number(fields[0], 1) if fields else 1
            column = self._number(fields[1], 1) if len(fields) > 1 else 1
            self.row = min(max(row - 1, 0), self.rows - 1)
            self.column = min(max(column - 1, 0), self.columns - 1)
            self.wrap_pending = False
        elif final == "J" and self._number(fields[0], 0) in (2, 3):
            self.cells = [[" "] * self.columns for _ in range(self.rows)]
            self.wrapped = [False] * self.rows
            self.wrap_pending = False
        elif private and final in ("h", "l"):
            enabled = final == "h"
            for field in fields:
                if field == "7":
                    self.autowrap = enabled
                elif field == "1049" and enabled:
                    self.cells = [[" "] * self.columns for _ in range(self.rows)]
                    self.wrapped = [False] * self.rows
                    self.row = 0
                    self.column = 0
                    self.wrap_pending = False

    def feed(self, data):
        for byte in data:
            if self.state == "ground":
                if byte == 0x1B:
                    self.state = "escape"
                elif byte == 0x0D:
                    self.column = 0
                    self.wrap_pending = False
                elif byte in (0x0A, 0x0B, 0x0C):
                    self.wrapped[self.row] = False
                    self._linefeed()
                elif byte == 0x08:
                    self.column = max(0, self.column - 1)
                    self.wrap_pending = False
                elif 0x20 <= byte < 0x7F:
                    self._put(byte)
            elif self.state == "escape":
                if byte == ord("["):
                    self.sequence.clear()
                    self.state = "csi"
                elif byte == ord("]"):
                    self.state = "osc"
                elif byte in (ord("("), ord(")")):
                    self.state = "charset"
                else:
                    self.state = "ground"
            elif self.state == "csi":
                if 0x40 <= byte <= 0x7E:
                    self._csi(chr(byte), self.sequence.decode("ascii", "ignore"))
                    self.state = "ground"
                else:
                    self.sequence.append(byte)
            elif self.state == "osc":
                if byte == 0x07:
                    self.state = "ground"
                elif byte == 0x1B:
                    self.state = "osc_escape"
            elif self.state == "osc_escape":
                self.state = "ground" if byte == ord("\\") else "osc"
            elif self.state == "charset":
                self.state = "ground"


class MwinSession:
    def __init__(self, arguments, rows=8, columns=60, environment=None, prefix=CTRL_O):
        master, slave = os.openpty()
        self.master = master
        self.prefix = prefix
        self.screen = HostScreen(rows, columns)
        self.transcript = bytearray()
        self._set_size(rows, columns)
        self.original_termios = termios.tcgetattr(master)
        env = os.environ.copy()
        env.setdefault("LC_ALL", "C")
        if environment:
            env.update(environment)
        self.process = subprocess.Popen(
            [str(MWIN), *map(str, arguments)],
            cwd=ROOT,
            env=env,
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
            start_new_session=True,
        )
        os.close(slave)
        os.set_blocking(master, False)

    def _set_size(self, rows, columns):
        size = struct.pack("HHHH", rows, columns, 0, 0)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, size)

    def pump(self, timeout=0.05):
        ready, _, _ = select.select([self.master], [], [], timeout)
        if not ready:
            return False
        received = False
        while True:
            try:
                data = os.read(self.master, 65536)
            except OSError as error:
                if error.errno in (errno.EAGAIN, errno.EIO):
                    break
                raise
            if not data:
                break
            received = True
            self.transcript.extend(data)
            self.screen.feed(data)
        return received

    def wait_for(self, predicate, description, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump()
            if predicate():
                return
            if self.process.poll() is not None:
                break
        visible = "\n".join("%02d %s" % item for item in enumerate(self.screen.lines()))
        raise AssertionError("timeout waiting for %s\n%s" % (description, visible))

    def wait_text(self, text, timeout=3.0):
        self.wait_for(lambda: self.screen.contains(text), repr(text), timeout)

    def wait_status(self, text, timeout=3.0):
        self.wait_for(lambda: text in self.screen.line(self.screen.rows - 1),
                      "status %r" % text, timeout)

    def send(self, data):
        view = memoryview(data)
        while view:
            written = os.write(self.master, view)
            view = view[written:]

    def command(self, byte):
        self.send(self.prefix + byte)

    def resize(self, rows, columns):
        self.screen.resize(rows, columns)
        self._set_size(rows, columns)
        os.kill(self.process.pid, signal.SIGWINCH)

    def wait_exit(self, timeout=3.0):
        deadline = time.monotonic() + timeout
        while self.process.poll() is None and time.monotonic() < deadline:
            self.pump()
        if self.process.poll() is None:
            raise AssertionError("mwin did not exit")
        self.pump(0)
        return self.process.returncode

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=2)
        try:
            os.close(self.master)
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, _type, _value, _traceback):
        self.close()


class CommandLineTests(unittest.TestCase):
    def run_mwin(self, *arguments, environment=None):
        env = os.environ.copy()
        if environment:
            env.update(environment)
        return subprocess.run(
            [str(MWIN), *arguments], cwd=ROOT, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )

    def test_help_and_version(self):
        for argument in ("-h", "--help"):
            with self.subTest(argument=argument):
                result = self.run_mwin(argument)
                self.assertEqual(result.returncode, 0)
                self.assertIn(b"usage: mwin", result.stdout)
        result = self.run_mwin("-v")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, ("mwin %s\n" % VERSION).encode("ascii"))

    def test_invalid_command_keys(self):
        for value in ("00", "-1", "256", "bad"):
            with self.subTest(value=value):
                result = self.run_mwin("-c", value)
                self.assertEqual(result.returncode, 2)

        result = self.run_mwin("-z")
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"usage: mwin", result.stderr)

    def test_invalid_scrollback_limits(self):
        for value in ("", "invalid", "-1", " 5", "5x", "9" * 80):
            with self.subTest(value=value):
                result = self.run_mwin(environment={"MWIN_SCROLLBACK": value})
                self.assertEqual(result.returncode, 2)
                self.assertIn(b"invalid MWIN_SCROLLBACK", result.stderr)

    def test_old_config_header_still_compiles(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "compat.o"
            result = subprocess.run([
                os.environ.get("CC", "cc"), "-DMWIN_CONFIG_H",
                "-DCOMMAND_KEY=109", "-DSHOW_STATUS=1", "-DMAX_WINDOWS=32",
                '-DCHILD_TERM="screen-256color"', "-std=c99", "-pedantic",
                "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-c",
                "-o", str(output), "mwin.c",
            ], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(result.returncode, 0, result.stderr.decode())


class TerminalInterfaceTests(unittest.TestCase):
    def probe(self, mode, **kwargs):
        return MwinSession([sys.executable, PROBE, mode], **kwargs)

    def test_child_environment_and_terminal_size(self):
        with self.probe("env", rows=8, columns=50) as session:
            session.wait_status("ENV-7x50-screen-256color-1")
        with MwinSession(["-s", sys.executable, PROBE, "env"],
                         rows=8, columns=50) as session:
            session.wait_text("ENV-8x50-screen-256color-1")
            self.assertNotIn("1:", session.screen.line(7))

    def test_prefix_help_redraw_and_literal_prefix(self):
        environment = {"MWIN_PROBE_BYTES": "1"}
        with self.probe("input", rows=8, columns=100,
                        environment=environment) as session:
            session.wait_status("[1:READY]")
            session.send(CTRL_O)
            session.wait_status("prefix ^O: waiting for key")
            session.send(b"?")
            session.wait_status("^O m:new")
            self.assertIn("^E:editor", session.screen.line(7))
            session.send(b"z")
            session.wait_status("[1:READY]")
            before = session.screen.lines(0, 6)
            start = len(session.transcript)
            session.command(b"\x0c")
            session.wait_for(lambda: b"\033[2J" in session.transcript[start:],
                             "full redraw")
            self.assertEqual(session.screen.lines(0, 6), before)
            session.command(CTRL_O)
            session.wait_status("[1:HEX-0f]")

    def test_alternate_prefix(self):
        environment = {"MWIN_PROBE_BYTES": "1"}
        with MwinSession(["-c", "^G", sys.executable, PROBE, "input"],
                         rows=8, columns=80, environment=environment,
                         prefix=b"\x07") as session:
            session.wait_status("[1:READY]")
            session.send(b"\x07")
            session.wait_status("prefix ^G: waiting for key")
            session.send(b"\x07")
            session.wait_status("[1:HEX-07]")

    def test_literal_and_decimal_prefixes(self):
        cases = (("g", b"g", "prefix g: waiting for key", "67"),
                 ("15", CTRL_O, "prefix ^O: waiting for key", "0f"))
        for configured, key, pending, received in cases:
            with self.subTest(configured=configured):
                environment = {"MWIN_PROBE_BYTES": "1"}
                with MwinSession(["-c", configured, sys.executable, PROBE, "input"],
                                 rows=8, columns=80, environment=environment,
                                 prefix=key) as session:
                    session.wait_status("[1:READY]")
                    session.send(key)
                    session.wait_status(pending)
                    session.send(key)
                    session.wait_status("[1:HEX-%s]" % received)

    def test_status_can_be_disabled_while_commands_still_work(self):
        environment = {"MWIN_PROBE_BYTES": "1"}
        with MwinSession(["-s", sys.executable, PROBE, "input"],
                         rows=6, columns=80, environment=environment) as session:
            session.wait_text("READY")
            session.command(CTRL_O)
            session.wait_text("HEX-0f")
            self.assertNotIn(b"waiting for key", session.transcript)

    def test_escape_is_forwarded_without_a_second_key(self):
        with self.probe("input", rows=8, columns=60,
                        environment={"MWIN_PROBE_BYTES": "1"}) as session:
            session.wait_status("[1:READY]")
            session.send(b"\x1b")
            session.wait_status("[1:HEX-1b]")

    def test_application_terminal_modes_reach_the_host(self):
        enabled = (b"\033[?1h", b"\033=", b"\033[?2004h", b"\033[?1004h",
                   b"\033[?1002h", b"\033[?1006h")
        disabled = (b"\033[?1l", b"\033>", b"\033[?2004l", b"\033[?1004l",
                    b"\033[?1002l", b"\033[?1006l")
        with self.probe("host-modes", rows=8, columns=80) as session:
            session.wait_status("[1:MODES-ON]")
            for sequence in enabled:
                self.assertIn(sequence, session.transcript)
            start = len(session.transcript)
            session.send(b"d")
            session.wait_status("[1:MODES-OFF]")
            for sequence in disabled:
                self.assertIn(sequence, session.transcript[start:])

    def test_bracketed_paste_bypasses_the_prefix(self):
        pasted = b"\033[200~x\x0fy\033[201~"
        environment = {"MWIN_PROBE_BYTES": str(len(pasted))}
        with self.probe("input", rows=8, columns=100,
                        environment=environment) as session:
            session.wait_status("[1:READY]")
            session.send(pasted)
            session.wait_status("[1:HEX-%s]" % pasted.hex())

    def test_window_navigation_and_previous_window(self):
        environment = {"SHELL": "/bin/sh"}
        with MwinSession(["sleep", "30"], rows=8, columns=100,
                         environment=environment) as session:
            session.wait_status("[1:sleep]")
            session.command(b"m")
            session.wait_status("[2:sh]")
            session.command(b"1")
            session.wait_status("[1:sleep]")
            session.command(b"\x0e")
            session.wait_status("[2:sh]")
            session.command(b"\x10")
            session.wait_status("[1:sleep]")
            session.command(b"o")
            session.wait_status("[2:sh]")
            session.command(b"o")
            session.wait_status("[1:sleep]")
            session.command(b"2")
            session.wait_status("[2:sh]")
            session.command(b"\x18")
            session.wait_status("[1:sleep]")
            session.command(b"o")
            session.wait_status("[1:sleep]")
            session.command(b"\x18")
            self.assertEqual(session.wait_exit(), 0)

    def test_direct_selection_of_window_ten(self):
        with MwinSession(["sleep", "30"], rows=8, columns=180,
                         environment={"SHELL": "/bin/sh"}) as session:
            session.wait_status("[1:sleep]")
            for number in range(2, 11):
                session.command(b"m")
                session.wait_status("[%d:sh]" % number)
            for number in range(1, 10):
                session.command(str(number).encode("ascii"))
                title = "sleep" if number == 1 else "sh"
                session.wait_status("[%d:%s]" % (number, title))
            session.command(b"0")
            session.wait_status("[10:sh]")

    def test_scrollback_navigation_and_limit(self):
        with self.probe("lines", rows=6, columns=60,
                        environment={"MWIN_SCROLLBACK": "5"}) as session:
            session.wait_for(lambda: session.screen.lines(0, 4) ==
                             ["L08", "L09", "L10", "L11", "LIVE"],
                             "live output")
            session.command(b"\x15")
            session.wait_status("scroll 2/5")
            self.assertEqual(session.screen.lines(0, 4),
                             ["L06", "L07", "L08", "L09", "L10"])
            session.send(b"\x15")
            session.wait_status("scroll 4/5")
            self.assertEqual(session.screen.lines(0, 4),
                             ["L04", "L05", "L06", "L07", "L08"])
            session.send(b"\x02")
            session.wait_status("scroll 5/5")
            session.send(b"j")
            session.wait_status("scroll 4/5")
            session.send(b"k")
            session.wait_status("scroll 5/5")
            session.send(b"\x04")
            session.wait_status("scroll 3/5")
            session.send(b"\x06")
            session.wait_status("[1:LINES-")
            self.assertEqual(session.screen.lines(0, 4),
                             ["L08", "L09", "L10", "L11", "LIVE"])
            session.command(b"\x15")
            session.wait_status("scroll 2/5")
            session.send(b"g")
            session.wait_status("scroll 5/5")
            session.send(b"G")
            session.wait_status("[1:LINES-")
            session.command(b"\x15")
            session.wait_status("scroll 2/5")
            session.send(b"\x1b")
            session.wait_status("[1:LINES-")
            session.send(b"x")
            session.wait_status("[1:HEX-78]")

    def test_window_switch_preserves_scrollback_position(self):
        environment = {"MWIN_SCROLLBACK": "20", "SHELL": "/bin/sh"}
        with self.probe("lines", rows=6, columns=60,
                        environment=environment) as session:
            session.wait_text("LIVE")
            session.command(b"\x15")
            session.wait_status("scroll 2/8")
            expected = session.screen.lines(0, 4)

            session.send(CTRL_O)
            session.wait_status("prefix ^O: waiting for key")
            session.send(b"m")
            session.wait_status("[2:sh]")
            session.command(b"1")
            session.wait_status("scroll 2/8")
            self.assertEqual(session.screen.lines(0, 4), expected)

    def test_edit_scrollback_with_editor(self):
        captured = b"H0\nH1\nabcdefghij\nKLMN\nEND"
        digest = hashlib.sha256(captured).hexdigest()[:16]
        for prefixed in (False, True):
            binding = "Ctrl-o Ctrl-e" if prefixed else "e"
            with self.subTest(binding=binding), \
                    tempfile.TemporaryDirectory() as directory:
                marker = Path(directory) / "editor-path"
                environment = {
                    "EDITOR": "%s %s editor" % (sys.executable, PROBE),
                    "MWIN_PROBE_MARKER": str(marker),
                    "MWIN_SCROLLBACK": "20",
                }
                with self.probe("capture", rows=6, columns=4,
                                environment=environment) as session:
                    session.wait_text("END")
                    if prefixed:
                        session.command(b"\x05")
                    else:
                        session.command(b"\x15")
                        session.wait_for(
                            lambda: session.screen.lines(0, 4) ==
                            ["H0", "H1", "abcd", "efgh", "ij"],
                            "scrollback viewport",
                        )
                        session.send(b"e")
                    session.wait_for(marker.exists, "editor temporary path")
                    session.resize(6, 80)
                    session.wait_status("[2:EDITOR-%s]" % digest)
                    temporary = Path(marker.read_text())
                    self.assertEqual(temporary.read_bytes(), captured)
                    session.command(b"\x18")
                    session.wait_status("[1:CAPTURE]")
                    session.wait_for(lambda: not temporary.exists(),
                                     "temporary file cleanup")

    def test_new_output_keeps_scrollback_anchored(self):
        with self.probe("lines", rows=6, columns=60,
                        environment={"MWIN_SCROLLBACK": "20"}) as session:
            session.wait_text("LIVE")
            match = re.search(r"LINES-(\d+)", session.screen.line(5))
            self.assertIsNotNone(match)
            child_pid = int(match.group(1))
            session.command(b"\x15")
            session.wait_status("scroll 2/8")
            self.assertEqual(session.screen.line(0), "L06")
            os.kill(child_pid, signal.SIGUSR1)
            session.wait_status("scroll 3/9")
            self.assertEqual(session.screen.line(0), "L06")

    def test_zero_scrollback_never_enters_browse_mode(self):
        with self.probe("lines", rows=6, columns=60,
                        environment={"MWIN_SCROLLBACK": "0"}) as session:
            session.wait_text("LIVE")
            session.command(b"\x15")
            session.wait_status("[1:LINES-")
            self.assertNotIn("scroll", session.screen.line(5))

    def test_soft_wrap_copy_and_resize(self):
        with self.probe("wrap", rows=8, columns=4) as session:
            session.wait_for(lambda: session.screen.line(3) == "KLMN",
                             "wrapped output at four columns")
            self.assertEqual(session.screen.logical_text(0, 3),
                             "abcdefghij\nKLMN")
            session.resize(8, 6)
            session.wait_for(lambda: session.screen.line(2) == "KLMN",
                             "reflowed output at six columns")
            self.assertEqual(session.screen.logical_text(0, 2),
                             "abcdefghij\nKLMN")
            session.resize(8, 3)
            session.wait_for(lambda: session.screen.lines(0, 2) == ["j", "KLM", "N"],
                             "reflowed output at three columns")
            session.command(b"\x15")
            session.send(b"g")
            session.wait_for(lambda: session.screen.lines(0, 5) ==
                             ["abc", "def", "ghi", "j", "KLM", "N"],
                             "reflowed scrollback at three columns")
            self.assertEqual(session.screen.logical_text(0, 5),
                             "abcdefghij\nKLMN")

    def test_alternate_screen_does_not_enter_history(self):
        with self.probe("alternate", rows=6, columns=60) as session:
            session.wait_text("PRIMARY")
            session.send(b"a")
            session.wait_status("[1:ALT]")
            session.wait_text("ALTERNATE")
            session.send(b"p")
            session.wait_status("[1:PRIMARY]")
            session.wait_text("PRIMARY")
            session.command(b"\x15")
            session.wait_status("[1:PRIMARY]")
            self.assertNotIn("scroll", session.screen.line(5))

    def test_close_terminates_the_child_process_group(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "stopped"
            environment = {"MWIN_PROBE_MARKER": str(marker)}
            with self.probe("group", rows=6, columns=70,
                            environment=environment) as session:
                session.wait_status("[1:GROUP-")
                match = re.search(r"GROUP-(\d+)-(\d+)", session.screen.line(5))
                self.assertIsNotNone(match)
                expected = {int(match.group(1)), int(match.group(2))}
                session.command(b"\x18")
                self.assertEqual(session.wait_exit(), 0)
                deadline = time.monotonic() + 2
                stopped = set()
                while time.monotonic() < deadline:
                    if marker.exists():
                        stopped = {int(line) for line in marker.read_text().splitlines()}
                    if expected <= stopped:
                        break
                    time.sleep(0.02)
                self.assertTrue(expected <= stopped, (expected, stopped))

    def test_natural_child_exit_restores_the_host_terminal(self):
        with self.probe("exit", rows=6, columns=40) as session:
            self.assertEqual(session.wait_exit(), 0)
            self.assertIn(b"\033[?1049l", session.transcript)
            restored = termios.tcgetattr(session.master)
            self.assertEqual(restored[3] & (termios.ECHO | termios.ICANON),
                             session.original_termios[3] &
                             (termios.ECHO | termios.ICANON))


if __name__ == "__main__":
    unittest.main(verbosity=2)
