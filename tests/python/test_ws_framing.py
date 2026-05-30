"""Tests for the hand-rolled WebSocket frame codec in ``dbus_client.py``.

The widget speaks raw WebSocket (RFC 6455) to the daemon without a library,
so the framing in ``_ws_send_frame`` / ``_ws_recv_frame`` is load-bearing:
a wrong mask or length encoding silently corrupts every command. These tests
exercise the codec end-to-end over a fake socket (no network):

* client frames are masked with the MASK bit set and the correct length
  encoding for each of the three RFC 6455 length buckets;
* a frame encoded by ``_ws_send_frame`` round-trips back through
  ``_ws_recv_frame`` to the original payload (masking applied then undone);
* the decoder reads server (unmasked) frames, the 126/127 length
  extensions, and control opcodes (ping/close);
* a short read / EOF raises ``WsError`` rather than hanging or returning
  a truncated payload.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_CODE = REPO_ROOT / "shared" / "chat-module" / "code"
sys.path.insert(0, str(SHARED_CODE))

import dbus_client  # type: ignore[import-not-found]  # noqa: E402


class FakeSocket:
    """Minimal stand-in for ``socket.socket`` for the framing helpers.

    Captures everything written via ``sendall`` and serves a preloaded byte
    buffer from ``recv`` (returning ``b""`` once drained, i.e. EOF).
    """

    def __init__(self, recv_bytes: bytes = b"") -> None:
        self.sent = bytearray()
        self._recv = bytearray(recv_bytes)

    def sendall(self, data: bytes) -> None:
        self.sent.extend(data)

    def recv(self, count: int) -> bytes:
        if not self._recv:
            return b""  # EOF
        chunk = bytes(self._recv[:count])
        del self._recv[:count]
        return chunk


def _unmask(mask_key: bytes, masked: bytes) -> bytes:
    return bytes(b ^ mask_key[i % 4] for i, b in enumerate(masked))


class SendFrameStructureTests(unittest.TestCase):
    def test_short_frame_header_and_mask_bit(self) -> None:
        sock = FakeSocket()
        dbus_client._ws_send_frame(sock, 0x1, b"hello")
        wire = bytes(sock.sent)
        # byte 0 == FIN (0x80) | opcode (0x1)
        self.assertEqual(wire[0], 0x81)
        # byte 1 high bit is the client MASK bit, low 7 bits = length
        self.assertTrue(wire[1] & 0x80, "client frames must set the MASK bit")
        self.assertEqual(wire[1] & 0x7F, 5)
        # 2 header bytes + 4 mask + 5 payload
        self.assertEqual(len(wire), 2 + 4 + 5)

    def test_masking_is_applied_and_reversible(self) -> None:
        payload = b"the quick brown fox"
        sock = FakeSocket()
        dbus_client._ws_send_frame(sock, 0x1, payload)
        wire = bytes(sock.sent)
        mask_key = wire[2:6]
        masked_payload = wire[6:]
        # The masked bytes are recoverable with the transmitted mask key.
        self.assertEqual(_unmask(mask_key, masked_payload), payload)

    def test_medium_length_uses_two_byte_extension(self) -> None:
        payload = b"x" * 200  # 126 <= len < 65536
        sock = FakeSocket()
        dbus_client._ws_send_frame(sock, 0x1, payload)
        wire = bytes(sock.sent)
        self.assertEqual(wire[1] & 0x7F, 126)
        self.assertEqual(int.from_bytes(wire[2:4], "big"), 200)

    def test_large_length_uses_eight_byte_extension(self) -> None:
        payload = b"y" * 65536  # >= 65536
        sock = FakeSocket()
        dbus_client._ws_send_frame(sock, 0x1, payload)
        wire = bytes(sock.sent)
        self.assertEqual(wire[1] & 0x7F, 127)
        self.assertEqual(int.from_bytes(wire[2:10], "big"), 65536)

    def test_send_text_uses_text_opcode_and_utf8(self) -> None:
        sock = FakeSocket()
        dbus_client._ws_send_text(sock, "café ☕")
        # Decode the frame we just produced back out.
        opcode, payload = dbus_client._ws_recv_frame(FakeSocket(bytes(sock.sent)))
        self.assertEqual(opcode, 0x1)
        self.assertEqual(payload.decode("utf-8"), "café ☕")


class RoundTripTests(unittest.TestCase):
    """A frame produced by the encoder must decode back to the original."""

    def _roundtrip(self, payload: bytes, opcode: int = 0x1) -> None:
        enc = FakeSocket()
        dbus_client._ws_send_frame(enc, opcode, payload)
        dec_opcode, dec_payload = dbus_client._ws_recv_frame(FakeSocket(bytes(enc.sent)))
        self.assertEqual(dec_opcode, opcode)
        self.assertEqual(dec_payload, payload)

    def test_empty_payload(self) -> None:
        self._roundtrip(b"")

    def test_small_payload(self) -> None:
        self._roundtrip(b"hello world")

    def test_boundary_125(self) -> None:
        # 125 is the largest 1-byte length.
        self._roundtrip(b"a" * 125)

    def test_boundary_126(self) -> None:
        # 126 is the smallest 2-byte-extension length.
        self._roundtrip(b"b" * 126)

    def test_boundary_65535(self) -> None:
        # 65535 is the largest 2-byte-extension length.
        self._roundtrip(b"c" * 65535)

    def test_boundary_65536(self) -> None:
        # 65536 is the smallest 8-byte-extension length.
        self._roundtrip(b"d" * 65536)

    def test_binary_payload_with_high_bytes(self) -> None:
        self._roundtrip(bytes(range(256)) * 4)


class RecvFrameTests(unittest.TestCase):
    """Decoding of inbound (server) frames, which are unmasked."""

    @staticmethod
    def _server_frame(opcode: int, payload: bytes) -> bytes:
        header = bytearray([0x80 | (opcode & 0x0F)])
        length = len(payload)
        if length < 126:
            header.append(length)  # no MASK bit — server->client is unmasked
        elif length < (1 << 16):
            header.append(126)
            header.extend(length.to_bytes(2, "big"))
        else:
            header.append(127)
            header.extend(length.to_bytes(8, "big"))
        return bytes(header) + payload

    def test_decodes_unmasked_text_frame(self) -> None:
        frame = self._server_frame(0x1, b"pong")
        opcode, payload = dbus_client._ws_recv_frame(FakeSocket(frame))
        self.assertEqual(opcode, 0x1)
        self.assertEqual(payload, b"pong")

    def test_decodes_unmasked_two_byte_extension(self) -> None:
        body = b"z" * 1000
        opcode, payload = dbus_client._ws_recv_frame(FakeSocket(self._server_frame(0x1, body)))
        self.assertEqual(opcode, 0x1)
        self.assertEqual(payload, body)

    def test_decodes_ping_and_close_opcodes(self) -> None:
        ping_op, ping_payload = dbus_client._ws_recv_frame(FakeSocket(self._server_frame(0x9, b"")))
        self.assertEqual(ping_op, 0x9)
        self.assertEqual(ping_payload, b"")

        close_op, _ = dbus_client._ws_recv_frame(FakeSocket(self._server_frame(0x8, b"")))
        self.assertEqual(close_op, 0x8)


class RecvExactTests(unittest.TestCase):
    def test_eof_before_count_raises_wserror(self) -> None:
        # Only 1 byte available, ask for 4 -> recv returns b"" -> WsError.
        with self.assertRaises(dbus_client.WsError):
            dbus_client._ws_recv_exact(FakeSocket(b"\x00"), 4)

    def test_truncated_frame_payload_raises_wserror(self) -> None:
        # Header claims 10 payload bytes but only 3 are present.
        frame = bytes([0x81, 10]) + b"abc"
        with self.assertRaises(dbus_client.WsError):
            dbus_client._ws_recv_frame(FakeSocket(frame))

    def test_exact_count_returns_all_bytes(self) -> None:
        self.assertEqual(dbus_client._ws_recv_exact(FakeSocket(b"abcdef"), 4), b"abcd")


if __name__ == "__main__":
    unittest.main()
