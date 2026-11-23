"""Fault-injection and lifecycle regression tests for the virtual ECU."""

import re
import shutil
import subprocess
import time

import can
import isotp
import pytest

from uds_client import UdsClient


def start_isotp_stack(interface):
    bus = can.Bus(interface="socketcan", channel=interface)
    stack = isotp.CanStack(
        bus,
        address=isotp.Address(
            isotp.AddressingMode.Normal_11bits,
            txid=0x7E0,
            rxid=0x7E8,
        ),
        params={
            "tx_data_length": 8,
            "tx_data_min_length": 8,
            "tx_padding": 0xAA,
            "max_frame_size": 4095,
            "blocking_send": False,
        },
    )
    stack.start()
    return bus, stack


def test_malformed_frames_do_not_prevent_following_valid_request(
    ecu_process, vcan_interface
):
    bus = can.Bus(interface="socketcan", channel=vcan_interface)
    malformed_frames = (
        can.Message(arbitration_id=0x7E0, data=b"", is_extended_id=False),
        can.Message(
            arbitration_id=0x7E0,
            data=bytes.fromhex("10 07 AA AA AA AA AA AA"),
            is_extended_id=False,
        ),
        can.Message(
            arbitration_id=0x7E0,
            data=bytes.fromhex("02 10 03 00 AA AA AA AA"),
            is_extended_id=False,
        ),
        can.Message(
            arbitration_id=0x7E0,
            is_extended_id=False,
            is_remote_frame=True,
        ),
        can.Message(
            arbitration_id=0x123457E0,
            data=bytes.fromhex("02 10 03 AA AA AA AA AA"),
            is_extended_id=True,
        ),
        can.Message(
            arbitration_id=0x7E0,
            data=bytes.fromhex("24 01 02 03 04 05 06 07"),
            is_extended_id=False,
        ),
        can.Message(
            arbitration_id=0x123,
            data=bytes.fromhex("02 10 03 AA AA AA AA AA"),
            is_extended_id=False,
        ),
    )

    try:
        for frame in malformed_frames:
            bus.send(frame)
    finally:
        bus.shutdown()

    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("10 03"))

    assert response == bytes.fromhex("50 03 00 32 01 F4")
    assert ecu_process.poll() is None


def test_oversize_did_write_is_rejected_without_changing_vin(
    ecu_process, vcan_interface
):
    bus, stack = start_isotp_stack(vcan_interface)
    original_vin = b"LVUDS2026DEMO0001"
    try:
        stack.send(bytes.fromhex("10 03"))
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        stack.send(bytes.fromhex("27 01"))
        seed_response = stack.recv(block=True, timeout=1.0)
        assert seed_response is not None
        seed = int.from_bytes(seed_response[2:], byteorder="big")
        stack.send(
            bytes.fromhex("27 02")
            + UdsClient.compute_key(seed).to_bytes(4, byteorder="big")
        )
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex("67 02")

        stack.send(bytes.fromhex("2E F1 90") + b"X" * 18)
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex("7F 2E 13")
        stack.send(bytes.fromhex("22 F1 90"))
        assert stack.recv(block=True, timeout=1.0) == (
            bytes.fromhex("62 F1 90") + original_vin
        )
    finally:
        stack.stop()
        bus.shutdown()


def test_shutdown_statistics_include_processed_requests(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        assert client.request(bytes.fromhex("99")) == bytes.fromhex("7F 99 11")

    ecu_process.terminate()
    _, stderr = ecu_process.communicate(timeout=2)
    match = re.search(
        r"can_frames_rx=(\d+) can_frames_tx=(\d+) "
        r"uds_requests=(\d+) uds_positive=(\d+) uds_negative=(\d+)",
        stderr,
    )

    assert ecu_process.returncode == 0
    assert match is not None
    can_frames_rx, can_frames_tx, requests, positive, negative = (
        int(value) for value in match.groups()
    )
    assert can_frames_rx >= 2
    assert can_frames_tx >= 2
    assert requests >= 2
    assert positive >= 1
    assert negative >= 1


@pytest.mark.integration
def test_interface_down_exits_cleanly_and_allows_restart(
    ecu_process, ecu_binary, vcan_interface
):
    if shutil.which("ip") is None:
        pytest.skip("iproute2 is required for interface fault injection")

    down = subprocess.run(
        ["ip", "link", "set", "dev", vcan_interface, "down"],
        capture_output=True,
        text=True,
        check=False,
    )
    if down.returncode != 0:
        pytest.skip("interface fault injection requires CAP_NET_ADMIN")

    try:
        ecu_process.wait(timeout=2)
        _, stderr = ecu_process.communicate()
        assert ecu_process.returncode == 1
        assert "event handling failed" in stderr
    finally:
        up = subprocess.run(
            ["ip", "link", "set", "dev", vcan_interface, "up"],
            capture_output=True,
            text=True,
            check=False,
        )
        assert up.returncode == 0, up.stderr

    restarted = subprocess.Popen(
        [str(ecu_binary), "-i", vcan_interface],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        assert restarted.poll() is None
        with UdsClient(vcan_interface) as client:
            assert client.request(bytes.fromhex("10 01")) == bytes.fromhex(
                "50 01 00 32 01 F4"
            )
    finally:
        if restarted.poll() is None:
            restarted.terminate()
            restarted.wait(timeout=2)
        restarted.communicate()
