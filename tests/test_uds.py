"""UDS service integration tests."""

import subprocess
import time

import can
import isotp

from uds_client import UdsClient


def run_harness(harness, scenario):
    subprocess.run([str(harness), scenario], check=True)


def start_isotp_stack(interface, blocksize=8, stmin=0):
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
            "blocksize": blocksize,
            "stmin": stmin,
            "blocking_send": False,
        },
    )
    stack.start()
    return bus, stack


def test_diagnostic_session_control_enters_extended_session(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("10 03"))
        duplicate = client.receive(timeout=0.1)

    assert response == bytes.fromhex("50 03 00 32 01 F4")
    assert duplicate is None


def test_diagnostic_session_control_enters_default_session(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("10 01"))

    assert response == bytes.fromhex("50 01 00 32 01 F4")


def test_diagnostic_session_control_rejects_unsupported_subfunction(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("10 02"))

    assert response == bytes.fromhex("7F 10 12")


def test_diagnostic_session_control_rejects_short_request(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("10"))

    assert response == bytes.fromhex("7F 10 13")


def test_tester_present_returns_positive_response(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("3E 00"))

    assert response == bytes.fromhex("7E 00")


def test_tester_present_rejects_unsupported_subfunction(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("3E 01"))

    assert response == bytes.fromhex("7F 3E 12")


def test_tester_present_rejects_short_request(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("3E"))

    assert response == bytes.fromhex("7F 3E 13")


def test_tester_present_suppress_bit_suppresses_positive_response(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        client.send_raw_frame(bytes.fromhex("02 3E 80 AA AA AA AA AA"))
        response = client.receive(timeout=0.1)

    assert response is None


def test_suppress_bit_does_not_suppress_negative_response(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("10 82"))

    assert response == bytes.fromhex("7F 10 12")


def test_session_and_s3_lifecycle_harness(isotp_harness):
    run_harness(isotp_harness, "uds-session-s3")


def test_did_read_write_harness(isotp_harness):
    run_harness(isotp_harness, "uds-did")


def test_dtc_harness(isotp_harness):
    run_harness(isotp_harness, "uds-dtc")


def test_read_dtc_reports_all_records(ecu_process, vcan_interface):
    bus, stack = start_isotp_stack(vcan_interface)
    try:
        stack.send(bytes.fromhex("19 02 FF"))
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex(
            "59 02 09 01 02 03 09 02 03 04 08 03 04 01 01"
        )
    finally:
        stack.stop()
        bus.shutdown()


def test_read_dtc_filters_confirmed_records(ecu_process, vcan_interface):
    bus, stack = start_isotp_stack(vcan_interface)
    try:
        stack.send(bytes.fromhex("19 02 08"))
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex(
            "59 02 09 01 02 03 09 02 03 04 08"
        )
    finally:
        stack.stop()
        bus.shutdown()


def test_read_dtc_returns_positive_empty_result_for_no_match(
    ecu_process, vcan_interface
):
    bus, stack = start_isotp_stack(vcan_interface)
    try:
        stack.send(bytes.fromhex("19 02 02"))
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex("59 02 09")
    finally:
        stack.stop()
        bus.shutdown()


def test_read_dtc_is_available_in_default_and_extended_sessions(
    ecu_process, vcan_interface
):
    bus, stack = start_isotp_stack(vcan_interface)
    expected = bytes.fromhex("59 02 09 01 02 03 09 02 03 04 08")
    try:
        stack.send(bytes.fromhex("19 02 08"))
        assert stack.recv(block=True, timeout=1.0) == expected
        stack.send(bytes.fromhex("10 03"))
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        stack.send(bytes.fromhex("19 02 08"))
        assert stack.recv(block=True, timeout=1.0) == expected
    finally:
        stack.stop()
        bus.shutdown()


def test_read_dtc_rejects_unsupported_subfunction_and_length(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        assert client.request(bytes.fromhex("19 05 00")) == bytes.fromhex(
            "7F 19 12"
        )
        assert client.request(bytes.fromhex("19 02")) == bytes.fromhex(
            "7F 19 13"
        )
        assert client.request(bytes.fromhex("19 02 00 00")) == bytes.fromhex(
            "7F 19 13"
        )


def test_unknown_service_returns_service_not_supported(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("99"))

    assert response == bytes.fromhex("7F 99 11")


def test_read_coolant_temperature_did(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("22 01 00"))

    assert response == bytes.fromhex("62 01 00 00 55")


def test_read_unknown_did_returns_request_out_of_range(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("22 12 34"))

    assert response == bytes.fromhex("7F 22 31")


def test_read_did_rejects_short_request(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("22 01"))

    assert response == bytes.fromhex("7F 22 13")


def test_read_all_static_dids(ecu_process, vcan_interface):
    expected = {
        "0100": bytes.fromhex("00 55"),
        "0101": bytes.fromhex("0F A0"),
        "0200": bytes.fromhex("00 64"),
    }

    with UdsClient(vcan_interface) as client:
        for did, value in expected.items():
            response = client.request(bytes.fromhex("22 " + did))
            assert response == bytes.fromhex("62 " + did) + value


def test_read_vin_uses_isotp_multiframe(ecu_process, vcan_interface):
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
        stack.send(bytes.fromhex("22 F1 89"))
        assert stack.recv(block=True, timeout=1.0) == (
            bytes.fromhex("62 F1 89") + b"vUDS-1.0.0"
        )
        stack.send(bytes.fromhex("22 F1 90"))
        assert stack.recv(block=True, timeout=1.0) == (
            bytes.fromhex("62 F1 90") + b"LVUDS2026DEMO0001"
        )
    finally:
        stack.stop()
        bus.shutdown()


def test_write_rejects_default_session_and_locked_extended_session(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        assert client.request(bytes.fromhex("2E 02 00 00 C8")) == bytes.fromhex(
            "7F 2E 7F"
        )
        assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        assert client.request(bytes.fromhex("2E 02 00 00 C8")) == bytes.fromhex(
            "7F 2E 33"
        )


def test_write_did_unlocks_updates_and_reads_back(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        seed = client.request_seed()
        assert client.send_key(UdsClient.compute_key(seed)) == bytes.fromhex(
            "67 02"
        )
        assert client.request(bytes.fromhex("2E 02 00 01 2C")) == bytes.fromhex(
            "6E 02 00"
        )
        assert client.request(bytes.fromhex("22 02 00")) == bytes.fromhex(
            "62 02 00 01 2C"
        )


def test_write_rejects_unknown_readonly_and_invalid_lengths(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        seed = client.request_seed()
        assert client.send_key(UdsClient.compute_key(seed)) == bytes.fromhex(
            "67 02"
        )
        assert client.request(bytes.fromhex("2E 12 34 00 01")) == bytes.fromhex(
            "7F 2E 31"
        )
        assert client.request(bytes.fromhex("2E F1 89 00 01")) == bytes.fromhex(
            "7F 2E 31"
        )
        assert client.request(bytes.fromhex("2E 02 00 01")) == bytes.fromhex(
            "7F 2E 13"
        )
        assert client.request(bytes.fromhex("2E 02 00 01 02 03")) == bytes.fromhex(
            "7F 2E 13"
        )


def test_session_switch_relocks_write_service(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        seed = client.request_seed()
        assert client.send_key(UdsClient.compute_key(seed)) == bytes.fromhex(
            "67 02"
        )
        assert client.request(bytes.fromhex("10 01")) == bytes.fromhex(
            "50 01 00 32 01 F4"
        )
        assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        assert client.request(bytes.fromhex("2E 02 00 00 C8")) == bytes.fromhex(
            "7F 2E 33"
        )


def test_long_write_uses_isotp_rx_and_updates_vin(ecu_process, vcan_interface):
    new_vin = b"LVUDS2026TEST0002"
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
        seed = int.from_bytes(seed_response[2:], byteorder="big")
        stack.send(
            bytes.fromhex("27 02")
            + UdsClient.compute_key(seed).to_bytes(4, byteorder="big")
        )
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex("67 02")
        stack.send(bytes.fromhex("2E F1 90") + new_vin)
        assert stack.recv(block=True, timeout=1.0) == bytes.fromhex("6E F1 90")
        stack.send(bytes.fromhex("22 F1 90"))
        assert stack.recv(block=True, timeout=1.0) == (
            bytes.fromhex("62 F1 90") + new_vin
        )
    finally:
        stack.stop()
        bus.shutdown()


def test_extended_session_suppress_positive_switches_session(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        client.send_raw_frame(bytes.fromhex("02 10 83 AA AA AA AA AA"))
        assert client.receive(timeout=0.1) is None
        assert client.request(bytes.fromhex("2E 02 00 00 C8")) == bytes.fromhex(
            "7F 2E 33"
        )


def test_s3_timeout_falls_back_to_default_and_tester_present_keeps_session_alive(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        assert client.request(bytes.fromhex("10 03")) == bytes.fromhex(
            "50 03 00 32 01 F4"
        )
        time.sleep(3.0)
        client.send_raw_frame(bytes.fromhex("02 3E 80 AA AA AA AA AA"))
        assert client.receive(timeout=0.1) is None
        time.sleep(3.0)
        assert client.request(bytes.fromhex("2E 02 00 00 C8")) == bytes.fromhex(
            "7F 2E 33"
        )
        time.sleep(5.5)
        assert client.request(bytes.fromhex("2E 02 00 00 C8")) == bytes.fromhex(
            "7F 2E 7F"
        )


def test_busy_request_during_multiframe_vin_tx_receives_nrc_21_without_side_effect(
    ecu_process, vcan_interface
):
    bus = can.Bus(
        interface="socketcan",
        channel=vcan_interface,
        can_filters=[
            {"can_id": 0x7E8, "can_mask": 0x7FF, "extended": False}
        ],
    )

    def send_7e0(hex_payload):
        bus.send(
            can.Message(
                arbitration_id=0x7E0,
                data=bytes.fromhex(hex_payload),
                is_extended_id=False,
            )
        )

    try:
        send_7e0("02 10 03 AA AA AA AA AA")
        msg = bus.recv(0.5)
        assert msg is not None
        assert bytes(msg.data) == bytes.fromhex("06 50 03 00 32 01 F4 AA")

        send_7e0("03 22 F1 90 AA AA AA AA")
        ff = bus.recv(0.5)
        assert ff is not None
        assert bytes(ff.data) == bytes.fromhex("10 14 62 F1 90 4C 56 55")

        send_7e0("02 10 01 AA AA AA AA AA")
        assert bus.recv(0.05) is None

        send_7e0("30 00 00 AA AA AA AA AA")
        cf1 = bus.recv(0.5)
        cf2 = bus.recv(0.5)
        busy_nrc = bus.recv(0.5)
        assert cf1 is not None
        assert cf2 is not None
        assert busy_nrc is not None
        assert bytes(cf1.data) == bytes.fromhex("21 44 53 32 30 32 36 44")
        assert bytes(cf2.data) == bytes.fromhex("22 45 4D 4F 30 30 30 31")
        assert bytes(busy_nrc.data) == bytes.fromhex("03 7F 10 21 AA AA AA AA")

        send_7e0("05 2E 02 00 00 C8 AA AA")
        verify = bus.recv(0.5)
        assert verify is not None
        assert bytes(verify.data) == bytes.fromhex("03 7F 2E 33 AA AA AA AA")
    finally:
        bus.shutdown()


def test_vin_multiframe_tx_respects_reference_isotp_blocksize_and_stmin(
    ecu_process, vcan_interface
):
    expected = bytes.fromhex("62 F1 90") + b"LVUDS2026DEMO0001"

    bus, stack = start_isotp_stack(vcan_interface, blocksize=1, stmin=0)
    try:
        stack.send(bytes.fromhex("22 F1 90"))
        assert stack.recv(block=True, timeout=1.0) == expected
    finally:
        stack.stop()
        bus.shutdown()

    bus, stack = start_isotp_stack(vcan_interface, blocksize=0, stmin=20)
    try:
        started = time.monotonic()
        stack.send(bytes.fromhex("22 F1 90"))
        assert stack.recv(block=True, timeout=1.0) == expected
        assert time.monotonic() - started >= 0.010
    finally:
        stack.stop()
        bus.shutdown()
