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

    def prompt(self):
        deadline = time.monotonic() + 10
        while True:
            index = self.pending.find(b"os> ")
            if index >= 0:
                output, self.pending = self.pending[:index], self.pending[index + 4:]
                return output.decode("utf-8", errors="replace").replace("\r", "")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError("Timed out waiting for the shell prompt")
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

    def command(self, text, end="\n"):
        self.process.stdin.write((text + end).encode("ascii"))
        self.process.stdin.flush()
        return self.prompt()

    def expect(self, text, fragments, end="\n"):
        output = self.command(text, end)
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


def test(qemu, kernel):
    shell = StageShell(qemu, kernel)
    try:
        assert "os_learn stage shell" in shell.prompt()
        shell.expect("help", ["spawn cpu|yield|sleep", "fork PID", "Ctrl-U"])
        shell.expect("ps", ["processes=0"])
        shell.expect("mem", ["total=2048 free=2048 used=0", "order=11 blocks=1"])
        shell.expect("unknown", ["unknown command", "error -1"])
        shell.expect("spawn", ["wrong argument count"])
        shell.expect("spawn cpu 0", ["error -1"])
        shell.expect("spawn cpu -1", ["error -1"])
        shell.expect("spawn cpu 18446744073709551616", ["error -1"])
        shell.expect("alloc 12", ["error -1"])
        shell.expect("alloc 11", ["block id=1", "bytes=8388608"])
        shell.expect("mem", ["free=0 used=2048"])
        shell.expect("spawn cpu 1", ["error -2"])
        shell.expect("free all", ["freed blocks=1"])
        shell.expect("mem", ["free=2048 used=0"])

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
        shell.expect("mem", ["free=2048 used=0"])

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
        shell.expect("mem", ["free=2048 used=0"])

        sleeper = shell.pid("spawn sleep 1")
        shell.expect("run", ["sleeping", "scheduler idle"])
        shell.expect(f"exit {sleeper} -2147483648", ["code=-2147483648"])
        shell.expect(f"show {sleeper}", ["state=ZOMBIE exit=-2147483648", "active=0"])
        shell.expect("reap all", ["reaped=1"])
        shell.expect("boost", ["priorities boosted"])
        shell.expect("tick 64", ["ticks="])
        shell.expect("memx\b", ["free=2048 used=0"])
        shell.expect("garbage\x15mem", ["free=2048 used=0"])
        shell.expect("ignored\x03", ["^C"], end="")
        shell.expect("z" * 300, ["line too long"])
        shell.expect("echo one two three", ["one two three"])
        shell.expect("echo " + " ".join(["x"] * 12), ["error -7"])
        shell.expect("mem", ["free=2048 used=0"])

        shell.expect("demo", ["sleeping", "Demo: ps; wake all; run; reap all; mem"])
        shell.expect("ps", ["processes=4", "SLEEPING"])
        shell.expect("wake all", ["woken=1"])
        shell.expect("run", ["woke", "done", "scheduler idle"])
        shell.expect("reap all", ["reaped=4"])
        shell.expect("mem", ["free=2048 used=0"])

        for _ in range(32):
            shell.pid("spawn cpu 1")
        shell.expect("spawn cpu 1", ["error -2"])
        shell.expect("ps", ["processes=32"])
        shell.expect("kill all", ["kill requested=32"])
        shell.expect("run", ["scheduler idle"])
        shell.expect("reap all", ["reaped=32"])
        shell.expect("mem", ["free=2048 used=0", "order=11 blocks=1"])
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
