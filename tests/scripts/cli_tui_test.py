#!/usr/bin/env python3
"""Drives `huxerui create` on a pseudo-terminal and checks what a person would see.

TEMPORARY -- PR 9 ONLY, NOT FOR UPSTREAM. This suite and .github/workflows/cli-tui.yml exist to check the redesigned
create prompts on real terminal back ends (a POSIX pty on Linux and macOS, ConPTY on Windows) while PR 9 is reviewed.
Remove both before the branch is proposed upstream.

    pip install pyte            # and pywinpty on Windows
    python3 tests/scripts/cli_tui_test.py path/to/huxerui

The screen is emulated by pyte, so the checks read cells, colors, and the cursor rather than escape sequences. The
raw stream is inspected only on POSIX hosts, where it is exactly what the CLI wrote; ConPTY re-renders it.
"""

import codecs
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

import pyte

WINDOWS = os.name == "nt"
if not WINDOWS:
    import fcntl
    import struct
    import termios

CLI = None
TIMEOUT = 20.0

VIOLET = "5b51e6"
CPP_BLUE = "659ad2"
TEAL = "2aa198"

UP, DOWN, ENTER, ESCAPE, BACKSPACE, CTRL_C = "\x1b[A", "\x1b[B", "\r", "\x1b", "\x7f", "\x03"


def environment(unicode=True, color=True, **extra):
    """A host environment reduced to what the CLI's terminal detection reads, so the runner's own does not leak in."""
    keep = ("PATH", "SYSTEMROOT", "SYSTEMDRIVE", "WINDIR", "TEMP", "TMP", "USERPROFILE", "HOME", "LOCALAPPDATA",
            "APPDATA", "COMSPEC", "PATHEXT")
    env = {name: os.environ[name] for name in keep if name in os.environ}
    env["TERM"] = "xterm-256color"
    if color:
        env["COLORTERM"] = "truecolor"
    else:
        env["NO_COLOR"] = "1"
    if WINDOWS:
        if unicode:
            env["WT_SESSION"] = "cli-tui-test"
    else:
        env["LANG"] = "C.UTF-8" if unicode else "C"
    env.update(extra)
    return env


class Terminal:
    """One CLI process on a pseudo-terminal of a fixed size, its output kept as a pyte screen and as text."""

    def __init__(self, arguments, directory, env, columns=80, rows=40):
        self.screen = pyte.Screen(columns, rows)
        self.stream = pyte.Stream(self.screen)
        self.raw = ""
        self.output = queue.Queue()
        self.closing = threading.Event()
        command = [CLI, *arguments]
        if WINDOWS:
            from winpty import PtyProcess

            self.process = PtyProcess.spawn(command, cwd=str(directory), env=env, dimensions=(rows, columns))
            self.reader = threading.Thread(target=self._read_conpty, daemon=True)
        else:
            self.master, self.slave = os.openpty()
            fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
            self.process = subprocess.Popen(command, cwd=directory, env=env, stdin=self.slave, stdout=self.slave,
                                            stderr=self.slave, start_new_session=True)
            self.reader = threading.Thread(target=self._read_pty, daemon=True)
        self.reader.start()

    def _read_pty(self):
        import select

        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        while not self.closing.is_set():
            if select.select([self.master], [], [], 0.05)[0]:
                try:
                    data = os.read(self.master, 4096)
                except OSError:
                    return
                self.output.put(decoder.decode(data))

    def _read_conpty(self):
        while not self.closing.is_set():
            try:
                data = self.process.read(4096)
            except EOFError:
                return
            if data:
                self.output.put(data)

    def pump(self, wait=0.05):
        deadline = time.monotonic() + wait
        while True:
            try:
                text = self.output.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                return
            self.raw += text
            self.stream.feed(text)

    def rows(self):
        return [line.rstrip() for line in self.screen.display]

    def text(self):
        return "\n".join(self.rows())

    def wait_for(self, needle):
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            self.pump()
            if needle in self.text():
                # Let the frame that showed the needle finish, including where it leaves the cursor.
                self.pump(0.15)
                return
        raise AssertionError(f"{needle!r} never appeared; the screen was:\n{self.text()}")

    def send(self, keys):
        if WINDOWS:
            self.process.write(keys)
        else:
            os.write(self.master, keys.encode())
        self.pump(0.15)

    def finish(self):
        deadline = time.monotonic() + TIMEOUT
        while self.exit_code() is None:
            if time.monotonic() > deadline:
                self.close()
                raise AssertionError(f"the CLI did not exit; the screen was:\n{self.text()}")
            self.pump()
        self.pump(0.3)
        return self.exit_code()

    def exit_code(self):
        if WINDOWS:
            return None if self.process.isalive() else self.process.exitstatus
        return self.process.poll()

    def local_modes(self):
        return termios.tcgetattr(self.master)[3]

    def close(self):
        self.closing.set()
        if WINDOWS:
            if self.process.isalive():
                self.process.terminate(force=True)
        else:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.reader.join(1)
            os.close(self.master)
            os.close(self.slave)


class CreatePromptTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix="cli-tui-"))
        # An empty SDK home: no agent skill is installed, so the agents question starts with nothing checked.
        self.home = self.directory / "home"
        self.home.mkdir()
        self.work = self.directory / "work"
        self.work.mkdir()
        self.terminals = []

    def tearDown(self):
        for terminal in self.terminals:
            terminal.close()

    def start(self, env, columns=80, rows=40, arguments=("create",)):
        env = dict(env, HUXERUI_HOME=str(self.home))
        terminal = Terminal(list(arguments), self.work, env, columns, rows)
        self.terminals.append(terminal)
        return terminal

    def cells(self, terminal, word):
        """The cells of the first occurrence of `word` on the screen."""
        for y, row in enumerate(terminal.screen.display):
            x = row.find(word)
            if x >= 0:
                return [terminal.screen.buffer[y][x + offset] for offset in range(len(word))]
        self.fail(f"{word!r} is not on the screen:\n{terminal.text()}")

    def test_defaults_render_aligned_answers_and_create_the_app(self):
        terminal = self.start(environment())
        terminal.wait_for("Project kind")
        header = terminal.rows()[0]
        self.assertIn("HuxerUI", header)
        self.assertIn("create a project", header)
        self.assertTrue(all(cell.fg == VIOLET and cell.bold for cell in self.cells(terminal, "HuxerUI")))

        # Moving the selection redraws the question in place rather than below the old frame.
        for key in (DOWN, UP, DOWN, DOWN):
            terminal.send(key)
        self.assertEqual(sum("Project kind" in row for row in terminal.rows()), 1, terminal.text())
        terminal.send(ENTER)

        terminal.wait_for("Project name")
        field = next(y for y, row in enumerate(terminal.rows()) if "hello-hui" in row and "Project" not in row)
        self.assertEqual(terminal.screen.cursor.y, field, terminal.text())
        start = terminal.screen.cursor.x
        terminal.send("abc")
        self.assertIn("abc", terminal.rows()[field])
        self.assertEqual(terminal.screen.cursor.x, start + 3)
        terminal.send(BACKSPACE * 3)
        self.assertEqual(terminal.screen.cursor.x, start)
        terminal.send(ENTER)

        terminal.wait_for("Build system")
        self.assertTrue(all(cell.fg == TEAL for cell in self.cells(terminal, "headers")))
        self.assertTrue(all(cell.fg == CPP_BLUE for cell in self.cells(terminal, "C++ modules")))
        self.assertIn("cmake + mcpp", terminal.text())
        terminal.send(ENTER)

        terminal.wait_for("Platforms")
        terminal.send(ENTER)
        terminal.wait_for("Agent skills")
        terminal.send(ENTER)
        terminal.wait_for("Create hello-hui?")
        terminal.send(ENTER)
        self.assertEqual(terminal.finish(), 0, terminal.text())

        self.assertTrue((self.work / "hello-hui" / "CMakeLists.txt").is_file(), terminal.text())
        rows = terminal.rows()
        answers = {question: next(row for row in rows if question in row)
                   for question in ("Project kind", "Project name", "Build system", "Platforms", "Agent skills")}
        self.assertIn("app", answers["Project kind"])
        self.assertIn("hello-hui", answers["Project name"])
        self.assertIn("cmake", answers["Build system"])
        # Every answer starts in one column, whatever the length of its question.
        columns = {len(row) - len(row[row.index(question) + len(question):].lstrip())
                   for question, row in answers.items()}
        self.assertEqual(len(columns), 1, "\n".join(answers.values()))
        self.assertIn("Create hello-hui?  yes", terminal.text())
        self.assertIn("same as", terminal.text())

        if not WINDOWS:
            raw = terminal.raw
            self.assertIn("\x1b[5 q", raw, "the text field shows a blinking bar cursor")
            self.assertGreater(raw.rfind("\x1b[0 q"), raw.rfind("\x1b[5 q"), "the cursor shape is restored")
            self.assertGreater(raw.rfind("\x1b[?25h"), raw.rfind("\x1b[?25l"), "the cursor is shown again")

    def test_a_library_defaults_to_both_build_systems(self):
        terminal = self.start(environment())
        terminal.wait_for("Project kind")
        terminal.send(DOWN + ENTER)
        terminal.wait_for("Project name")
        terminal.send(ENTER)
        terminal.wait_for("Build system")
        selected = next(row for row in terminal.rows() if "cmake + mcpp" in row)
        self.assertIn("headers & C++ modules", selected)
        self.assertIn("(#include & import)", selected)
        self.assertTrue(all(cell.fg == VIOLET for cell in self.cells(terminal, "cmake + mcpp")),
                        "the initial choice is the library's dual declaration")
        terminal.send(ESCAPE)
        self.assertNotEqual(terminal.finish(), 0)

    def test_escape_and_ctrl_c_cancel_without_writing_and_restore_the_terminal(self):
        keys = [ESCAPE] if WINDOWS else [ESCAPE, CTRL_C]
        for key in keys:
            with self.subTest(key=repr(key)):
                terminal = self.start(environment())
                terminal.wait_for("Project kind")
                terminal.send(DOWN + ENTER)
                terminal.wait_for("Project name")
                terminal.send(key)
                self.assertNotEqual(terminal.finish(), 0, terminal.text())
                self.assertIn("cancelled", terminal.text().lower())
                self.assertEqual(list(self.work.iterdir()), [])
                if not WINDOWS:
                    modes = terminal.local_modes()
                    self.assertTrue(modes & termios.ICANON and modes & termios.ECHO, "line mode is restored")
                    self.assertGreater(terminal.raw.rfind("\x1b[0 q"), terminal.raw.rfind("\x1b[5 q"))
                    self.assertGreater(terminal.raw.rfind("\x1b[?25h"), terminal.raw.rfind("\x1b[?25l"))

    def test_no_color_draws_in_the_default_color(self):
        terminal = self.start(environment(color=False))
        terminal.wait_for("Project kind")
        terminal.send(ENTER)
        terminal.wait_for("Project name")
        terminal.send(ENTER)
        terminal.wait_for("Build system")
        colored = {cell.fg for row in terminal.screen.buffer.values() for cell in row.values()} - {"default"}
        self.assertEqual(colored, set(), terminal.text())
        if not WINDOWS:
            self.assertNotIn("\x1b[3", terminal.raw)
            self.assertNotIn("\x1b[9", terminal.raw)
        terminal.send(ESCAPE)
        terminal.finish()

    def test_a_terminal_without_utf8_draws_ascii(self):
        terminal = self.start(environment(unicode=False))
        terminal.wait_for("Project kind")
        terminal.send(ENTER)
        terminal.wait_for("Project name")
        terminal.send(ENTER)
        terminal.wait_for("Build system")
        self.assertTrue(terminal.text().isascii(), terminal.text())
        terminal.send(ESCAPE)
        terminal.finish()

    def test_a_narrow_terminal_truncates_instead_of_wrapping(self):
        columns = 36
        terminal = self.start(environment(), columns=columns)
        terminal.wait_for("Project kind")
        terminal.send(DOWN + ENTER)
        terminal.wait_for("Project name")
        terminal.send(ENTER)
        terminal.wait_for("Build system")
        rows = terminal.rows()
        self.assertTrue(all(len(row) < columns for row in rows), "\n".join(rows))
        self.assertIn("…", terminal.text())
        self.assertEqual(sum("Build system" in row for row in rows), 1, terminal.text())
        terminal.send(UP)
        self.assertEqual(sum("Build system" in row for row in terminal.rows()), 1, terminal.text())
        terminal.send(ESCAPE)
        terminal.finish()

    def test_a_dual_project_left_to_the_default_says_so_before_it_can_fail(self):
        result = subprocess.run([CLI, "create", "app", "dual", "--build-system", "cmake,mcpp", "--agent", "none"],
                                cwd=self.work, env=dict(environment(), HUXERUI_HOME=str(self.home)),
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=TIMEOUT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.work = self.work / "dual"
        # The empty SDK home stops the CMake path, after the note has named the other choice.
        terminal = self.start(environment(), arguments=("build", "linux"))
        self.assertNotEqual(terminal.finish(), 0)
        rows = [row for row in terminal.rows() if row]
        self.assertEqual(rows[0], "│  Using cmake, the default (or --build-system mcpp)", terminal.text())
        self.assertTrue(rows[1].startswith("huxerui: "), terminal.text())

    def test_doctor_marks_each_requirement_and_what_mcpp_provides(self):
        result = subprocess.run([CLI, "create", "app", "modular", "--build-system", "mcpp", "--platform", "android",
                                 "--agent", "none"], cwd=self.work, env=dict(environment(), HUXERUI_HOME=str(self.home)),
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=TIMEOUT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.work = self.work / "modular"
        terminal = self.start(environment(), arguments=("doctor",))
        terminal.finish()
        rows = terminal.rows()
        self.assertTrue(any(row.startswith("◆  HuxerUI CLI ") for row in rows), terminal.text())
        self.assertIn("◆  Common environment", rows)
        self.assertIn("◆  Platform android", rows)
        provided = next(row for row in rows if "provided by mcpp" in row)
        self.assertIn("Android NDK", provided)
        self.assertTrue(all(cell.fg == VIOLET for cell in self.cells(terminal, "◆")))

    def test_help_is_colored_and_reads_as_without_a_terminal(self):
        plain = subprocess.run([CLI, "--help"], capture_output=True, text=True, encoding="utf-8",
                               timeout=TIMEOUT).stdout
        terminal = self.start(environment(), columns=300, rows=60, arguments=("--help",))
        terminal.finish()
        self.assertEqual("\n".join(terminal.rows()).rstrip(), plain.rstrip())
        self.assertTrue(all(cell.fg == VIOLET and cell.bold for cell in self.cells(terminal, "HuxerUI project")))
        self.assertTrue(all(cell.bold for cell in self.cells(terminal, "Usage:")))
        self.assertTrue(all(cell.fg == VIOLET for cell in self.cells(terminal, "huxerui build")))
        self.assertTrue(all(cell.fg == "808080" for cell in self.cells(terminal, "[platform-list]")))

    def test_without_a_terminal_create_applies_defaults_and_writes_no_escapes(self):
        result = subprocess.run([CLI, "create", "app", "plain", "--agent", "none"], cwd=self.work,
                                env=dict(environment(), HUXERUI_HOME=str(self.home)), stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, encoding="utf-8", timeout=TIMEOUT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("\x1b", result.stdout + result.stderr)
        self.assertTrue((self.work / "plain").is_dir())


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: cli_tui_test.py path/to/huxerui [unittest arguments]")
    CLI = str(Path(sys.argv.pop(1)).resolve())
    unittest.main()
