"""Daemon-affine shell ownership, bounded prefix capture, and shared byte cursors."""

import asyncio
import codecs
import contextlib
import math
import os
import signal
import subprocess
import threading
import time
import uuid
from pathlib import Path

from .protocol import PTCError, encode
from .transport import MAX_COMMAND_REPLY_BYTES

CAPTURE_BYTES = 64 * 1024
MAX_RECORDS = 64
MAX_ACTIVE = 16
DRAIN_SECONDS = 0.1
REAP_SECONDS = 5


def timeout(value, *, zero=True):
    """Reject invalid deadlines before performing any process or input effect."""
    if type(value) not in (int, float):
        raise ValueError("timeout_seconds must be a finite number")
    try:
        valid = math.isfinite(value) and (0 <= value if zero else 0 < value) and value <= 300
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError("timeout_seconds must be in [0, 300] (kill requires >0)")
    return value


def shell():
    """Use the same explicit shell preference as the native worker, never PATH lookup."""
    for path in ("/bin/bash", "/usr/bin/bash", "/bin/sh"):
        if os.access(path, os.X_OK) and Path(path).is_file():
            return path
    raise FileNotFoundError("run_command requires bash or sh")


class ManagedCommand:
    """One owned child. All mutation happens on the daemon event loop.

    A retained record remains usable after exit. Draining continues independently
    of runner/client lifetime; read cursors do not refund the shared prefix budget.
    """

    def __init__(self, identity, process):
        self.id = identity
        self.process = process
        self.started = time.monotonic()
        self.data = {"stdout": bytearray(), "stderr": bytearray()}
        self.ends = {name: "open" for name in self.data}
        self.omitted = {name: 0 for name in self.data}
        self.cursors = {name: 0 for name in self.data}
        self.remaining = CAPTURE_BYTES
        self.killed = False
        self.initial_timeout = False
        self.closing = None
        self.loop = asyncio.get_running_loop()
        self.readers = {}
        self.streams_done = asyncio.Event()
        for pipe in (process.stdout, process.stderr, process.stdin):
            os.set_blocking(pipe.fileno(), False)
        self.pump = asyncio.create_task(self._pump())

    def complete(self):
        """Capture completion includes forced/failed ends; it does not imply no loss."""
        return all(end != "open" for end in self.ends.values())

    def finish(self, name, end):
        """Record a terminal stream reason once, then release its descriptor."""
        if self.ends[name] == "open":
            descriptor = self.readers.pop(name, None)
            if descriptor is not None:
                self.loop.remove_reader(descriptor)
            self.ends[name] = end
            getattr(self.process, name).close()
            if self.complete():
                self.streams_done.set()

    def _readable(self, name):
        """Drain ready data without starving the other pipe or control requests.

        Level-triggered readiness schedules another callback if a prolific writer
        exhausts this callback's four-chunk allowance. Once EAGAIN is reached,
        there are no more callbacks until new data or EOF arrives. Capture loss
        never stops draining the child's pipe.
        """
        for _ in range(4):
            if self.ends[name] != "open":
                return
            try:
                chunk = os.read(self.readers[name], 65536)
            except BlockingIOError:
                return
            except OSError:
                self.finish(name, "failed")
                return
            if not chunk:
                self.finish(name, "eof")
                return
            kept = min(self.remaining, len(chunk))
            self.data[name].extend(chunk[:kept])
            self.remaining -= kept
            self.omitted[name] += len(chunk) - kept

    async def _pump(self):
        """Join pipe readiness and child exit without a periodic per-command timer.

        One blocking wait thread per live child avoids event-loop polling and
        contention with the coordinator's shared executor. The thread touches no
        capture state; it posts completion only after wait() has reaped the child.
        All descriptors and capture metadata remain event-loop-affine.
        """
        exited = self.loop.create_future()

        def notify(error):
            if exited.done():
                return
            if error is None:
                exited.set_result(None)
            else:
                exited.set_exception(error)

        def reap():
            error = None
            try:
                self.process.wait()
            except Exception as exc:
                error = exc
            self.loop.call_soon_threadsafe(notify, error)

        try:
            for name in self.data:
                if self.ends[name] == "open":
                    descriptor = getattr(self.process, name).fileno()
                    self.loop.add_reader(descriptor, self._readable, name)
                    self.readers[name] = descriptor
            threading.Thread(target=reap, name="ptc-command-wait", daemon=True).start()
            await exited
            self.process.stdin.close()
            await self.streams_done.wait()
        finally:
            for name in self.ends:
                self.finish(name, "failed")

    async def wait(self, seconds):
        """Wait for both exit and capture settlement without consuming output."""
        deadline = time.monotonic() + seconds
        while self.process.poll() is None or not self.complete():
            if time.monotonic() >= deadline:
                return True
            await asyncio.sleep(min(0.01, max(0, deadline - time.monotonic())))
        return False

    def snapshot(self):
        """Flat metadata; output is populated only by read()."""
        code = self.process.poll()
        truncated = any(self.omitted.values()) or any(
            e in ("stopped", "failed") for e in self.ends.values()
        )
        hints = []
        if truncated:
            hints.append(
                "Capture truncated/incomplete; redirect large output to files and read selected portions."
            )
        return dict(
            id=self.id,
            status="running" if code is None else ("killed" if self.killed else "exited"),
            pid=self.process.pid,
            exit_code=code if code is not None and code >= 0 else None,
            termination_signal=-code if code is not None and code < 0 else None,
            timed_out=self.initial_timeout,
            duration_seconds=time.monotonic() - self.started,
            stdout="",
            stderr="",
            output_complete=self.complete(),
            output_truncated=truncated,
            stdout_end=self.ends["stdout"],
            stderr_end=self.ends["stderr"],
            stdout_omitted_bytes=self.omitted["stdout"],
            stderr_omitted_bytes=self.omitted["stderr"],
            stdout_cursor=self.cursors["stdout"],
            stderr_cursor=self.cursors["stderr"],
            read_limited=False,
            input_bytes_written=0,
            input_complete=True,
            released=False,
            hints=hints,
        )

    def read(self, full, max_bytes):
        """Decode retained bytes, postponing incomplete growing UTF-8 suffixes.

        Build a reply within the private IPC budget before advancing any cursor.
        EOF/truncation flushes undecodable tails, but a caller's small read budget
        alone does not turn a split code point into replacement characters.
        """
        if max_bytes is None:
            max_bytes = CAPTURE_BYTES if full else 16384
        if (
            type(full) is not bool
            or type(max_bytes) is not int
            or not 1 <= max_bytes <= CAPTURE_BYTES
        ):
            raise ValueError("full must be bool; max_bytes must be in [1, 65536]")
        budget = max_bytes
        while True:
            result = self.snapshot()
            remaining = budget
            positions = {}
            for name in self.data:
                start = 0 if full else self.cursors[name]
                end = min(len(self.data[name]), start + remaining)
                final = end == len(self.data[name]) and (
                    self.ends[name] != "open" or self.omitted[name] > 0
                )
                decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
                result[name] = decoder.decode(bytes(self.data[name][start:end]), final=final)
                deferred = len(decoder.getstate()[0])
                positions[name] = end - deferred
                remaining -= end - start
                result["read_limited"] |= positions[name] < len(self.data[name])
            if result["read_limited"]:
                result["hints"].append(
                    "Read is partial; read again, or increase max_bytes for a deferred UTF-8 character."
                )
            if len(encode({"result": result})) <= MAX_COMMAND_REPLY_BYTES:
                break
            budget //= 2
        if not full:
            self.cursors.update(positions)
            result.update(
                stdout_cursor=self.cursors["stdout"], stderr_cursor=self.cursors["stderr"]
            )
        return result

    async def send(self, text, close_stdin, seconds):
        """Write atomic UTF-8 chunks so an expired send can resume through the text API.

        Nonblocking pipe writes up to PC_PIPE_BUF are all-or-nothing. End each
        chunk on a character boundary and report the actual bytes written, never
        a rounded-down count that hides an already transmitted continuation.
        """
        if not isinstance(text, str) or type(close_stdin) is not bool:
            raise ValueError("text must be str; close_stdin must be bool")
        data = text.encode("utf-8")
        if len(data) > CAPTURE_BYTES:
            raise ValueError("stdin write exceeds 64 KiB")
        if self.process.stdin.closed:
            raise ValueError("stdin is closed")
        pipe_buf = os.fpathconf(self.process.stdin.fileno(), "PC_PIPE_BUF")
        if pipe_buf < 4:
            raise OSError("stdin pipe cannot atomically accept a UTF-8 character")
        count = 0
        deadline = time.monotonic() + seconds
        while count < len(data):
            if count and seconds > 0 and time.monotonic() >= deadline:
                break
            end = min(len(data), count + pipe_buf)
            while end < len(data) and data[end] & 0xC0 == 0x80:
                end -= 1
            try:
                expected = end - count
                written = os.write(self.process.stdin.fileno(), data[count:end])
                count += written
                if written != expected:
                    # Do not turn an unexpected OS/transport contract violation
                    # into a supposedly resumable text result.
                    raise OSError("unexpected short atomic pipe write")
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    break
                await asyncio.sleep(min(0.005, max(0, deadline - time.monotonic())))
                continue
            except (OSError, ValueError) as exc:
                raise OSError(f"stdin write failed after {count} bytes") from exc
            if count == len(data) or (seconds > 0 and time.monotonic() >= deadline):
                break
            # Zero means no waiting for capacity, not just one small write.
            # The 64 KiB input budget bounds this loop even without backpressure.
            await asyncio.sleep(0)
        if close_stdin and count == len(data):
            self.process.stdin.close()
        result = self.snapshot()
        result.update(input_bytes_written=count, input_complete=count == len(data))
        if count < len(data):
            result["hints"].append(
                "stdin partially written at a UTF-8 boundary; send only the remaining suffix."
            )
        return result

    async def _close(self):
        """Best effort kill/reap and bounded pipe drain; preserve the first failure."""
        failure = None
        try:
            self.process.stdin.close()
        except Exception as exc:
            failure = exc
        try:
            if self.process.poll() is None:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                    self.killed = True
                except ProcessLookupError:
                    pass
                except OSError as exc:
                    failure = failure or exc
                    # Still attempt to settle the directly owned child if group
                    # signalling failed. This does not claim descendant cleanup.
                    self.process.kill()
                    self.killed = True
            deadline = time.monotonic() + REAP_SECONDS
            while self.process.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError("command reaping deadline expired")
                await asyncio.sleep(0.01)
        except Exception as exc:
            failure = failure or exc
        try:
            await self.wait(DRAIN_SECONDS)
        except Exception as exc:
            failure = failure or exc
        finally:
            for name in self.ends:
                try:
                    self.finish(name, "stopped")
                except Exception as exc:
                    failure = failure or exc
            if self.process.poll() is None:
                self.pump.cancel()
            outcome = (await asyncio.gather(self.pump, return_exceptions=True))[0]
            if isinstance(outcome, Exception):
                failure = failure or outcome
        if failure:
            raise failure

    async def close(self):
        """One shared cleanup task; cancellation cannot abandon acquired resources."""
        if self.closing is None:
            self.closing = asyncio.create_task(self._close())
        try:
            await asyncio.shield(self.closing)
        except asyncio.CancelledError:
            await self.closing
            raise


class Commands:
    """Bounded per-daemon registry, also one lifecycle Resource.

    Registration happens before any wait. Created IDs are appended to the current
    execution's effect list so a cancelled/failed checkpoint cannot hide a launch.
    """

    def __init__(self, resources):
        self.resources = resources
        self.records = {}
        self.registered = False
        self.closed = False

    def alive(self):
        """Exited records still own retained output until release or resource expiry."""
        return bool(self.records)

    async def close(self):
        """Close every record even if one fails, then surface the first failure."""
        self.closed = True
        results = await asyncio.gather(
            *(item.close() for item in self.records.values()), return_exceptions=True
        )
        self.records.clear()
        for result in results:
            if isinstance(result, BaseException):
                raise result

    async def dispatch(self, value, context, created):
        """Private runner requests only; execution context comes from daemon admission."""
        if self.closed:
            raise PTCError("stopping", "command registry is closed")
        operation = value.get("operation")
        if operation == "launch":
            command, directory = value.get("command"), value.get("working_directory")
            if not isinstance(command, str) or not command or "\0" in command:
                raise ValueError("command must be nonempty text without NUL")
            if directory is not None and (
                not isinstance(directory, str) or not directory or "\0" in directory
            ):
                raise ValueError("working_directory must be nonempty text without NUL")
            action = value.get("timeout_action")
            if action not in ("keep", "kill"):
                raise ValueError("timeout_action must be keep or kill")
            seconds = timeout(value.get("timeout_seconds"), zero=action == "keep")
            if (
                len(self.records) >= MAX_RECORDS
                or len(created) >= MAX_RECORDS
                or sum(
                    item.process.poll() is None or not item.complete()
                    for item in self.records.values()
                )
                >= MAX_ACTIVE
            ):
                raise PTCError("command_limit", "command capacity reached; stop/release commands")
            identity = f"{self.resources.generation}:{uuid.uuid4()}"
            process = subprocess.Popen(
                [shell(), "-c", command],
                cwd=os.path.join(context["cwd"], directory)
                if directory is not None
                else context["cwd"],
                env=context["environment"],
                umask=context["umask"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
                close_fds=True,
            )
            try:
                item = ManagedCommand(identity, process)
            except BaseException as exc:
                try:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=REAP_SECONDS)
                except BaseException as cleanup:
                    exc.add_note(f"command acquisition cleanup also failed: {cleanup}")
                finally:
                    for pipe in (process.stdin, process.stdout, process.stderr):
                        try:
                            pipe.close()
                        except BaseException as cleanup:
                            exc.add_note(f"command pipe cleanup also failed: {cleanup}")
                raise
            self.records[identity] = item
            created.append(identity)
            if not self.registered:
                self.resources.add(self)
                self.registered = True
            self.resources.touch()
            try:
                item.initial_timeout = await item.wait(seconds)
                if item.initial_timeout and action == "kill":
                    await item.close()
            except BaseException:
                if action == "kill":
                    await item.close()
                raise
            return {"id": identity}
        identity = value.get("id")
        if not isinstance(identity, str) or identity not in self.records:
            raise PTCError(
                "resource_lost", "command is released, foreign or from an old daemon generation"
            )
        item = self.records[identity]
        self.resources.touch()
        if operation == "get":
            return {"id": identity}
        if operation == "status":
            return item.snapshot()
        if operation == "read":
            return item.read(value.get("full"), value.get("max_bytes"))
        if operation == "wait":
            expired = await item.wait(timeout(value.get("timeout_seconds")))
            return dict(item.snapshot(), timed_out=expired)
        if operation == "send":
            return await item.send(
                value.get("text"), value.get("close_stdin"), timeout(value.get("timeout_seconds"))
            )
        if operation == "stop":
            await item.close()
            return item.snapshot()
        if operation == "release":
            if item.process.poll() is None:
                return item.snapshot()
            await item.close()
            result = dict(item.snapshot(), released=True)
            del self.records[identity]
            return result
        raise ValueError("unknown command operation")
