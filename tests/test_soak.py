"""Long-running sequential UDS regression coverage."""

import re
import time
from pathlib import Path

from uds_client import UdsClient


SOAK_REPETITIONS = 100
SOAK_REQUESTS_PER_REPETITION = 10
RSS_LIMIT_KIB = 8 * 1024
RSS_GROWTH_LIMIT_KIB = 1024


def read_process_metrics(pid):
    status = Path(f"/proc/{pid}/status").read_text()
    rss_match = re.search(r"^VmRSS:\s+(\d+) kB$", status, re.MULTILINE)
    stat_fields = Path(f"/proc/{pid}/stat").read_text().split()

    assert rss_match is not None
    return int(rss_match.group(1)), int(stat_fields[13]) + int(stat_fields[14])


def process_uses_address_sanitizer(pid):
    return "libasan" in Path(f"/proc/{pid}/maps").read_text()


def test_thousand_sequential_mixed_uds_requests_remain_stable(
    ecu_process, vcan_interface
):
    baseline_rss_kib, baseline_cpu_ticks = read_process_metrics(ecu_process.pid)
    completed_requests = 0
    started_at = time.monotonic()

    with UdsClient(vcan_interface) as client:
        for repetition in range(SOAK_REPETITIONS):
            sample_period = (100 + repetition).to_bytes(2, byteorder="big")

            assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
                "50 03 00 32 01 F4"
            )
            seed = client.request_seed()
            assert client.send_key(UdsClient.compute_key(seed)) == bytes.fromhex(
                "67 02"
            )
            assert client.request(bytes.fromhex("2E 02 00") + sample_period) == (
                bytes.fromhex("6E 02 00")
            )
            assert client.request(bytes.fromhex("22 02 00")) == (
                bytes.fromhex("62 02 00") + sample_period
            )
            assert client.request(bytes.fromhex("19 02 02")) == bytes.fromhex(
                "59 02 09"
            )
            assert client.request(bytes.fromhex("3E 00")) == bytes.fromhex("7E 00")
            assert client.request(bytes.fromhex("22 12 34")) == bytes.fromhex(
                "7F 22 31"
            )
            assert client.request(bytes.fromhex("10 02")) == bytes.fromhex(
                "7F 10 12"
            )
            assert client.request(bytes.fromhex("99")) == bytes.fromhex("7F 99 11")
            completed_requests += SOAK_REQUESTS_PER_REPETITION

    elapsed_seconds = time.monotonic() - started_at
    final_rss_kib, final_cpu_ticks = read_process_metrics(ecu_process.pid)
    uses_address_sanitizer = process_uses_address_sanitizer(ecu_process.pid)

    assert completed_requests == 1000
    assert ecu_process.poll() is None
    if not uses_address_sanitizer:
        assert final_rss_kib < RSS_LIMIT_KIB
        assert final_rss_kib <= baseline_rss_kib + RSS_GROWTH_LIMIT_KIB
    assert final_cpu_ticks >= baseline_cpu_ticks
    print(
        "soak requests=1000 "
        f"elapsed_s={elapsed_seconds:.3f} "
        f"rss_kib={baseline_rss_kib}->{final_rss_kib} "
        f"cpu_ticks={baseline_cpu_ticks}->{final_cpu_ticks} "
        f"asan={uses_address_sanitizer}"
    )
