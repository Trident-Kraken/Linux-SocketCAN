"""Shared pytest fixtures for the vUDS tester."""

from pathlib import Path
import os
import socket
import subprocess

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def pytest_addoption(parser):
    group = parser.getgroup("vuds")
    group.addoption(
        "--ecu-binary",
        default=str(PROJECT_ROOT / "build" / "vuds_ecu"),
        help="path to the vuds_ecu executable",
    )
    group.addoption(
        "--vcan-interface",
        default=os.environ.get("VCAN_INTERFACE", "vcan0"),
        help="SocketCAN interface used by integration tests",
    )
    group.addoption(
        "--isotp-harness",
        default=str(PROJECT_ROOT / "build" / "isotp_rx_harness"),
        help="path to the ISO-TP API harness",
    )


@pytest.fixture(scope="session")
def project_root():
    return PROJECT_ROOT


@pytest.fixture(scope="session")
def ecu_binary(pytestconfig):
    binary = Path(pytestconfig.getoption("--ecu-binary")).resolve()
    if not binary.is_file():
        pytest.skip(f"ECU binary not found: {binary}")
    return binary


@pytest.fixture(scope="session")
def isotp_harness(pytestconfig):
    harness = Path(pytestconfig.getoption("--isotp-harness")).resolve()
    if not harness.is_file():
        pytest.skip(f"ISO-TP harness not found: {harness}")
    return harness


@pytest.fixture(scope="session")
def vcan_interface(pytestconfig):
    interface = pytestconfig.getoption("--vcan-interface")
    try:
        socket.if_nametoindex(interface)
    except OSError:
        pytest.skip(
            f"{interface} is unavailable; run scripts/setup_vcan.sh "
            "or set --vcan-interface"
        )
    return interface


@pytest.fixture
def ecu_process(ecu_binary, vcan_interface):
    process = subprocess.Popen(
        [str(ecu_binary), "-i", vcan_interface],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        try:
            process.wait(timeout=0.2)
        except subprocess.TimeoutExpired:
            pass
        else:
            _, stderr = process.communicate()
            pytest.fail(
                f"ECU exited during startup with {process.returncode}: "
                f"{stderr.strip()}"
            )
        yield process
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        process.communicate()
