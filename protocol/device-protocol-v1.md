# StackChan Companion device protocol v1

Status: frozen for D3 implementation  
Wire version: `protocol: 1`  
Transport: authenticated WSS text frames only

## 1. Security and capability boundary

Production WSS exposes only three server commands: `set_expression`, `play_gesture`, and `set_idle_mode`. There is no `move_head`, raw angle, servo speed, GPIO, shell, file, camera, microphone, or audio command in protocol v1. Calibration angles may exist only in an isolated local maintenance mode and can never originate from a model response or production WSS.

Device authentication belongs to the HTTP upgrade request and is specified in D4: `Authorization: Bearer <device token>` plus a separate bounded device-ID header. Tokens are never JSON fields, URL query parameters, log fields, acknowledgements, or shared examples.

Every JSON object uses an exact field set. Missing fields, extra fields, duplicate JSON keys, unknown enum values, wrong field types, non-finite numbers, and unknown field combinations are rejected without guessing an action.

## 2. Transport and resource limits

| Limit | v1 value |
|---|---:|
| Text frame size | at most 4096 UTF-8 bytes |
| JSON nesting | at most 4 object/array levels, root included |
| String size | at most 128 UTF-8 bytes |
| Object members | at most 16 |
| Arrays | not used by v1 messages; reject |
| Binary frames | reject; close code `1003` |
| Device event rate | at most 20 frames in any rolling 1000 ms |
| Command queue | 4 ordinary commands |
| Recent command-ID cache | 16 completed or admitted IDs |
| Default command TTL | 10,000 ms |
| Maximum command TTL | 30,000 ms |
| Heartbeat interval | 15,000 ms |
| Miss threshold | 3 consecutive intervals |

The server supplies Unix epoch milliseconds in command and welcome messages. Firmware compares command expiry with its server-time offset; monotonic time is used for heartbeat, rate limiting, queue time, and reconnect backoff. A command issued more than 5 seconds in the future relative to the server-adjusted receive time is rejected.

## 3. Server-to-device messages

### 3.1 Welcome

```json
{"protocol":1,"type":"welcome","server_time_ms":1700000000000,"heartbeat_interval_ms":15000,"heartbeat_miss_limit":3}
```

The device starts heartbeat scheduling only after this message. `online` is never a trusted device event: the server derives it from the authenticated WSS connection and closes a connection after the missed-heartbeat threshold.

### 3.2 Command envelope

```json
{"protocol":1,"type":"command","id":"11111111-1111-4111-8111-111111111111","issued_at_ms":1700000000000,"expires_at_ms":1700000010000,"command":{"name":"set_expression","expression":"happy"}}
```

`id` is a canonical lowercase UUID v4. `expires_at_ms` must be after `issued_at_ms`, and their difference must not exceed 30 seconds. Expired commands receive an `expired` acknowledgement and are not queued. An already cached ID is acknowledged with its prior terminal state where available and is never executed again.

Exact command payloads:

| Command | Exact payload fields | Allowed values |
|---|---|---|
| `set_expression` | `name`, `expression` | `neutral`, `happy`, `shy`, `sad`, `angry`, `sleepy`, `surprised` |
| `play_gesture` | `name`, `gesture`, `intensity` | gesture: `still`, `nod`, `shake`, `look_left`, `look_right`, `look_up`, `look_down`, `nuzzle`; finite intensity `0.0..1.0` |
| `set_idle_mode` | `name`, `mode` | `on`, `off` |

`still` is valid but performs no servo movement. It may be used to cancel a pending ordinary gesture. Queue overflow returns `busy`; it does not evict an older ordinary command. Local emergency stop, return-to-home, disconnect handling, and offline state have higher priority and may clear the ordinary queue.

### 3.3 Event acknowledgements

```json
{"protocol":1,"type":"heartbeat_ack","seq":42,"server_time_ms":1700000000000}
```

```json
{"protocol":1,"type":"event_ack","event":"battery"}
```

## 4. Device-to-server messages

All events include exactly `protocol: 1` and their listed fields.

| Event | Exact additional fields | Bounds |
|---|---|---|
| `heartbeat` | `seq`, `uptime_ms` | integers; `seq` `0..2147483647`, uptime `0..9223372036854775807` |
| `battery` | `percent` | finite number `0..100` |
| `touch_start` | `zone` | `top` |
| `touch_end` | `zone` | `top` |
| `stroke` | `zone`, `direction` | zone `top`; `left_to_right` or `right_to_left` |
| `button` | `button`, `action` | button `boot`; action `press` or `release` |
| `command_ack` | `id`, `status`, optional `reason` | canonical command UUID; status below |

Only the top sensing area is named in v1 because the three physical Si12T channel positions have not yet been calibrated. Raw samples and long-term touch trajectories are not uploaded. Stage E may introduce a separately versioned mapping after measurement.

`command_ack.status` is one of:

- `accepted`: validated and admitted to execution or accepted as a no-motion `still` command;
- `completed`: finished or safely reached its terminal state;
- `rejected`: unsupported, invalid, or unsafe; `reason` may be `invalid`, `unsupported`, `unsafe`, or `internal`;
- `expired`: not executed because its TTL elapsed;
- `busy`: not admitted because the ordinary queue is full.

`reason` is allowed only with `rejected`. Acknowledgements are short-lived operational state; they are not conversation messages and are never promoted to long-term memory.

## 5. Ordering, duplicate delivery, and reconnect

1. Validate the frame, protocol version, exact schema, UUID, TTL, and command payload before touching the queue.
2. If the ID is in the 16-entry cache, do not execute it; return the cached terminal acknowledgement when possible.
3. If expired, return `expired` and remember the ID without executing it.
4. If the four-entry ordinary queue is full, return `busy`; the server may retry only with a new command ID and a fresh TTL.
5. Otherwise remember/admit the ID, return `accepted`, execute once, then return `completed`.
6. Reconnect clears transport-local pending state but not the recent-ID cache. The cache may be RAM-only and therefore resets on reboot; server-side pending-command state must also suppress replay after reconnect.

## 6. Shared contract examples

`开发/协议/examples/` contains no credentials. Both `开发/服务器/test_device_protocol.py` and the firmware host test `protocol_v1_test` consume these files:

- accepted expression and gesture commands;
- an expired command;
- a command delivered twice;
- an out-of-bounds intensity;
- an unknown raw-angle command;
- an unknown field combination;
- a wrong protocol version.

The acceptance rule is exact: normal samples are accepted by both validators, error samples are rejected by both, the expired sample is classified `expired`, and the duplicate sample is admitted once only.
