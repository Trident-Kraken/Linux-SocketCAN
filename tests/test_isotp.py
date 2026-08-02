"""ISO-TP integration tests."""

import subprocess

import can
import isotp

from uds_client import UdsClient


def run_harness(harness, scenario):
    result = subprocess.run(
        [str(harness), scenario],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr or result.stdout


def test_receive_20_byte_payload_via_reference_isotp(
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
            "blocking_send": True,
        },
    )
    payload = bytes([0x10, 0x03]) + bytes(range(18))

    stack.start()
    try:
        stack.send(payload, send_timeout=2.0)
        response = stack.recv(block=True, timeout=1.0)
    finally:
        stack.stop()
        bus.shutdown()

    assert response == bytes.fromhex("7F 10 13")


def test_receive_20_byte_payload_with_public_api_harness(isotp_harness):
    run_harness(isotp_harness, "basic")


def test_receive_4095_byte_payload_and_wrap_sequence_number(isotp_harness):
    run_harness(isotp_harness, "boundary")


def test_sequence_error_discards_message_and_recovers(isotp_harness):
    run_harness(isotp_harness, "sn-error")


def test_n_cr_timeout_discards_message_and_recovers(isotp_harness):
    run_harness(isotp_harness, "timeout")


def test_new_valid_transfer_preempts_waiting_transfer(isotp_harness):
    run_harness(isotp_harness, "preempt")


def test_receive_requires_full_classic_frame_and_padding(isotp_harness):
    run_harness(isotp_harness, "strict")


def test_transmit_single_and_multi_frame_boundaries(isotp_harness):
    run_harness(isotp_harness, "tx-basic")


def test_transmit_4095_byte_payload_and_wrap_sequence_number(isotp_harness):
    run_harness(isotp_harness, "tx-boundary")


def test_transmit_flow_control_wait_block_size_and_stmin(isotp_harness):
    run_harness(isotp_harness, "tx-flow")


def test_transmit_stmin_microsecond_and_reserved_values(isotp_harness):
    run_harness(isotp_harness, "tx-stmin")


def test_transmit_flow_control_overflow_and_n_bs_timeout(isotp_harness):
    run_harness(isotp_harness, "tx-abort")


def test_transmit_can_errors_complete_with_failure_status(isotp_harness):
    run_harness(isotp_harness, "tx-can-error")


def test_transmit_timer_cleanup_errors_complete_with_failure_status(
    isotp_harness,
):
    run_harness(isotp_harness, "tx-cleanup-error")


def test_flow_control_wait_restarts_n_bs_timer(isotp_harness):
    run_harness(isotp_harness, "tx-wait")


def test_uds_busy_slot_replies_after_tx_completion_or_timeout(isotp_harness):
    run_harness(isotp_harness, "uds-busy")


def test_seven_byte_single_frame_reaches_uds(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        response = client.request(bytes.fromhex("10 03 00 00 00 00 00"))

    assert response == bytes.fromhex("7F 10 13")


def test_zero_length_single_frame_is_ignored(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        client.send_raw_frame(bytes.fromhex("00 AA AA AA AA AA AA AA"))
        assert client.receive(timeout=0.1) is None

        response = client.request(bytes.fromhex("10 03"))

    assert response == bytes.fromhex("50 03 00 32 01 F4")


def test_single_frame_length_beyond_dlc_is_ignored(
    ecu_process, vcan_interface
):
    with UdsClient(vcan_interface) as client:
        client.send_raw_frame(bytes.fromhex("03 10 03"))
        assert client.receive(timeout=0.1) is None

        response = client.request(bytes.fromhex("10 01"))

    assert response == bytes.fromhex("50 01 00 32 01 F4")


def test_unknown_pci_type_is_ignored(ecu_process, vcan_interface):
    with UdsClient(vcan_interface) as client:
        client.send_raw_frame(bytes.fromhex("40 10 03 AA AA AA AA AA"))
        assert client.receive(timeout=0.1) is None

        response = client.request(bytes.fromhex("22 01 00"))

    assert response == bytes.fromhex("62 01 00 00 55")
