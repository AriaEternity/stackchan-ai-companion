"""Strict, bounded StackChan Companion device protocol v1/v2 primitives."""

from __future__ import annotations

from collections import deque
import json
import math
import time
from typing import Any
from uuid import UUID, uuid4

from persona import ALLOWED_EMOTIONS, ALLOWED_GESTURES

PROTOCOL_VERSION = 1
LATEST_PROTOCOL_VERSION = 2
SUPPORTED_PROTOCOL_VERSIONS = frozenset({PROTOCOL_VERSION, LATEST_PROTOCOL_VERSION})
MAX_TEXT_FRAME_BYTES = 4096
MAX_NESTING_DEPTH = 4
MAX_STRING_BYTES = 128
MAX_DISPLAY_TEXT_BYTES = 512
MAX_OBJECT_MEMBERS = 16
MAX_COMMAND_TTL_MS = 30_000
DEFAULT_COMMAND_TTL_MS = 10_000
MAX_COMMAND_FUTURE_SKEW_MS = 5_000
HEARTBEAT_INTERVAL_MS = 15_000
HEARTBEAT_MISS_LIMIT = 3
EVENT_RATE_LIMIT = 20
EVENT_RATE_WINDOW_MS = 1_000
RECENT_COMMAND_ID_CAPACITY = 16

ALLOWED_DEVICE_EVENTS = {
    "heartbeat", "battery", "touch_start", "touch_end", "stroke", "button", "command_ack",
}
ACK_STATUSES = {"accepted", "completed", "rejected", "expired", "busy"}
REJECTION_REASONS = {"invalid", "unsupported", "unsafe", "internal"}


class ProtocolError(ValueError):
    """A safe protocol rejection with a stable machine-readable category."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


class ExpiredCommandError(ProtocolError):
    def __init__(self, command: dict[str, Any]):
        super().__init__("expired", "command has expired")
        self.command = command


def _reject_constant(value: str) -> None:
    raise ProtocolError("invalid_json", f"non-finite JSON number is forbidden: {value}")


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError("duplicate_key", "duplicate JSON key")
        result[key] = value
    return result


def _check_json_limits(value: Any, depth: int = 1, *, max_string_bytes: int = MAX_STRING_BYTES) -> None:
    if depth > MAX_NESTING_DEPTH:
        raise ProtocolError("too_deep", "JSON nesting limit exceeded")
    if isinstance(value, str):
        if len(value.encode("utf-8")) > max_string_bytes:
            raise ProtocolError("string_too_long", "JSON string limit exceeded")
        return
    if isinstance(value, list):
        raise ProtocolError("array_forbidden", "arrays are not used by protocol v1")
    if isinstance(value, dict):
        if len(value) > MAX_OBJECT_MEMBERS:
            raise ProtocolError("too_many_fields", "JSON object member limit exceeded")
        for key, child in value.items():
            _check_json_limits(key, depth, max_string_bytes=max_string_bytes)
            _check_json_limits(child, depth + 1, max_string_bytes=max_string_bytes)


def decode_text_frame(payload: str, *, max_string_bytes: int = MAX_STRING_BYTES) -> dict[str, Any]:
    """Decode one WSS text frame. Binary frames must never be passed here."""
    if (isinstance(max_string_bytes, bool) or not isinstance(max_string_bytes, int)
            or not MAX_STRING_BYTES <= max_string_bytes <= MAX_DISPLAY_TEXT_BYTES):
        raise ProtocolError("invalid_limit", "invalid JSON string limit")
    if not isinstance(payload, str):
        raise ProtocolError("binary_forbidden", "binary frames are forbidden")
    if len(payload.encode("utf-8")) > MAX_TEXT_FRAME_BYTES:
        raise ProtocolError("frame_too_large", "text frame limit exceeded")
    try:
        value = json.loads(payload, object_pairs_hook=_unique_object, parse_constant=_reject_constant)
    except ProtocolError:
        raise
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ProtocolError("invalid_json", "invalid JSON text frame") from exc
    if not isinstance(value, dict):
        raise ProtocolError("invalid_shape", "protocol message must be an object")
    _check_json_limits(value, max_string_bytes=max_string_bytes)
    return value


def _require_exact_fields(value: dict[str, Any], required: set[str], optional: set[str] | None = None) -> None:
    allowed = required | (optional or set())
    if set(value) != required and not (required <= set(value) <= allowed):
        raise ProtocolError("invalid_fields", "message has missing or unknown fields")


def _require_protocol(value: dict[str, Any], protocol_version: int = PROTOCOL_VERSION) -> None:
    protocol = value.get("protocol")
    if protocol_version not in SUPPORTED_PROTOCOL_VERSIONS:
        raise ProtocolError("wrong_protocol", "unsupported protocol version")
    if not _is_int(protocol, protocol_version, protocol_version):
        raise ProtocolError("wrong_protocol", "unsupported protocol version")


def _is_number(value: Any) -> bool:
    return not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(value)


def _is_int(value: Any, minimum: int, maximum: int) -> bool:
    return not isinstance(value, bool) and isinstance(value, int) and minimum <= value <= maximum


def _is_uuid4(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    try:
        parsed = UUID(value)
    except (ValueError, AttributeError):
        return False
    return parsed.version == 4 and str(parsed) == value


def persona_commands(emotion: str, gesture: str, intensity: float) -> list[dict[str, Any]]:
    """Convert model-approved enums into bounded firmware commands."""
    commands = [{"name": "set_expression", "expression": emotion if emotion in ALLOWED_EMOTIONS else "neutral"}]
    numeric_intensity = float(intensity)
    if not math.isfinite(numeric_intensity):
        numeric_intensity = 0.0
    if gesture in ALLOWED_GESTURES and gesture != "still":
        commands.append({
            "name": "play_gesture",
            "gesture": gesture,
            "intensity": max(0.0, min(numeric_intensity, 1.0)),
        })
    return commands


def bounded_display_text(value: str) -> str:
    """Return display-safe text bounded on Unicode character boundaries."""
    if not isinstance(value, str):
        raise ProtocolError("invalid_command", "display text must be a string")
    normalized = value.replace("\r\n", " ").replace("\r", " ").replace("\n", " ").replace("\t", " ")
    sanitized = "".join(
        character for character in normalized
        if ord(character) >= 0x20 and ord(character) != 0x7F
    ).strip()
    if not sanitized:
        sanitized = "..."
    if len(sanitized.encode("utf-8")) <= MAX_DISPLAY_TEXT_BYTES:
        return sanitized

    byte_budget = MAX_DISPLAY_TEXT_BYTES - len("...".encode("utf-8"))
    prefix: list[str] = []
    used = 0
    for character in sanitized:
        encoded_length = len(character.encode("utf-8"))
        if used + encoded_length > byte_budget:
            break
        prefix.append(character)
        used += encoded_length
    return "".join(prefix).rstrip() + "..."


def display_text_command(reply: str) -> dict[str, Any]:
    return {"name": "show_text", "text": bounded_display_text(reply)}


def _valid_display_text(value: Any) -> bool:
    return (isinstance(value, str) and bool(value)
            and len(value.encode("utf-8")) <= MAX_DISPLAY_TEXT_BYTES
            and all(ord(character) >= 0x20 and ord(character) != 0x7F for character in value))


def _validate_command_payload(command: Any, *, protocol_version: int = PROTOCOL_VERSION) -> dict[str, Any]:
    if protocol_version not in SUPPORTED_PROTOCOL_VERSIONS:
        raise ProtocolError("wrong_protocol", "unsupported protocol version")
    if not isinstance(command, dict) or not isinstance(command.get("name"), str):
        raise ProtocolError("invalid_command", "command payload must be an object with a name")
    name = command["name"]
    if name == "set_expression":
        _require_exact_fields(command, {"name", "expression"})
        if command["expression"] not in ALLOWED_EMOTIONS:
            raise ProtocolError("invalid_command", "unsupported expression")
    elif name == "play_gesture":
        _require_exact_fields(command, {"name", "gesture", "intensity"})
        if command["gesture"] not in ALLOWED_GESTURES:
            raise ProtocolError("invalid_command", "unsupported gesture")
        if not _is_number(command["intensity"]) or not 0.0 <= command["intensity"] <= 1.0:
            raise ProtocolError("invalid_command", "gesture intensity must be finite and between 0 and 1")
    elif name == "set_idle_mode":
        _require_exact_fields(command, {"name", "mode"})
        if command["mode"] not in {"on", "off"}:
            raise ProtocolError("invalid_command", "unsupported idle mode")
    elif name == "show_text" and protocol_version >= 2:
        _require_exact_fields(command, {"name", "text"})
        if not _valid_display_text(command["text"]):
            raise ProtocolError("invalid_command", "display text is empty, unsafe, or too long")
    else:
        raise ProtocolError("unsupported_command", "unsafe or unsupported device command")
    return command


def validate_command(message: dict[str, Any], *, received_at_ms: int,
                     protocol_version: int = PROTOCOL_VERSION) -> dict[str, Any]:
    _require_exact_fields(message, {"protocol", "type", "id", "issued_at_ms", "expires_at_ms", "command"})
    _require_protocol(message, protocol_version)
    if message["type"] != "command":
        raise ProtocolError("invalid_type", "expected command message")
    if not _is_uuid4(message["id"]):
        raise ProtocolError("invalid_id", "command id must be a canonical lowercase UUID v4")
    issued_at = message["issued_at_ms"]
    expires_at = message["expires_at_ms"]
    if not _is_int(received_at_ms, 0, 9_223_372_036_854_775_807):
        raise ProtocolError("invalid_time", "receive time must be a non-negative integer")
    if not _is_int(issued_at, 0, 9_223_372_036_854_775_807) or not _is_int(expires_at, 0, 9_223_372_036_854_775_807):
        raise ProtocolError("invalid_time", "command timestamps must be non-negative integers")
    if expires_at <= issued_at or expires_at - issued_at > MAX_COMMAND_TTL_MS:
        raise ProtocolError("invalid_time", "command TTL is invalid")
    if issued_at > received_at_ms + MAX_COMMAND_FUTURE_SKEW_MS:
        raise ProtocolError("invalid_time", "command issue time is too far in the future")
    _validate_command_payload(message["command"], protocol_version=protocol_version)
    if received_at_ms > expires_at:
        raise ExpiredCommandError(message)
    return message


def envelope(command: dict[str, Any], *, now_ms: int | None = None, ttl_ms: int = DEFAULT_COMMAND_TTL_MS,
             command_id: str | None = None, protocol_version: int = PROTOCOL_VERSION) -> dict[str, Any]:
    _validate_command_payload(command, protocol_version=protocol_version)
    if not _is_int(ttl_ms, 1, MAX_COMMAND_TTL_MS):
        raise ProtocolError("invalid_time", "command TTL is out of range")
    issued_at = time.time_ns() // 1_000_000 if now_ms is None else now_ms
    if not _is_int(issued_at, 0, 9_223_372_036_854_775_807):
        raise ProtocolError("invalid_time", "issue time is invalid")
    identifier = str(uuid4()) if command_id is None else command_id
    message = {
        "protocol": protocol_version,
        "type": "command",
        "id": identifier,
        "issued_at_ms": issued_at,
        "expires_at_ms": issued_at + ttl_ms,
        "command": command,
    }
    return validate_command(message, received_at_ms=issued_at, protocol_version=protocol_version)


def validate_event(event: dict[str, Any], *, protocol_version: int = PROTOCOL_VERSION) -> dict[str, Any]:
    if not isinstance(event, dict):
        raise ProtocolError("invalid_shape", "event must be an object")
    _require_protocol(event, protocol_version)
    event_type = event.get("type")
    if event_type not in ALLOWED_DEVICE_EVENTS:
        raise ProtocolError("unsupported_event", "unsupported device event")
    if event_type == "heartbeat":
        _require_exact_fields(event, {"protocol", "type", "seq", "uptime_ms"})
        if not _is_int(event["seq"], 0, 2_147_483_647) or not _is_int(event["uptime_ms"], 0, 9_223_372_036_854_775_807):
            raise ProtocolError("invalid_event", "invalid heartbeat counters")
    elif event_type == "battery":
        _require_exact_fields(event, {"protocol", "type", "percent"})
        if not _is_number(event["percent"]) or not 0 <= event["percent"] <= 100:
            raise ProtocolError("invalid_event", "battery percent must be finite and between 0 and 100")
    elif event_type in {"touch_start", "touch_end"}:
        _require_exact_fields(event, {"protocol", "type", "zone"})
        if event["zone"] != "top":
            raise ProtocolError("invalid_event", "unsupported touch zone")
    elif event_type == "stroke":
        _require_exact_fields(event, {"protocol", "type", "zone", "direction"})
        if event["zone"] != "top" or event["direction"] not in {"left_to_right", "right_to_left"}:
            raise ProtocolError("invalid_event", "invalid stroke")
    elif event_type == "button":
        _require_exact_fields(event, {"protocol", "type", "button", "action"})
        if event["button"] != "boot" or event["action"] not in {"press", "release"}:
            raise ProtocolError("invalid_event", "invalid button event")
    elif event_type == "command_ack":
        _require_exact_fields(event, {"protocol", "type", "id", "status"}, {"reason"})
        if not _is_uuid4(event["id"]) or event["status"] not in ACK_STATUSES:
            raise ProtocolError("invalid_event", "invalid command acknowledgement")
        has_reason = "reason" in event
        if event["status"] == "rejected":
            if has_reason and event["reason"] not in REJECTION_REASONS:
                raise ProtocolError("invalid_event", "invalid rejection reason")
        elif has_reason:
            raise ProtocolError("invalid_fields", "reason is allowed only for rejected acknowledgements")
    return event


class RecentCommandIds:
    """A fixed-size in-memory command-ID cache for at-most-once admission."""

    def __init__(self, capacity: int = RECENT_COMMAND_ID_CAPACITY):
        if capacity <= 0:
            raise ValueError("capacity must be positive")
        self._capacity = capacity
        self._order: deque[str] = deque()
        self._ids: set[str] = set()

    def seen(self, command_id: str) -> bool:
        return command_id in self._ids

    def remember(self, command_id: str) -> bool:
        if command_id in self._ids:
            return False
        if len(self._order) == self._capacity:
            self._ids.remove(self._order.popleft())
        self._order.append(command_id)
        self._ids.add(command_id)
        return True


class SlidingWindowRateLimiter:
    """A deterministic rolling-window limiter driven by caller monotonic time."""

    def __init__(self, limit: int = EVENT_RATE_LIMIT, window_ms: int = EVENT_RATE_WINDOW_MS):
        if limit <= 0 or window_ms <= 0:
            raise ValueError("rate limit and window must be positive")
        self._limit = limit
        self._window_ms = window_ms
        self._events: deque[int] = deque()
        self._last_now_ms: int | None = None

    def allow(self, now_ms: int) -> bool:
        if not _is_int(now_ms, 0, 9_223_372_036_854_775_807):
            return False
        if self._last_now_ms is not None and now_ms < self._last_now_ms:
            return False
        self._last_now_ms = now_ms
        cutoff = now_ms - self._window_ms
        while self._events and self._events[0] <= cutoff:
            self._events.popleft()
        if len(self._events) >= self._limit:
            return False
        self._events.append(now_ms)
        return True
