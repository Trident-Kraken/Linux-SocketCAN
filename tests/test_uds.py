"""UDS service integration tests."""

from uds_client import UdsClient


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
