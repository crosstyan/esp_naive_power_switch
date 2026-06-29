#!/usr/bin/env python3
"""Client for the ESP32 UDP power switch service."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import socket
import struct
import sys
import time
from typing import Optional, Tuple


REQ_MAGIC = b"PSW2"
ACK_MAGIC = b"PSA1"
DEFAULT_PORT = 44444

REQ = struct.Struct("!4sHBBHH")
ACK = struct.Struct("!4sHBBH")

CMD_STATUS = 1
CMD_SET_LINES = 2
CMD_PULSE_LINES = 3

STATUS_NAMES = {
    0: "ok",
    1: "bad_magic",
    2: "bad_length",
    3: "bad_command",
    4: "internal_error",
    5: "bad_flags",
    6: "bad_mask",
}

LINE_RESET = 1 << 0
LINE_BOOT = 1 << 1
LINE_ALL = LINE_RESET | LINE_BOOT

STATE_RESET_ASSERTED = LINE_RESET
STATE_BOOT_ASSERTED = LINE_BOOT


class PowerSwitchError(RuntimeError):
    """Base exception for power-switch client failures."""


class PowerSwitchProtocolError(PowerSwitchError):
    """Raised when the device returns an invalid or non-OK response."""


@dataclass(frozen=True)
class Ack:
    sequence: int
    status: int
    state_bits: int
    detail: int

    @property
    def status_name(self) -> str:
        return STATUS_NAMES.get(self.status, f"unknown_{self.status}")

    @property
    def reset_asserted(self) -> bool:
        return bool(self.state_bits & STATE_RESET_ASSERTED)

    @property
    def boot_asserted(self) -> bool:
        return bool(self.state_bits & STATE_BOOT_ASSERTED)

    def as_dict(self) -> dict[str, object]:
        return {
            "sequence": self.sequence,
            "status": self.status_name,
            "state_bits": self.state_bits,
            "reset_asserted": self.reset_asserted,
            "boot_asserted": self.boot_asserted,
            "detail": self.detail,
        }


class PowerSwitchClient:
    def __init__(
        self,
        host: str,
        port: int = DEFAULT_PORT,
        timeout: float = 3.0,
        bind: Optional[Tuple[str, int]] = None,
    ) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.bind = bind
        self._sequence = 0

    def status(self) -> Ack:
        return self._request(CMD_STATUS)

    def set_lines(self, mask: int, asserted_bits: int) -> Ack:
        return self._request(CMD_SET_LINES, mask, asserted_bits)

    def pulse_lines(self, mask: int, duration_ms: int) -> Ack:
        return self._request(CMD_PULSE_LINES, mask, duration_ms)

    def reset(self, pulse_ms: int = 300) -> Ack:
        return self.pulse_lines(LINE_RESET, pulse_ms)

    def enter_maskrom(self, reset_ms: int = 300, boot_hold_ms: int = 1500) -> Ack:
        self.hold_boot()
        ack = self.reset(pulse_ms=reset_ms)
        time.sleep(boot_hold_ms / 1000.0)
        self.release_boot()
        return ack

    def entering_maskrom(self, reset_ms: int = 300, boot_hold_ms: int = 1500) -> Ack:
        return self.enter_maskrom(reset_ms=reset_ms, boot_hold_ms=boot_hold_ms)

    def release_all(self) -> Ack:
        return self.set_lines(LINE_ALL, 0)

    def hold_reset(self) -> Ack:
        return self.set_lines(LINE_RESET, LINE_RESET)

    def hold_boot(self) -> Ack:
        return self.set_lines(LINE_BOOT, LINE_BOOT)

    def hold_both(self) -> Ack:
        return self.set_lines(LINE_ALL, LINE_ALL)

    def release_reset(self) -> Ack:
        return self.set_lines(LINE_RESET, 0)

    def release_boot(self) -> Ack:
        return self.set_lines(LINE_BOOT, 0)

    def _next_sequence(self) -> int:
        self._sequence = (self._sequence + 1) & 0xFFFF
        return self._sequence

    def _request(self, command: int, arg0: int = 0, arg1: int = 0) -> Ack:
        sequence = self._next_sequence()
        packet = REQ.pack(REQ_MAGIC, sequence, command, 0, arg0, arg1)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(self.timeout)
            if self.bind is not None:
                sock.bind(self.bind)
            sock.sendto(packet, (self.host, self.port))
            data, _addr = sock.recvfrom(64)

        if len(data) != ACK.size:
            raise PowerSwitchProtocolError(f"bad ack length: {len(data)}")

        magic, ack_sequence, status, state_bits, detail = ACK.unpack(data)
        if magic != ACK_MAGIC:
            raise PowerSwitchProtocolError(f"bad ack magic: {magic!r}")
        if ack_sequence != sequence:
            raise PowerSwitchProtocolError(
                f"sequence mismatch: expected {sequence}, got {ack_sequence}"
            )

        ack = Ack(
            sequence=ack_sequence,
            status=status,
            state_bits=state_bits,
            detail=detail,
        )
        if ack.status != 0:
            raise PowerSwitchProtocolError(
                f"device returned {ack.status_name}, detail={ack.detail}"
            )
        return ack


def _positive_int(value: str) -> int:
    parsed = int(value, 10)
    if parsed < 0 or parsed > 65535:
        raise argparse.ArgumentTypeError("value must be in range 0..65535")
    return parsed


def _line_mask(value: str) -> int:
    normalized = value.lower().replace("_", "-")
    if normalized in ("reset", "k6", "resetn"):
        return LINE_RESET
    if normalized in ("boot", "k5", "saradc0-boot", "maskrom"):
        return LINE_BOOT
    if normalized in ("both", "all"):
        return LINE_ALL
    raise argparse.ArgumentTypeError("line must be reset, boot, or both")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Control an ESP32 UDP power switch")
    parser.add_argument("host", help="ESP32 IPv4 address or hostname")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="UDP port")
    parser.add_argument("--timeout", type=float, default=3.0, help="UDP response timeout")

    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("status", help="Read current commanded GPIO state")
    subparsers.add_parser("release-all", help="Release RESETn and SARADC0_BOOT")
    subparsers.add_parser("hold-reset", help="Hold K6 RESETn low until release-all")
    subparsers.add_parser("hold-boot", help="Hold K5 SARADC0_BOOT low until release-all")
    subparsers.add_parser("hold-both", help="Hold both control lines low until release-all")

    assert_line = subparsers.add_parser("assert", help="Primitive: assert selected line(s)")
    assert_line.add_argument("line", type=_line_mask, help="reset, boot, or both")

    release_line = subparsers.add_parser("release", help="Primitive: release selected line(s)")
    release_line.add_argument("line", type=_line_mask, help="reset, boot, or both")

    pulse = subparsers.add_parser("pulse", help="Primitive: pulse selected line(s)")
    pulse.add_argument("line", type=_line_mask, help="reset, boot, or both")
    pulse.add_argument("--duration-ms", type=_positive_int, default=300)

    reset = subparsers.add_parser("reset", help="Pulse K6 RESETn")
    reset.add_argument("--pulse-ms", type=_positive_int, default=300)

    maskrom = subparsers.add_parser("maskrom", help="Enter Maskrom via K5 then K6")
    maskrom.add_argument("--reset-ms", type=_positive_int, default=300)
    maskrom.add_argument("--boot-hold-ms", type=_positive_int, default=1500)

    enter_maskrom = subparsers.add_parser("enter-maskrom", help="Alias for maskrom")
    enter_maskrom.add_argument("--reset-ms", type=_positive_int, default=300)
    enter_maskrom.add_argument("--boot-hold-ms", type=_positive_int, default=1500)

    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    client = PowerSwitchClient(args.host, port=args.port, timeout=args.timeout)

    if args.command == "status":
        ack = client.status()
    elif args.command == "release-all":
        ack = client.release_all()
    elif args.command == "hold-reset":
        ack = client.hold_reset()
    elif args.command == "hold-boot":
        ack = client.hold_boot()
    elif args.command == "hold-both":
        ack = client.hold_both()
    elif args.command == "assert":
        ack = client.set_lines(args.line, args.line)
    elif args.command == "release":
        ack = client.set_lines(args.line, 0)
    elif args.command == "pulse":
        ack = client.pulse_lines(args.line, args.duration_ms)
    elif args.command == "reset":
        ack = client.reset(pulse_ms=args.pulse_ms)
    elif args.command in ("maskrom", "enter-maskrom"):
        ack = client.enter_maskrom(reset_ms=args.reset_ms, boot_hold_ms=args.boot_hold_ms)
    else:
        raise AssertionError(f"unhandled command {args.command!r}")

    print(json.dumps(ack.as_dict(), sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, PowerSwitchError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
