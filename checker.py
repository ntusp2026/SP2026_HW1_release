#!/usr/bin/env python3
"""Public testcase judge for SP2026 HW1: csieLedger."""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable


ROOT = Path(__file__).resolve().parent
SERVER_PATH = ROOT / "server"
RECORD_PATH = ROOT / "accountRecord"

ACCOUNT_ID_START = 902001
ACCOUNT_NUM = 20

WELCOME = (
    b"================================\n"
    b" Welcome to CSIE Ledger System \n"
    b"================================\n"
    b"Please enter your command: "
)
READY_PROMPT = b"Please enter your command: "
OP_PROMPT = b"Please enter an operation: "

DEFAULT_BALANCES = [
    500,
    1200,
    0,
    999999,
    350,
    42,
    7600,
    18,
    910,
    1000000,
    240,
    87,
    650,
    3000,
    1,
    777,
    25000,
    64,
    880,
    150,
]


class JudgeError(RuntimeError):
    pass


def write_records(balances: list[int]) -> None:
    data = b"".join(
        struct.pack("=ii", ACCOUNT_ID_START + index, balance)
        for index, balance in enumerate(balances)
    )
    RECORD_PATH.write_bytes(data)


def read_balance(account_id: int) -> int:
    offset = (account_id - ACCOUNT_ID_START) * struct.calcsize("=ii")
    data = RECORD_PATH.read_bytes()[offset : offset + struct.calcsize("=ii")]
    stored_id, balance = struct.unpack("=ii", data)
    if stored_id != account_id:
        raise JudgeError(f"record {account_id} has an unexpected id: {stored_id}")
    return balance


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class ServerProcess:
    def __init__(self) -> None:
        self.port = free_port()
        self.proc: subprocess.Popen[bytes] | None = None

    def __enter__(self) -> "ServerProcess":
        if not SERVER_PATH.is_file():
            raise JudgeError("./server does not exist. Run `make` first.")

        self.proc = subprocess.Popen(
            [str(SERVER_PATH), str(self.port)],
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise JudgeError("server exited before accepting connections")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return self
            except OSError:
                time.sleep(0.03)

        raise JudgeError("server did not start within 2 seconds")

    def __exit__(self, *_: object) -> None:
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=1.0)


class Client:
    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
        self.sock.settimeout(1.0)
        self.buffer = b""
        self.expect(WELCOME)

    def close(self) -> None:
        self.sock.close()

    def expect(self, expected: bytes) -> None:
        while len(self.buffer) < len(expected):
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout as exc:
                raise JudgeError(f"timeout; expected {expected!r}, received {self.buffer!r}") from exc
            if not chunk:
                raise JudgeError(f"connection closed; expected {expected!r}, received {self.buffer!r}")
            self.buffer += chunk

        actual = self.buffer[: len(expected)]
        self.buffer = self.buffer[len(expected) :]
        if actual != expected:
            raise JudgeError(f"output mismatch\nexpected: {expected!r}\nactual:   {actual!r}")

    def command(self, command: str, expected: bytes) -> None:
        self.sock.sendall(command.encode() + b"\n")
        self.expect(expected)

    def command_and_expect_close(self, command: str, expected: bytes) -> None:
        self.sock.sendall(command.encode() + b"\n")
        self.expect(expected)
        if self.buffer:
            raise JudgeError(f"unexpected output before close: {self.buffer!r}")
        try:
            extra = self.sock.recv(1)
        except socket.timeout as exc:
            raise JudgeError("server did not close the connection") from exc
        if extra:
            raise JudgeError(f"unexpected output before close: {extra!r}")


def task_1_1() -> None:
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as server:
        client = Client(server.port)
        try:
            client.command(
                "read 902001",
                b">>> Account 902001 balance: 500\n" + READY_PROMPT,
            )
            client.command_and_expect_close("exit", b">>> Client exit.\n")
        finally:
            client.close()


def task_1_2() -> None:
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as server:
        client = Client(server.port)
        try:
            client.command(
                "begin 902001",
                b">>> Transaction started on account 902001.\n"
                b">>> Current balance: 500\n"
                + OP_PROMPT,
            )
            client.command("add 100", b">>> Pending balance: 600\n" + OP_PROMPT)
            client.command("add -30", b">>> Pending balance: 570\n" + OP_PROMPT)
            client.command(
                "commit",
                b">>> Transaction committed.\n"
                b">>> Account 902001 balance: 570\n"
                + READY_PROMPT,
            )
            client.command_and_expect_close("exit", b">>> Client exit.\n")
        finally:
            client.close()

    if read_balance(902001) != 570:
        raise JudgeError("commit did not update accountRecord")


def task_1_3() -> None:
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as server:
        client = Client(server.port)
        try:
            client.command(
                "begin 902001",
                b">>> Transaction started on account 902001.\n"
                b">>> Current balance: 500\n"
                + OP_PROMPT,
            )
            client.command(
                "add -501",
                b">>> [Error] Balance out of range.\n" + OP_PROMPT,
            )
            client.command(
                "add 1000000",
                b">>> [Error] Balance out of range.\n" + OP_PROMPT,
            )
            client.command("abort", b">>> Transaction aborted.\n" + READY_PROMPT)
            client.command(
                "read 902001",
                b">>> Account 902001 balance: 500\n" + READY_PROMPT,
            )
            client.command_and_expect_close("exit", b">>> Client exit.\n")
        finally:
            client.close()

    if read_balance(902001) != 500:
        raise JudgeError("abort or rejected add modified accountRecord")


def task_1_4() -> None:
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as server:
        client = Client(server.port)
        try:
            client.command_and_expect_close(
                "read 902000", b">>> [Error] Invalid command.\n"
            )
        finally:
            client.close()


def task_2() -> None:
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as server:
        owner = Client(server.port)
        other = Client(server.port)
        try:
            owner.command(
                "begin 902001",
                b">>> Transaction started on account 902001.\n"
                b">>> Current balance: 500\n"
                + OP_PROMPT,
            )
            other.command("read 902001", b">>> Locked.\n" + READY_PROMPT)
            other.command(
                "read 902002",
                b">>> Account 902002 balance: 1200\n" + READY_PROMPT,
            )
            owner.command("abort", b">>> Transaction aborted.\n" + READY_PROMPT)
            other.command(
                "read 902001",
                b">>> Account 902001 balance: 500\n" + READY_PROMPT,
            )
            owner.command_and_expect_close("exit", b">>> Client exit.\n")
            other.command_and_expect_close("exit", b">>> Client exit.\n")
        finally:
            owner.close()
            other.close()


TASKS: dict[str, Callable[[], None]] = {
    "1-1": task_1_1,
    "1-2": task_1_2,
    "1-3": task_1_3,
    "1-4": task_1_4,
    "2": task_2,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-t",
        "--task",
        nargs="+",
        choices=TASKS,
        default=list(TASKS),
        help="public tasks to run",
    )
    args = parser.parse_args()

    if not RECORD_PATH.is_file():
        print("[ERROR] accountRecord is missing", file=sys.stderr)
        return 1

    original_record = RECORD_PATH.read_bytes()
    failed = 0

    try:
        for task_name in args.task:
            try:
                TASKS[task_name]()
            except (JudgeError, OSError, struct.error) as exc:
                failed += 1
                print(f"[FAIL] Task {task_name}: {exc}")
            else:
                print(f"[PASS] Task {task_name}")
    finally:
        RECORD_PATH.write_bytes(original_record)

    print(f"\nPassed {len(args.task) - failed}/{len(args.task)} public tasks.")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
