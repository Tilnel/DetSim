#!/usr/bin/env python3
"""Session reply semantics and measured transcript read cost; no tracer needed."""
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
from _support import session as runner


class Clock:
    def __init__(self, log, chunks=()):
        self.now = 0
        self.log = log
        self.chunks = list(chunks)
        self.sleeps = 0
        self.delays = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds
        self.sleeps += 1
        self.delays.append(seconds)
        if self.chunks:
            with self.log.open("ab") as output:
                output.write(self.chunks.pop(0))


class SessionFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="detsim-case-session-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.session = runner.Session.__new__(runner.Session)
        self.session.directory = self.directory
        self.session.log = self.directory / "transcript.log"
        self.session.log.write_bytes(b"")
        self.session.offset = 0
        self.session.started = 0
        self.session.timeout = 1
        self.session.commands = []
        self.session.proc = SimpleNamespace(stdin=io.BytesIO(), returncode=None,
                                            poll=mock.Mock(return_value=None))

    def send(self, command=None, chunks=()):
        clock = Clock(self.session.log, chunks)
        with mock.patch.object(runner.time, "monotonic", clock.monotonic), \
             mock.patch.object(runner.time, "sleep", clock.sleep):
            reply = self.session.send(command)
        return reply, clock

    def append(self, data):
        with self.session.log.open("ab") as output:
            output.write(data)


class SessionFunctionalTests(SessionFixture):
    def test_wait_backoff_starts_short_is_capped_and_preserves_timeout(self):
        clock = Clock(self.session.log)
        with mock.patch.object(runner.time, "monotonic", clock.monotonic), \
             mock.patch.object(runner.time, "sleep", clock.sleep):
            with self.assertRaisesRegex(TimeoutError, "command deadline: p missing"):
                self.session.send("p missing")
        self.assertEqual(clock.delays[:4], [.0001, .0002, .0004, .0008])
        self.assertTrue(all(0 < delay <= .005 for delay in clock.delays))
        self.assertIn(.005, clock.delays)
        self.assertGreaterEqual(clock.now, self.session.timeout)
        self.assertLess(clock.now, self.session.timeout + .0051)

    def test_locale_decoder_and_byte_offsets_preserve_non_utf8_replies(self):
        self.append(b"caf\xe9\r\n(ptmc) ")
        with mock.patch.object(runner.locale, "getpreferredencoding", return_value="latin-1") as encoding:
            self.assertEqual(self.send()[0], "caf\u00e9\n(ptmc) ")
            self.append(b"\xf6(ptmc) ")
            self.assertEqual(self.send("si")[0], "\u00f6(ptmc) ")
        self.assertEqual(encoding.call_args_list, [mock.call(False), mock.call(False)])

    def test_returns_current_reply_without_previous_consumed_output(self):
        self.append(b"first reply\n(ptmc) ")
        self.assertEqual(self.send()[0], "first reply\n(ptmc) ")
        self.append(b"second reply\n(ptmc) ")
        self.assertEqual(self.send()[0], "second reply\n(ptmc) ")

    def test_records_commands_and_writes_utf8_with_one_newline(self):
        self.append(b"(ptmc) ")
        self.assertEqual(self.send("p \u03bb")[0], "(ptmc) ")
        self.assertEqual(self.session.commands, ["p \u03bb"])
        self.assertEqual(self.session.proc.stdin.getvalue(), "p \u03bb\n".encode())

    def test_initial_prompt_does_not_record_or_write_a_command(self):
        self.append(b"welcome\n(ptmc) ")
        self.assertEqual(self.send()[0], "welcome\n(ptmc) ")
        self.assertEqual(self.session.commands, [])
        self.assertEqual(self.session.proc.stdin.getvalue(), b"")

    def test_waits_until_complete_prompt_including_its_trailing_space(self):
        self.append(b"reply(ptm")
        reply, clock = self.send(chunks=[b"c)", b" "])
        self.assertEqual(reply, "reply(ptmc) ")
        self.assertEqual(clock.sleeps, 2)

    def test_prompt_inside_reply_does_not_hide_subsequent_output(self):
        self.append(b"embedded (ptmc) token\n")
        self.assertEqual(self.send(chunks=[b"completed(ptmc) "])[0],
                         "embedded (ptmc) token\ncompleted(ptmc) ")

    def test_decodes_utf8_split_across_polling_reads_without_replacement(self):
        self.append(b"emoji \xf0\x9f")
        reply, clock = self.send(chunks=[b"\x98", b"\x80(ptmc) "])
        self.assertEqual(reply, "emoji \U0001f600(ptmc) ")
        self.assertEqual(clock.sleeps, 2)

    def test_replaces_malformed_utf8_and_preserves_next_reply_boundary(self):
        self.append(b"bad\xff\xc3(\xe2(ptmc) ")
        self.assertEqual(self.send()[0], "bad\ufffd\ufffd(\ufffd(ptmc) ")
        self.append("\u4e0b\u4e00\u6761(ptmc) ".encode())
        self.assertEqual(self.send()[0], "\u4e0b\u4e00\u6761(ptmc) ")

    def test_normalizes_crlf_and_cr_without_misplacing_next_reply(self):
        self.append("\u03bb\r\nfirst\r(ptmc) ".encode())
        self.assertEqual(self.send()[0], "\u03bb\nfirst\n(ptmc) ")
        self.append(b"next\rstep\r\n(ptmc) ")
        self.assertEqual(self.send()[0], "next\nstep\n(ptmc) ")

    def test_normalizes_crlf_split_between_polling_reads_once(self):
        self.append(b"first\r")
        self.assertEqual(self.send(chunks=[b"\nsecond\r", b"third(ptmc) "])[0],
                         "first\nsecond\nthird(ptmc) ")

    def test_complete_prompt_is_returned_even_when_process_has_exited(self):
        self.append(b"last reply(ptmc) ")
        self.session.proc.returncode = 0
        self.session.proc.poll.return_value = 0
        self.assertEqual(self.send()[0], "last reply(ptmc) ")

    def test_eof_diagnostic_contains_last_4000_current_reply_characters(self):
        self.append(b"already consumed(ptmc) ")
        self.send()
        tail = ("\u8bca" * 4100 + "\r\nerror\ufffd").replace("\r\n", "\n")
        self.append(("\u8bca" * 4100 + "\r\nerror").encode() + b"\xff")
        self.session.proc.returncode = 7
        self.session.proc.poll.return_value = 7
        with self.assertRaises(runner.CaseFailure) as caught:
            self.send("si")
        self.assertEqual(str(caught.exception), tail[-4000:])
        self.assertEqual(self.session.commands, ["si"])

    def test_empty_current_reply_at_eof_reports_process_exit_code(self):
        self.append(b"consumed(ptmc) ")
        self.send()
        self.session.proc.returncode = 9
        self.session.proc.poll.return_value = 9
        with self.assertRaisesRegex(runner.CaseFailure, "^tracer exited 9$"):
            self.send()

    def test_broken_stdin_pipe_keeps_command_and_returns_available_reply(self):
        self.append(b"available(ptmc) ")
        self.session.proc.stdin = mock.Mock()
        self.session.proc.stdin.write.side_effect = BrokenPipeError
        self.assertEqual(self.send("q")[0], "available(ptmc) ")
        self.assertEqual(self.session.commands, ["q"])

    def test_timeout_keeps_submitted_command_and_reports_its_deadline(self):
        self.session.timeout = 0
        with self.assertRaisesRegex(TimeoutError, "^command deadline: si 0$"):
            self.send("si 0")
        self.assertEqual(self.session.commands, ["si 0"])
        self.assertEqual(self.session.proc.stdin.getvalue(), b"si 0\n")


class ReadMeter:
    """Count returned bytes for real file reads, without requiring seek calls."""
    def __init__(self, stream, meter):
        self.stream = stream
        self.meter = meter

    def read(self, *args, **kwargs):
        data = self.stream.read(*args, **kwargs)
        self.meter.append(len(data) if isinstance(data, bytes) else len(data.encode()))
        return data

    def __getattr__(self, name):
        return getattr(self.stream, name)

    def __enter__(self):
        self.stream.__enter__()
        return self

    def __exit__(self, *args):
        return self.stream.__exit__(*args)


class SessionReadBoundTests(SessionFixture):
    def test_polling_small_reply_does_not_reread_large_consumed_prefix(self):
        self.append(b"X" * (1024 * 1024) + b"(ptmc) ")
        self.send()
        self.append(b"next ")
        reads = []
        original_open = Path.open

        def measured_open(path, mode="r", *args, **kwargs):
            stream = original_open(path, mode, *args, **kwargs)
            return ReadMeter(stream, reads) if path == self.session.log and "r" in mode else stream

        with mock.patch.object(Path, "open", measured_open):
            reply, clock = self.send("si", chunks=[b"small ", b"reply(ptmc) "])
        self.assertEqual(reply, "next small reply(ptmc) ")
        self.assertEqual(clock.sleeps, 2)
        self.assertGreater(sum(reads), 0, "read instrumentation did not observe transcript bytes")
        self.assertLessEqual(sum(reads), 3 * len(reply.encode()),
                             f"polling consumed {sum(reads)} bytes for {len(reply.encode())}-byte reply; reads={reads}")


class CompletedSearchProtocolTests(unittest.TestCase):
    source, target, other, first_parent = (letter * 16 for letter in "abcd")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ds-search-receipt-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.receipt = {"schema": 1, "source": self.source, "target": self.target,
                        "process": 0, "thread": 1, "choice": -1,
                        "action": "normal", "exit_codes": [0]}
        self.footers = [self.receipt]
        self.search_error = ""
        self.replay_error = False
        self.sessions = []

    def execute(self):
        owner = self
        current_hash = runner.Session.current_hash

        class FakeSession:
            def __init__(self, *_args, **_kwargs):
                self.commands = []
                self.state = owner.source
                self.actor = 0
                self.log = owner.directory / "transcript.log"
                self.log.write_text("")
                owner.sessions.append(self)

            def send(self, text=None):
                if text is None:
                    return "(ptmc) "
                self.commands.append(text)
                if text.startswith("dfs "):
                    self.state = owner.other
                    return ("Complete explore all 7 sys_states (new: 7)\n" +
                            "".join("SEARCH_EXITED " + json.dumps(item) + "\n" for item in owner.footers) +
                            owner.search_error + "(ptmc) ")
                if text == "info current":
                    if self.state == owner.source:
                        return "Current state " + owner.source + " (no predecessor).\n(ptmc) "
                    if self.state == owner.target:
                        # TSS may retain another first incoming edge. The
                        # receipt identifies the edge executed by this search.
                        return (owner.first_parent + " => " + owner.target +
                                "\n Tracee 0: [TID:2] exit(0)\n(ptmc) ")
                    return (owner.source + " => " + owner.other +
                            "\n Tracee 0: [TID:1] futex(0, 0) = 0\n(ptmc) ")
                if text.startswith("load "):
                    self.state = text.split()[1]
                    self.actor = 0  # predecessor's saved selection is main.
                    return "Loaded state " + self.state + "\n(ptmc) "
                if text == "sw 0":
                    return "Switched to process 0 (pid=123)\n(ptmc) "
                if text.startswith("thread "):
                    self.actor = int(text.split()[1])
                    return f"Switched to thread {self.actor} (TID={self.actor + 1}) in process 0\n(ptmc) "
                if text == "si 0":
                    if self.state == owner.target:
                        return "Tracee #0 has exited with status 0.\n(ptmc) "
                    if self.state != owner.source or self.actor != 1:
                        raise runner.CaseFailure("completion replay selected wrong source/actor")
                    self.state = owner.target
                    if owner.replay_error:
                        return "DetSim state/execution error: injected failure\n(ptmc) "
                    return "exit(0) = ?\n(ptmc) "
                raise AssertionError("unexpected command: " + text)

            def current_hash(self):
                return current_hash(self)

            def close(self):
                return 0

        with mock.patch.object(runner, "Session", FakeSession):
            return runner.search(Path("/unused/tracer"), ["fixture"], "checker",
                                 self.directory, "full", 10, depth=100)

    def test_receipt_selects_searched_terminal_and_replays_explicit_nonmain_actor(self):
        result = self.execute()
        self.assertEqual(result["status"], "pass", result)
        self.assertEqual(result["predecessor"], self.source)
        self.assertEqual(result["completion"], self.target)
        self.assertEqual(result["replay"], "same_process_hash_match_3x")
        commands = self.sessions[0].commands
        self.assertIn("load " + self.target, commands)
        self.assertEqual(commands.count("load " + self.source), 3)
        self.assertEqual(commands.count("thread 1"), 3)
        self.assertNotIn("load " + self.first_parent, commands)

    def test_no_receipt_keeps_nonterminal_depth_bound_non_green(self):
        self.footers = []
        result = self.execute()
        self.assertEqual(result["status"], "bounded", result)
        self.assertIsNone(result["guest_exit"])

    def test_invalid_receipts_fail_without_current_cursor_fallback(self):
        mutations = [("schema", True), ("schema", 2), ("source", "0" * 16),
                     ("target", self.source), ("source", "bad"), ("process", True),
                     ("process", 1), ("thread", -1), ("thread", 64), ("thread", True),
                     ("choice", -3), ("choice", 1.5), ("choice", True),
                     ("action", "unknown"), ("action", "timeout"),
                     ("choice", -2), ("exit_codes", [7]), ("exit_codes", []),
                     ("exit_codes", [False])]
        original = dict(self.receipt)
        for key, value in mutations:
            with self.subTest(field=key, value=value):
                changed = dict(original)
                changed[key] = value
                self.footers = [changed]
                result = self.execute()
                self.assertNotIn(result["status"], ("pass", "violation", "bounded"), result)

    def test_duplicate_receipts_cannot_select_a_successful_alternative(self):
        bad = dict(self.receipt, exit_codes=[7])
        self.footers = [bad, self.receipt]
        self.assertNotIn(self.execute()["status"], ("pass", "violation", "bounded"))

    def test_search_and_replay_execution_errors_remain_non_green(self):
        self.search_error = "DetSim state/execution error: injected failure\n"
        self.assertNotEqual(self.execute()["status"], "pass")
        self.search_error = ""
        self.replay_error = True
        self.assertNotEqual(self.execute()["status"], "pass")


if __name__ == "__main__":
    unittest.main()
