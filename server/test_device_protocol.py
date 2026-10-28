from __future__ import annotations

import math
from pathlib import Path
import unittest

from device_protocol import (
    ExpiredCommandError,
    MAX_DISPLAY_TEXT_BYTES,
    ProtocolError,
    RecentCommandIds,
    SlidingWindowRateLimiter,
    decode_text_frame,
    display_text_command,
    envelope,
    persona_commands,
    validate_command,
    validate_event,
)


EXAMPLES = Path(__file__).parents[1] / "protocol" / "examples"
EXAMPLES_V2 = Path(__file__).parents[1] / "protocol" / "examples-v2"
RECEIVED_AT_MS = 1_700_000_005_000


def load_example(name: str) -> dict:
    return decode_text_frame((EXAMPLES / name).read_text(encoding="utf-8"))


def load_v2_example(name: str) -> dict:
    return decode_text_frame((EXAMPLES_V2 / name).read_text(encoding="utf-8"),
                             max_string_bytes=MAX_DISPLAY_TEXT_BYTES)


class DeviceProtocolTests(unittest.TestCase):
    def test_v2_display_text_is_bounded_and_v1_stays_frozen(self):
        accepted = validate_command(load_v2_example("accepted-display-text.json"),
                                    received_at_ms=RECEIVED_AT_MS, protocol_version=2)
        self.assertEqual(accepted["command"]["name"], "show_text")
        for name in ("empty-display-text.json", "wrong-protocol-command.json"):
            with self.subTest(name=name), self.assertRaises(ProtocolError):
                validate_command(load_v2_example(name), received_at_ms=RECEIVED_AT_MS,
                                 protocol_version=2)
        with self.assertRaises(ProtocolError):
            envelope({"name": "show_text", "text": "not valid in v1"})

        command = display_text_command("你" * 400 + "\x00\tend")
        encoded = command["text"].encode("utf-8")
        self.assertLessEqual(len(encoded), MAX_DISPLAY_TEXT_BYTES)
        self.assertTrue(command["text"].endswith("..."))
        self.assertNotIn("\x00", command["text"])
        self.assertEqual(encoded.decode("utf-8"), command["text"])
        self.assertEqual(display_text_command("line one\nline two\tend")["text"],
                         "line one line two end")
        message = envelope(command, protocol_version=2)
        self.assertEqual(message["protocol"], 2)

    def test_shared_accepted_commands(self):
        for name in ("accepted-expression.json", "accepted-gesture.json", "duplicate-command.json"):
            with self.subTest(name=name):
                self.assertEqual(validate_command(load_example(name), received_at_ms=RECEIVED_AT_MS)["protocol"], 1)

    def test_shared_expired_command(self):
        with self.assertRaises(ExpiredCommandError):
            validate_command(load_example("expired-command.json"), received_at_ms=1_700_000_001_000)

    def test_shared_rejected_commands(self):
        names = (
            "out-of-bounds-command.json",
            "unknown-command.json",
            "unknown-field-command.json",
            "wrong-protocol-command.json",
        )
        for name in names:
            with self.subTest(name=name), self.assertRaises(ProtocolError):
                validate_command(load_example(name), received_at_ms=RECEIVED_AT_MS)

        wrong_type = load_example("accepted-expression.json")
        wrong_type["protocol"] = 1.0
        with self.assertRaises(ProtocolError):
            validate_command(wrong_type, received_at_ms=RECEIVED_AT_MS)

    def test_duplicate_command_is_admitted_once(self):
        command = validate_command(load_example("duplicate-command.json"), received_at_ms=RECEIVED_AT_MS)
        recent = RecentCommandIds(capacity=2)
        self.assertTrue(recent.remember(command["id"]))
        self.assertTrue(recent.seen(command["id"]))
        self.assertFalse(recent.remember(command["id"]))
        self.assertTrue(recent.remember("99999999-9999-4999-8999-999999999999"))
        self.assertTrue(recent.remember("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"))
        self.assertFalse(recent.seen(command["id"]))

    def test_rate_limiter_boundaries_and_monotonic_time(self):
        limiter = SlidingWindowRateLimiter(limit=3, window_ms=1_000)
        self.assertTrue(limiter.allow(1_000))
        self.assertTrue(limiter.allow(1_001))
        self.assertTrue(limiter.allow(1_999))
        self.assertFalse(limiter.allow(1_999))
        self.assertTrue(limiter.allow(2_000))
        self.assertFalse(limiter.allow(1_999))

    def test_frame_limits_and_strict_json(self):
        invalid_frames = (
            b'{"protocol":1}',
            "[]",
            '{"protocol":1,"protocol":1}',
            '{"protocol":NaN}',
            '{"a":{"b":{"c":{"d":1}}}}',
            '{"protocol":1,"values":[]}',
            " " * 4097,
        )
        for payload in invalid_frames:
            with self.subTest(payload_type=type(payload).__name__), self.assertRaises(ProtocolError):
                decode_text_frame(payload)  # type: ignore[arg-type]

    def test_exact_event_schemas_and_ack_statuses(self):
        valid = (
            {"protocol": 1, "type": "heartbeat", "seq": 0, "uptime_ms": 0},
            {"protocol": 1, "type": "battery", "percent": 78.5},
            {"protocol": 1, "type": "touch_start", "zone": "top"},
            {"protocol": 1, "type": "touch_end", "zone": "top"},
            {"protocol": 1, "type": "stroke", "zone": "top", "direction": "left_to_right"},
            {"protocol": 1, "type": "button", "button": "boot", "action": "press"},
            {"protocol": 1, "type": "command_ack", "id": "11111111-1111-4111-8111-111111111111", "status": "accepted"},
            {"protocol": 1, "type": "command_ack", "id": "11111111-1111-4111-8111-111111111111", "status": "completed"},
            {"protocol": 1, "type": "command_ack", "id": "11111111-1111-4111-8111-111111111111", "status": "rejected", "reason": "unsafe"},
            {"protocol": 1, "type": "command_ack", "id": "11111111-1111-4111-8111-111111111111", "status": "expired"},
            {"protocol": 1, "type": "command_ack", "id": "11111111-1111-4111-8111-111111111111", "status": "busy"},
        )
        for event in valid:
            with self.subTest(event=event["type"]):
                self.assertIs(validate_event(event), event)

        invalid = (
            {"protocol": 1, "type": "stroke", "zone": "top", "direction": "up"},
            {"protocol": 1, "type": "touch_start", "zone": "cheek"},
            {"protocol": 1, "type": "battery", "percent": math.inf},
            {"protocol": 1, "type": "command_ack", "id": "11111111-1111-4111-8111-111111111111", "status": "completed", "reason": "unsafe"},
            {"protocol": 1, "type": "online"},
        )
        for event in invalid:
            with self.subTest(event=event["type"]), self.assertRaises(ProtocolError):
                validate_event(event)

    def test_envelope_has_ttl_and_never_exposes_raw_angles(self):
        message = envelope(
            {"name": "set_expression", "expression": "happy"},
            now_ms=1_700_000_000_000,
            command_id="11111111-1111-4111-8111-111111111111",
        )
        self.assertEqual(message["protocol"], 1)
        self.assertEqual(message["expires_at_ms"] - message["issued_at_ms"], 10_000)
        with self.assertRaises(ProtocolError):
            envelope({"name": "move_head", "yaw": 1, "pitch": 2})
        self.assertEqual(persona_commands("happy", "nod", math.nan)[1]["intensity"], 0.0)


if __name__ == "__main__":
    unittest.main()
