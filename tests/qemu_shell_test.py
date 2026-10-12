#!/usr/bin/env python3
"""Drive the real QEMU UART shell; only Python's standard library is needed."""

import argparse
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import time


class StageShell:
    def __init__(self, qemu, kernel):
        self.process = subprocess.Popen(
            shlex.split(qemu) + [
                "-machine", "virt", "-m", "128M", "-smp", "1",
                "-nographic", "-bios", "none", "-kernel", str(Path(kernel).resolve()),
                "-monitor", "none", "-serial", "stdio",
            ],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.pending = b""
        self.transcript = bytearray()
        self.checks = 0

    def prompt(self, marker=b"os> "):
        deadline = time.monotonic() + 10
        while True:
            index = self.pending.find(marker)
            if index >= 0:
                output, self.pending = self.pending[:index], self.pending[index + len(marker):]
                return output.decode("utf-8", errors="replace").replace("\r", "")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"Timed out waiting for prompt {marker!r}")
            ready, _, _ = select.select([self.process.stdout], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                raise AssertionError("QEMU exited before the shell returned to its prompt")
            self.pending += chunk
            self.transcript.extend(chunk)
            if len(self.pending) > 1024 * 1024:
                raise AssertionError("Unexpectedly large shell output")

    def command(self, text, end="\n", marker=b"os> "):
        self.process.stdin.write((text + end).encode("ascii"))
        self.process.stdin.flush()
        return self.prompt(marker)

    def expect(self, text, fragments, end="\n", marker=b"os> "):
        output = self.command(text, end, marker)
        for fragment in fragments:
            if fragment not in output:
                raise AssertionError(f"{text!r}: missing {fragment!r} in {output!r}")
        self.checks += 1
        return output

    def pid(self, text, label="pid"):
        output = self.command(text)
        match = re.search(rf"\b{label}=(\d+)", output)
        if not match:
            raise AssertionError(f"No {label} in {output!r}")
        self.checks += 1
        return int(match.group(1))

    def quit(self):
        self.process.stdin.write(b"quit\n")
        self.process.stdin.flush()
        output, _ = self.process.communicate(timeout=5)
        self.transcript.extend(output)
        if self.process.returncode != 0 or b"bye" not in output:
            raise AssertionError(f"QEMU shutdown failed: {self.process.returncode}")
        self.checks += 1

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        Path("build/shell_test.log").write_bytes(self.transcript)


def filesystem_checks(shell, baseline):
    shell.expect("pwd", ["\n/\n"])
    shell.expect("ls", ["TYPE SIZE INODE NAME", " init\n", " alt\n"])
    shell.expect("mkdir workspace", [])
    shell.expect("mkdir workspace", ["error -14"])
    shell.expect("cd workspace", [])
    shell.expect("pwd", ["\n/workspace\n"])
    shell.expect("ls", ["(empty)"])
    shell.expect("mkdir nested/", [])
    shell.expect("cd ./nested//../nested", [])
    shell.expect("pwd", ["\n/workspace/nested\n"])
    shell.expect("touch empty", [])
    shell.expect("ls", ["f 0 ", " empty\n"])
    shell.expect("cat empty", [])
    shell.expect("cd empty", ["error -12"])
    shell.expect("pwd", ["\n/workspace/nested\n"])
    shell.expect("touch empty/", ["error -12"])
    shell.expect("touch missing/file", ["error -11"])
    shell.expect("touch " + "n" * 64, ["error -7"])
    shell.expect("touch .", ["error -13"])
    shell.expect("mkdir ..", ["error -16"])
    shell.expect("rm /", ["error -16"])
    shell.expect("rm /workspace/nested", ["error -4"])
    shell.expect("ls empty", ["f 0 ", " empty\n"])
    shell.expect("ls missing", ["error -11"])
    shell.expect("cat .", ["error -13"])
    shell.expect("editor .", ["error -13"])
    shell.expect("editor /init", ["ASCII text", "file unchanged", "error -1"])
    shell.expect("editor missing/file", ["error -11"])
    shell.expect("editor missing/", ["error -12"])
    shell.expect('touch "space name"', [])
    shell.expect("ls", [" space name\n"])
    shell.expect("rm space\\ name", [])
    shell.expect('touch "unfinished', ["unterminated quote", "error -1"])
    shell.expect("touch dangling\\", ["unfinished escape", "error -1"])
    shell.expect("touch ''", ["error -1"])

    edit = b"edit> "
    text = b"text> "
    shell.expect('editor "my note"', ["(new file)", "bytes=0 lines=0"], marker=edit)
    shell.expect("a", ["single '.'"], marker=text)
    shell.expect("hello  kernel", [], marker=text)
    shell.expect("second line", [], marker=text)
    shell.expect("", [], marker=text)
    shell.expect("..", [], marker=text)
    shell.expect(".", [], marker=edit)
    shell.expect("p", ["1 | hello  kernel\n", "2 | second line\n", "3 | \n", "4 | .\n"], marker=edit)
    shell.expect("q", ["unsaved changes"], marker=edit)
    shell.expect("i 2", [], marker=text)
    shell.expect("inserted", [], marker=text)
    shell.expect(".", [], marker=edit)
    shell.expect("r 3", [], marker=text)
    shell.expect("replacement with spaces", [], marker=edit)
    shell.expect("d 4", [], marker=edit)
    shell.expect("d 0", ["invalid line number"], marker=edit)
    shell.expect("r 99", ["no such line"], marker=edit)
    shell.expect("i 18446744073709551616", ["invalid line number"], marker=edit)
    shell.expect("unknown", ["unknown command"], marker=edit)
    shell.expect("w", ["saved bytes="], marker=edit)
    shell.expect("q", [])
    expected = "hello  kernel\ninserted\nreplacement with spaces\n.\n"
    shell.expect('cat "my note"', [expected])
    shell.expect('touch "my note"', [])
    shell.expect('cat "my note"', [expected])
    shell.expect('edit "my note"', ["lines=4"], marker=edit)
    shell.expect("d 4", [], marker=edit)
    shell.expect("d 3", [], marker=edit)
    shell.expect("d 2", [], marker=edit)
    shell.expect("r 1", [], marker=text)
    shell.expect("short", [], marker=edit)
    shell.expect("wq", ["saved bytes=6"])
    output = shell.expect('cat "my note"', ["\nshort\n"])
    assert "replacement" not in output and "inserted" not in output
    shell.expect('editor "my note"', ["bytes=6 lines=1"], marker=edit)
    shell.expect("r 1", [], marker=text)
    shell.expect("discard me", [], marker=edit)
    shell.expect("q!", [])
    shell.expect('cat "my note"', ["\nshort\n"])
    shell.expect('editor "my note"', [], marker=edit)
    shell.expect("d 1", [], marker=edit)
    shell.expect("wq", ["saved bytes=0"])
    shell.expect('ls "my note"', ["f 0 "])
    shell.expect("editor never-saved", ["(new file)"], marker=edit)
    shell.expect("a", [], marker=text)
    shell.expect("staged", [], marker=text)
    shell.expect("\x03", ["^C"], end="", marker=edit)
    shell.expect("\x04", ["unsaved changes"], end="", marker=edit)
    shell.expect("q!", [])
    shell.expect("ls never-saved", ["error -11"])
    shell.expect("editor canceled", [], marker=edit)
    shell.expect("r 1", ["no such line"], marker=edit)
    shell.expect("a", [], marker=text)
    shell.expect("z" * 300, ["line too long"], marker=text)
    shell.expect("\x04", [], end="", marker=edit)
    shell.expect("p", ["(empty)"], marker=edit)
    shell.expect("\x04", [], end="")
    shell.expect("ls canceled", ["error -11"])

    # Exact editor capacity, failed growth, save/reopen, and allocation cleanup.
    shell.expect("editor large", [], marker=edit)
    shell.expect("i 1", [], marker=text)
    shell.expect("", [], marker=text)
    for _ in range(63):
        shell.expect("x" * 255, [], marker=text)
    shell.expect("x" * 254, [], marker=text)
    shell.expect("too much", ["buffer full"], marker=edit)
    shell.expect("r 1", [], marker=text)
    shell.expect("extra", ["buffer full"], marker=edit)
    shell.expect("wq", ["saved bytes=16384"])
    shell.expect("ls large", ["f 16384 "])
    shell.expect("editor large", ["bytes=16384 lines=65"], marker=edit)
    output = shell.expect("p", ["1 | \n", "65 | "], marker=edit)
    assert "extra" not in output
    shell.expect("q", [])
    shell.expect("rm large", [])
    shell.expect('rm "my note"', [])
    shell.expect("rm empty", [])
    shell.expect("cd ..", [])
    shell.expect("rm nested", [])
    shell.expect("cd /", [])
    shell.expect("rm workspace", [])
    shell.expect("mkdir 'quoted directory'", [])
    shell.expect('cd "quoted directory"', [])
    shell.expect("pwd", ["\n/quoted directory\n"])
    shell.expect("cd", [])
    shell.expect('rm "quoted directory"', [])
    # Existing files may have an unterminated final line. Save edits in place,
    # so an already-open process descriptor still sees the same inode's data.
    parent = shell.pid("image /init")
    shell.expect(f"open {parent} raw create", ["opened fd=0"])
    shell.expect(f"write {parent} 0 tail", ["wrote bytes=4"])
    shell.expect(f"close {parent} 0", ["closed descriptor"])
    shell.expect(f"open {parent} raw r", ["opened fd=0"])
    shell.expect("editor raw", ["bytes=4 lines=1"], marker=edit)
    shell.expect("a", [], marker=text)
    shell.expect("next", [], marker=text)
    shell.expect(".", [], marker=edit)
    shell.expect("wq", ["saved bytes=10"])
    shell.expect(f"read {parent} 0 10", ["read bytes=10 data=tail\nnext\n"])
    shell.expect("editor raw", [], marker=edit)
    shell.expect("i 1", [], marker=text)
    shell.expect("head", [], marker=text)
    shell.expect(".", [], marker=edit)
    shell.expect("wq", ["saved bytes=15"])
    shell.expect("cat raw", ["head\ntail\nnext\n"])
    shell.expect(f"exit {parent}", [f"exited pid={parent}"])
    shell.expect(f"reap {parent}", ["reaped=1"])
    shell.expect("rm raw", [])
    shell.expect("ps", ["processes=0"])
    shell.expect("mem", [baseline])


def editor_exhaustion_checks(shell, baseline):
    edit, text = b"edit> ", b"text> "
    shell.expect("editor failure-note", [], marker=edit)
    shell.expect("a", [], marker=text)
    shell.expect("original", [], marker=text)
    shell.expect(".", [], marker=edit)
    shell.expect("wq", ["saved bytes=9"])
    before = shell.command("mem")
    blocks = [(int(order), int(count)) for order, count in
              re.findall(r"order=(\d+) blocks=(\d+)", before)]
    assert (3, 1) in blocks, "Need one free order-3 block for the editor buffer"
    reserved = 0
    for order, count in blocks:
        for _ in range(count):
            if order == 3:
                continue  # kalloc(16384) needs eight pages including its header.
            shell.expect(f"alloc {order}", ["block id="])
            reserved += 1
    shell.expect("mem", ["free=8"])
    for path in ("failure-note", "failure-new"):
        shell.expect(f"editor {path}", [], marker=edit)
        if path == "failure-note":
            shell.expect("r 1", [], marker=text)
            shell.expect("edited", [], marker=edit)
        shell.expect("a", [], marker=text)
        for _ in range(4):
            shell.expect("x" * 200, [], marker=text)
        shell.expect(".", [], marker=edit)
        shell.expect("w", ["save failed: error -2", "changes kept in editor"], marker=edit)
        shell.expect("q", ["unsaved changes"], marker=edit)
        shell.expect("p", [" | " + "x" * 200], marker=edit)
        shell.expect("q!", [])
    shell.expect("cat failure-note", ["\noriginal\n"])
    shell.expect("ls failure-note", ["f 9 "])
    shell.expect("ls failure-new", ["error -11"])
    shell.expect("editor failure-note", [], marker=edit)
    shell.expect("r 1", [], marker=text)
    shell.expect("recovered", [], marker=edit)
    shell.expect("wq", ["saved bytes=10"])
    shell.expect("cat failure-note", ["\nrecovered\n"])
    shell.expect("free all", [f"freed blocks={reserved}"])
    shell.expect("rm failure-note", [])
    shell.expect("mem", [baseline])


def test(qemu, kernel):
    shell = StageShell(qemu, kernel)
    try:
        assert "os_learn stage shell" in shell.prompt()
        shell.expect("help", ["spawn cpu|yield|sleep", "fork PID", "Ctrl-U"])
        shell.expect("ps", ["processes=0"])
        initial_memory = shell.command("mem")
        memory = re.search(r"total=(\d+) free=(\d+) used=(\d+)", initial_memory)
        assert memory
        total, free, used = map(int, memory.groups())
        assert total == 2048 and 0 < used < 16 and free + used == total
        baseline = f"free={free} used={used}"
        initial_blocks = [(int(order), int(count)) for order, count in
                          re.findall(r"order=(\d+) blocks=(\d+)", initial_memory)]
        shell.expect("unknown", ["unknown command", "error -1"])
        shell.expect("spawn", ["wrong argument count"])
        shell.expect("spawn cpu 0", ["error -1"])
        shell.expect("spawn cpu -1", ["error -1"])
        shell.expect("spawn cpu 18446744073709551616", ["error -1"])
        shell.expect("alloc 12", ["error -1"])
        allocated_blocks = 0
        for order, blocks in initial_blocks:
            for _ in range(blocks):
                shell.expect(f"alloc {order}", ["block id=", f"bytes={4096 << order}"])
                allocated_blocks += 1
        shell.expect("mem", ["free=0 used=2048"])
        shell.expect("spawn cpu 1", ["error -2"])
        shell.expect("free all", [f"freed blocks={allocated_blocks}"])
        shell.expect("mem", [baseline])

        filesystem_checks(shell, baseline)
        editor_exhaustion_checks(shell, baseline)

        cpu = shell.pid("spawn cpu 6")
        yielding = shell.pid("spawn yield 3")
        sleeper = shell.pid("spawn sleep 2")
        shell.expect("reap all", ["reaped=0"])
        shell.expect(f"reap {cpu}", ["error -4"])
        shell.expect("ps", ["processes=3", "RUNNABLE"])
        shell.expect("queues", ["level=0 quantum=1 allotment=2 ready=3"])
        shell.expect("run", [f"job {cpu} done", f"job {yielding} done",
                             f"job {sleeper} sleeping", "level=1", "scheduler idle"])
        shell.expect(f"show {cpu}", ["state=ZOMBIE exit=0", "started=1 active=0", "result=0"])
        shell.expect(f"show {sleeper}", ["state=SLEEPING", "started=1 active=1"])
        shell.expect(f"wake {sleeper}", ["woken=1"])
        shell.expect("run", [f"job {sleeper} woke", f"job {sleeper} done", "scheduler idle"])
        shell.expect("reap all", ["reaped=3"])
        shell.expect("mem", [baseline])

        sleeper = shell.pid("spawn sleep 2")
        shell.expect("run", [f"job {sleeper} sleeping", "scheduler idle"])
        shell.expect(f"kill {sleeper}", ["kill requested=1"])
        output = shell.expect("run", ["scheduler idle"])
        assert f"job {sleeper} woke" not in output
        shell.expect(f"show {sleeper}", ["state=ZOMBIE exit=-1", "active=0"])
        shell.expect(f"reap {sleeper}", ["reaped=1"])
        shell.expect(f"kill {sleeper}", ["error -3"])

        parent = shell.pid("image init")
        child = shell.pid(f"fork {parent}", label="child")
        parent_map = shell.command(f"show {parent}")
        child_map = shell.command(f"show {child}")
        parent_code = re.search(r"va=0x1000 physical=(0x[0-9a-f]+)", parent_map)
        child_code = re.search(r"va=0x1000 physical=(0x[0-9a-f]+)", child_map)
        assert parent_code and child_code and parent_code.group(1) != child_code.group(1)
        assert f"a0={child}" in parent_map and "a0=0" in child_map
        shell.expect(f"exec {child} alt hello world", ["name=alt argc=2 entry=0x1002"])
        shell.expect(f"show {child}", ["pc=0x1002", "a0=2", "mapped_pages=3"])
        shell.expect("run", ["error -8", "user-image execution"])
        shell.expect("kill all", ["kill requested=2"])
        shell.expect("run", ["scheduler idle"])
        shell.expect("reap all", ["reaped=2"])
        shell.expect("mem", [baseline])

        parent = shell.pid("image /init")
        shell.expect(f"mkdir {parent} /work", ["filesystem operation OK"])
        shell.expect(f"chdir {parent} /work", ["filesystem operation OK"])
        shell.expect(f"open {parent} note create", ["opened fd=0"])
        shell.expect(f"write {parent} 0 abcdef", ["wrote bytes=6"])
        shell.expect(f"open {parent} note r", ["opened fd=1"])
        child = shell.pid(f"fork {parent}", label="child")
        shell.expect(f"read {parent} 1 2", ["read bytes=2 data=ab"])
        shell.expect(f"read {child} 1 2", ["read bytes=2 data=cd"])
        shell.expect(f"dup {child} 1", ["duplicated fd=2"])
        shell.expect(f"read {child} 2 2", ["read bytes=2 data=ef"])
        shell.expect(f"exec {child} /alt", ["name=alt argc=0"])
        shell.expect(f"show {child}", ["fds: 0 1 2"])
        shell.expect(f"open {child} note r", ["opened fd=3"])
        shell.expect(f"unlink {parent} note", ["filesystem operation OK"])
        shell.expect(f"read {child} 3 6", ["read bytes=6 data=abcdef"])
        shell.expect(f"close {child} 3", ["closed descriptor"])
        shell.expect(f"chdir {parent} /", ["filesystem operation OK"])
        shell.expect(f"unlink {parent} /work", ["filesystem operation OK"])
        shell.expect("kill all", ["kill requested=2"])
        shell.expect("run", ["scheduler idle"])
        shell.expect("reap all", ["reaped=2"])
        shell.expect("mem", [baseline])

        sleeper = shell.pid("spawn sleep 1")
        shell.expect("run", ["sleeping", "scheduler idle"])
        shell.expect(f"exit {sleeper} -2147483648", ["code=-2147483648"])
        shell.expect(f"show {sleeper}", ["state=ZOMBIE exit=-2147483648", "active=0"])
        shell.expect("reap all", ["reaped=1"])
        shell.expect("boost", ["priorities boosted"])
        shell.expect("tick 64", ["ticks="])
        shell.expect("memx\b", [baseline])
        shell.expect("garbage\x15mem", [baseline])
        shell.expect("ignored\x03", ["^C"], end="")
        shell.expect("z" * 300, ["line too long"])
        shell.expect("echo one two three", ["one two three"])
        shell.expect("echo " + " ".join(["x"] * 12), ["error -7"])
        shell.expect("mem", [baseline])

        shell.expect("demo", ["sleeping", "Demo: ps; wake all; run; reap all; mem"])
        shell.expect("ps", ["processes=4", "SLEEPING"])
        shell.expect("wake all", ["woken=1"])
        shell.expect("run", ["woke", "done", "scheduler idle"])
        shell.expect("reap all", ["reaped=4"])
        shell.expect("mem", [baseline])

        for _ in range(32):
            shell.pid("spawn cpu 1")
        shell.expect("spawn cpu 1", ["error -2"])
        shell.expect("ps", ["processes=32"])
        shell.expect("kill all", ["kill requested=32"])
        shell.expect("run", ["scheduler idle"])
        shell.expect("reap all", ["reaped=32"])
        shell.expect("mem", [baseline])
        shell.quit()
        print(f"SHELL OK: {shell.checks} interactive checks; log: build/shell_test.log")
    finally:
        shell.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    parser.add_argument("--kernel", default="build/shell.elf")
    args = parser.parse_args()
    test(args.qemu, args.kernel)
