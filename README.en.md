# StackChan AI Companion

**A small desktop companion connected through text, memory and touch.**

[简体中文](README.md) · [Getting started](docs/getting-started.md) · [Hardware status](docs/hardware-status.md)

[![CI](https://github.com/AriaEternity/stackchan-ai-companion/actions/workflows/ci.yml/badge.svg)](https://github.com/AriaEternity/stackchan-ai-companion/actions/workflows/ci.yml)

![Architecture overview](docs/images/overview.svg)

A self-hosted experiment for the M5Stack StackChan K151: a browser chat UI, an authenticated FastAPI gateway, confirmed long-term memory, a bounded device protocol, and ESP32-S3 firmware extensions.

**Experimental hardware status:** physical gesture amplitude, direction and completion still need renewed on-device validation. Development is paused and the original device was restored to factory firmware. This repository publishes source code, not a ready-to-flash binary. The proposed next phase of text-driven horizontal motion has not been implemented.

## Implemented software

- Single-owner web login, persisted messages, incremental history and command status.
- Recent messages, bounded rolling summaries and retrieval of confirmed memories. Model-generated memory candidates require confirmation.
- A DeepSeek-compatible JSON adapter with structured retries and explicit fallback.
- Versioned device messages, header authentication, heartbeats, TTL, deduplication and acknowledgements.
- Bounded secure WebSocket handling, offline recovery, on-screen text, expressions and touch sessions.
- Named gestures with a default server motion lock and device calibration gate.
- Offline Python regression, shared protocol fixtures and C++ host tests.

A `completed` acknowledgement alone does not prove that a physical gesture matched its intended direction or amplitude. Camera, microphone, speech recognition and speech output are not enabled in Companion.

## Run without a device or model key

```bash
python -m venv .venv
# Windows: .venv\Scripts\activate
source .venv/bin/activate
python -m pip install -r requirements-dev.txt
python tools/demo.py
```

Open `http://127.0.0.1:8787` and use password `local-demo`. This loopback-only demo uses a deterministic fake model and a temporary database. It does not connect to physical hardware or call a paid model.

![Real web UI with synthetic demo data](docs/images/web-demo.png)

Self-hosting requires your own credentials and HTTPS; see [deployment](docs/deployment.md). The detailed technical guides are currently in Chinese, with runnable commands and configuration examples.

## Repository

| Path | Purpose |
| --- | --- |
| `server/` | Gateway, storage, web UI and tests |
| `firmware/` | ESP-IDF sources and host tests |
| `protocol/` | v1/v2 contracts and shared fixtures |
| `examples/` | Synthetic example character |
| `deploy/` | Generic deployment templates |
| `tools/` | Demo, dependency fetching, provisioning helper and publication checks |

The firmware derives from [M5Stack/StackChan at b72b3ed](https://github.com/m5stack/StackChan/tree/b72b3ede38b32d54f0b6ba51c62cfcef2ec3ae1e), whose public source version is `1.4.3`. This is an independently prepared source repository. Upstream notices remain intact. The project-owned code is MIT; see [third-party notices](THIRD-PARTY-NOTICES.md).

No personal character documents, conversations, memories, device backups, NVS dumps, credentials or production configurations are distributed.
