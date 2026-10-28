"""Provision initial Companion credentials over a physical USB serial link."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
import time


FRAME_PREFIX = b"SC-PROVISION-1 "
READY = b"SC-PROVISION-READY"
OK = b"SC-PROVISION-OK"
ERROR = b"SC-PROVISION-ERROR"
LOCKED = b"SC-PROVISION-LOCKED"
DEVICE_ID_PATTERN = re.compile(rb"[A-Za-z0-9_.-]{1,64}\Z")
TOKEN_PATTERN = re.compile(rb"[A-Za-z0-9._~+/=-]{1,128}\Z")


def load_ascii_value(path: Path, pattern: re.Pattern[bytes], label: str) -> bytearray:
    raw = bytearray(path.read_bytes().strip())
    if not pattern.fullmatch(raw):
        for index in range(len(raw)):
            raw[index] = 0
        raise ValueError(f"{label} file does not contain a valid value")
    return raw


def build_frame(device_id: bytearray, token: bytearray) -> bytearray:
    return bytearray(FRAME_PREFIX) + device_id + b" " + token + b"\n"


def wipe(value: bytearray) -> None:
    for index in range(len(value)):
        value[index] = 0


def provision(port: str, device_id_path: Path, token_path: Path, timeout_seconds: float) -> None:
    import serial

    device_id = load_ascii_value(device_id_path, DEVICE_ID_PATTERN, "device ID")
    token = load_ascii_value(token_path, TOKEN_PATTERN, "device token")
    frame = build_frame(device_id, token)
    try:
        connection = serial.Serial()
        connection.port = port
        connection.baudrate = 115200
        connection.timeout = 0.25
        # USB Serial/JTAG can report a short write while its bulk endpoint is
        # draining even though the device is still accepting console input.
        # Use a blocking write so a valid frame is not truncated by pyserial's
        # timeout path.
        connection.write_timeout = None
        connection.dtr = False
        connection.rts = False
        connection.open()
        try:
            deadline = time.monotonic() + timeout_seconds
            pending = bytearray()
            sent = False
            while time.monotonic() < deadline:
                chunk = connection.read(256)
                if not chunk:
                    continue
                pending.extend(chunk)
                if len(pending) > 4096 and b"\n" not in pending:
                    pending.clear()
                while b"\n" in pending:
                    line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    status = line.strip(b"\r")
                    if status == READY and not sent:
                        connection.write(frame)
                        connection.flush()
                        sent = True
                    elif status == OK:
                        print("Companion provisioning completed.")
                        return
                    elif status == ERROR:
                        raise RuntimeError("device rejected the provisioning frame")
                    elif status == LOCKED:
                        raise RuntimeError("device provisioning input is locked until the app is reopened")
            if not sent:
                raise TimeoutError("device did not enter unconfigured Companion provisioning mode")
            raise TimeoutError("device did not acknowledge provisioning")
        finally:
            connection.close()
    finally:
        wipe(frame)
        wipe(device_id)
        wipe(token)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--device-id-file", required=True, type=Path)
    parser.add_argument("--token-file", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()
    try:
        provision(args.port, args.device_id_file, args.token_file, args.timeout)
    except Exception as exc:
        print(f"Provisioning failed: {type(exc).__name__}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
