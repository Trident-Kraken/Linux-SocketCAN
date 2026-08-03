"""Tester-side ISO-TP and UDS client helpers."""

import can


class UdsClient:
    """Minimal single-frame UDS client for UDS integration tests."""

    def __init__(self, interface, request_id=0x7E0, response_id=0x7E8):
        self._request_id = request_id
        self._bus = can.Bus(
            interface="socketcan",
            channel=interface,
            can_filters=[
                {
                    "can_id": response_id,
                    "can_mask": 0x7FF,
                    "extended": False,
                }
            ],
        )

    def close(self):
        self._bus.shutdown()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def send_raw_frame(self, data):
        self._bus.send(
            can.Message(
                arbitration_id=self._request_id,
                data=data,
                is_extended_id=False,
            )
        )

    def receive(self, timeout=0.5):
        response = self._bus.recv(timeout)
        if response is None:
            return None

        data = bytes(response.data)
        if len(data) != 8 or data[0] >> 4 != 0:
            raise ValueError("ECU response is not a classic CAN ISO-TP SF")

        payload_len = data[0] & 0x0F
        if not 1 <= payload_len <= 7:
            raise ValueError("ECU response has an invalid SF length")
        if any(byte != 0xAA for byte in data[1 + payload_len :]):
            raise ValueError("ECU response has invalid SF padding")
        return data[1 : 1 + payload_len]

    def request(self, payload, timeout=0.5):
        if not 1 <= len(payload) <= 7:
            raise ValueError("single-frame payload length must be 1..7 bytes")

        frame_data = bytes([len(payload)]) + payload
        frame_data += bytes([0xAA]) * (8 - len(frame_data))
        self.send_raw_frame(frame_data)
        response = self.receive(timeout)
        if response is None:
            raise TimeoutError("timed out waiting for ECU response")
        return response

    @staticmethod
    def compute_key(seed):
        """Reference implementation of the demo ECU's 32-bit key formula."""
        key = (seed ^ 0xA5A5A5A5) & 0xFFFFFFFF
        key = ((key << 3) | (key >> 29)) & 0xFFFFFFFF
        return (key + 0x3C6EF35F) & 0xFFFFFFFF

    def request_seed(self, timeout=0.5):
        response = self.request(bytes.fromhex("27 01"), timeout)
        if len(response) != 6 or response[:2] != bytes.fromhex("67 01"):
            raise AssertionError(f"unexpected requestSeed response: {response!r}")
        return int.from_bytes(response[2:], byteorder="big")

    def send_key(self, key, timeout=0.5):
        payload = bytes.fromhex("27 02") + key.to_bytes(4, byteorder="big")
        return self.request(payload, timeout)
