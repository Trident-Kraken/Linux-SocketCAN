"""Run mixed UDS traffic against a Valgrind-instrumented virtual ECU."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time

from uds_client import UdsClient


def run_request_cycle(client, repetition):
    sample_period = (100 + repetition % 100).to_bytes(2, byteorder="big")

    assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
        "50 03 00 32 01 F4"
    )
    seed = client.request_seed()
    assert client.send_key(UdsClient.compute_key(seed)) == bytes.fromhex("67 02")
    assert client.request(bytes.fromhex("2E 02 00") + sample_period) == (
        bytes.fromhex("6E 02 00")
    )
    assert client.request(bytes.fromhex("22 02 00")) == (
        bytes.fromhex("62 02 00") + sample_period
    )
    assert client.request(bytes.fromhex("19 02 02")) == bytes.fromhex("59 02 09")
    assert client.request(bytes.fromhex("3E 00")) == bytes.fromhex("7E 00")
    assert client.request(bytes.fromhex("22 12 34")) == bytes.fromhex("7F 22 31")
    assert client.request(bytes.fromhex("10 02")) == bytes.fromhex("7F 10 12")
    assert client.request(bytes.fromhex("99")) == bytes.fromhex("7F 99 11")


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ecu-binary", type=Path, default=Path("build/vuds_ecu"))
    parser.add_argument("--interface", default="vcan0")
    parser.add_argument("--duration", type=float, default=600.0)
    parser.add_argument("--log-file", type=Path)
    arguments = parser.parse_args()

    if arguments.duration <= 0:
        parser.error("--duration must be positive")
    arguments.ecu_binary = arguments.ecu_binary.resolve()
    if not arguments.ecu_binary.is_file():
        parser.error(f"ECU binary does not exist: {arguments.ecu_binary}")
    return arguments


def wait_for_ecu(process, interface):
    deadline = time.monotonic() + 5.0

    while time.monotonic() < deadline:
        if process.poll() is not None:
            _, stderr = process.communicate()
            raise RuntimeError(f"Valgrind ECU exited during startup: {stderr}")
        try:
            with UdsClient(interface) as client:
                if client.request(bytes.fromhex("10 01"), timeout=0.2) == (
                    bytes.fromhex("50 01 00 32 01 F4")
                ):
                    return
        except (OSError, TimeoutError):
            time.sleep(0.1)

    raise RuntimeError("Valgrind ECU did not respond to the startup request")


def main():
    arguments = parse_arguments()
    log_file = arguments.log_file or Path(tempfile.gettempdir()) / "vuds-valgrind.log"
    command = [
        "valgrind",
        "--leak-check=full",
        "--show-leak-kinds=definite",
        "--errors-for-leak-kinds=definite",
        "--error-exitcode=99",
        "--track-fds=yes",
        "--log-fd=2",
        str(arguments.ecu_binary),
        "-i",
        arguments.interface,
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    cycles = 0

    try:
        wait_for_ecu(process, arguments.interface)
        started_at = time.monotonic()
        deadline = started_at + arguments.duration
        next_progress_at = started_at + 10.0
        with UdsClient(arguments.interface) as client:
            while time.monotonic() < deadline:
                run_request_cycle(client, cycles)
                cycles += 1
                if time.monotonic() >= next_progress_at:
                    print(
                        f"valgrind progress cycles={cycles} "
                        f"elapsed_s={time.monotonic() - started_at:.1f}",
                        flush=True,
                    )
                    next_progress_at += 10.0
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        _, stderr = process.communicate()
        log_file.write_text(stderr)

    report = stderr
    if process.returncode != 0:
        raise RuntimeError(
            f"Valgrind ECU exited with {process.returncode}: {stderr}\n{report}"
        )
    if "ERROR SUMMARY: 0 errors" not in report:
        raise RuntimeError(f"Valgrind reported errors:\n{report}")
    if (
        "All heap blocks were freed -- no leaks are possible" not in report
        and "definitely lost: 0 bytes" not in report
    ):
        raise RuntimeError(f"Valgrind reported a definite leak:\n{report}")

    elapsed_seconds = time.monotonic() - started_at
    print(
        f"valgrind cycles={cycles} requests={cycles * 10} "
        f"elapsed_s={elapsed_seconds:.3f} log={log_file}"
    )


if __name__ == "__main__":
    main()
