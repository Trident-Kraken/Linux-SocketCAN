"""ISO-TP integration tests."""

from uds_client import UdsClient


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
