# StackChan Companion device protocol v2

Status: active for `companion-0.2.4+base1.4.3` software validation  
Wire version: `protocol: 2`  
Transport: authenticated WSS text frames only

## 1. Compatibility and negotiation

Protocol v1 remains frozen in `device-protocol-v1.md`. A v1 device sends no protocol header and the server treats an absent header as v1. A v2 device adds `X-StackChan-Protocol: 2` to the authenticated WebSocket upgrade. The header value must be exactly `1` or `2`; an unknown value is rejected before upgrade.

After negotiation, every message on that WebSocket uses the selected protocol number. Mixing v1 and v2 messages closes the connection. Authentication remains `Authorization: Bearer <device token>` plus `X-StackChan-Device-ID`; credentials never appear in the URL or JSON.

## 2. v2 command extension

v2 retains the three v1 commands unchanged and adds one display-only command:

```json
{"protocol":2,"type":"command","id":"99999999-9999-4999-8999-999999999999","issued_at_ms":1700000000000,"expires_at_ms":1700000010000,"command":{"name":"show_text","text":"neko is here."}}
```

| Command | Exact payload fields | Bounds and behavior |
|---|---|---|
| `show_text` | `name`, `text` | Non-empty valid UTF-8; at most 512 decoded UTF-8 bytes; C0 and DEL controls are forbidden |

The server turns line breaks and tabs into spaces, removes other controls, and truncates on a Unicode character boundary. If truncation is needed, the final three bytes are ASCII `...`, included in the 512-byte limit. The firmware never receives the unchecked web reply.

For a normal v2 reply, the server sends `set_expression`, an optional allowed `play_gesture`, and then `show_text`. Placing text last prevents the expression command from immediately dismissing the same reply. If the server motion policy is locked, a non-`still` model gesture is not placed on the wire; the gateway records a safe `rejected/server_motion_locked` result instead.

`show_text` is a short synchronous display operation. A `completed` acknowledgement means the bounded display update was accepted; it does not mean a full-screen message remained visible for its entire presentation time. In `companion-0.2.4+base1.4.3`, a reply of at most 64 UTF-8 bytes without a line break clears full-screen text and leaves the face visible. Longer text enters full-screen mode for at most 15 seconds. A later `set_expression`, local head touch, screen click, connection loss or app close also clears text and restores the face. Local touch does not wait for a network command.

Empty text, invalid UTF-8, forbidden controls, over-limit decoded text, extra fields and `show_text` under protocol v1 are rejected. The command cannot access files, audio, camera, microphone, GPIO, raw display buffers or arbitrary LVGL objects.

## 3. Resource limits

All v1 frame, nesting, object, queue, UUID, TTL, acknowledgement, heartbeat and event-rate limits remain unchanged. General strings remain bounded to 128 UTF-8 bytes. Only `show_text.text` has the dedicated 512-byte decoded limit; the total WSS text frame remains at most 4096 UTF-8 bytes.

Device events and acknowledgements keep their v1 schemas but carry `protocol: 2` on a v2 connection. Shared no-secret v2 fixtures are stored in `开发/协议/examples-v2/`; v1 fixtures remain unchanged in `开发/协议/examples/`.

## 4. Safety boundary

v2 still exposes no raw angle, servo speed, shell, file, URL, GPIO, camera, microphone or audio command. The existing named gesture whitelist, calibration lock, queue, cooldown, motion budget, offline stop and recent-ID behavior are unchanged. The production server additionally enforces its own fail-closed motion policy before a gesture reaches WSS; this policy is not device calibration telemetry. Speech output is not part of v2 and remains an optional later version.
