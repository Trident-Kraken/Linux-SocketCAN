"""UDS SecurityAccess integration tests."""

import subprocess
import time

import can
import isotp

from uds_client import UdsClient


def test_security_state_machine_harness(isotp_harness):
    subprocess.run([str(isotp_harness), "uds-security"], check=True)


def test_reference_key_algorithm_known_vector():
    assert UdsClient.compute_key(0x12345678) == 0xF8FE924C


def test_security_access_unlocks_via_reference_isotp(
    ecu_process, vcan_interface
):
    bus = can.Bus(interface="socketcan", channel=vcan_interface)
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
    try:
        stack.send(bytes.fromhex("10 03"))
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        stack.send(bytes.fromhex("27 01"))
        seed_response = stack.recv(block=True, timeout=1.0)
        assert seed_response is not None
        assert seed_response[:2] == bytes.fromhex("67 01")
        seed = int.from_bytes(seed_response[2:], byteorder="big")
        stack.send(
            bytes.fromhex("27 02")
            + UdsClient.compute_key(seed).to_bytes(4, byteorder="big"),
        )
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex("67 02")
    finally:
        stack.stop()
        bus.shutdown()


def test_request_seed_changes_challenge(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        enter_extended_session(client)
        seeds = [client.request_seed() for _ in range(4)]

    assert all(seed not in (0, 0xFFFFFFFF) for seed in seeds)
    assert len(set(seeds)) > 1


def enter_extended_session(client):
    assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
        "50 03 00 32 01 F4"
    )


def test_security_access_is_restricted_to_extended_session(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("27 01"))

    assert response == bytes.fromhex("7F 27 7F")


def test_security_access_rejects_invalid_length_and_subfunction(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        enter_extended_session(client)
        invalid_subfunction = client.request(bytes.fromhex("27 03"))
        invalid_length = client.request(bytes.fromhex("27"))

    assert invalid_subfunction == bytes.fromhex("7F 27 12")
    assert invalid_length == bytes.fromhex("7F 27 13")


def test_security_access_requires_seed_before_key(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        enter_extended_session(client)
        response = client.request(bytes.fromhex("27 02 00 00 00 00"))

    assert response == bytes.fromhex("7F 27 24")


def test_security_access_unlocks_with_reference_key(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        enter_extended_session(client)
        seed = client.request_seed()
        assert seed not in (0, 0xFFFFFFFF)
        replacement_seed = client.request_seed()
        assert replacement_seed not in (0, 0xFFFFFFFF)
        if replacement_seed == seed:
            replacement_seed = client.request_seed()
        if replacement_seed != seed:
            assert client.send_key(UdsClient.compute_key(seed)) == bytes.fromhex(
                "7F 27 35"
            )
        response = client.send_key(UdsClient.compute_key(replacement_seed))

    assert response == bytes.fromhex("67 02")


def test_security_access_lockout_and_lazy_recovery(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        enter_extended_session(client)
        seed = client.request_seed()
        wrong_key = UdsClient.compute_key(seed) ^ 0x01
        assert client.send_key(wrong_key) == bytes.fromhex("7F 27 35")
        assert client.send_key(wrong_key) == bytes.fromhex("7F 27 35")
        assert client.send_key(wrong_key) == bytes.fromhex("7F 27 36")
        assert client.request(bytes.fromhex("27 01")) == bytes.fromhex(
            "7F 27 37"
        )

        for _ in range(4):
            time.sleep(2.0)
            assert client.request(bytes.fromhex("3E 00")) == bytes.fromhex(
                "7E 00"
            )
        time.sleep(2.2)
        recovered_seed = client.request_seed()

    assert recovered_seed not in (0, 0xFFFFFFFF)


def test_request_seed_returns_zero_when_already_unlocked(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        enter_extended_session(client)
        seed = client.request_seed()
        assert client.send_key(UdsClient.compute_key(seed)) == bytes.fromhex(
            "67 02"
        )
        assert client.request(bytes.fromhex("27 01")) == bytes.fromhex(
            "67 01 00 00 00 00"
        )
        assert client.request(bytes.fromhex("2E 02 00 01 2C")) == bytes.fromhex(
            "6E 02 00"
        )
