"""Authenticated production gateway for StackChan chat and device events."""

from __future__ import annotations

import asyncio
from collections import deque
from contextlib import asynccontextmanager
from datetime import datetime, timezone
import json
from pathlib import Path
import secrets
import time
from typing import Any

from fastapi import Cookie, Depends, FastAPI, Header, HTTPException, Query, Request, WebSocket, WebSocketDisconnect, status
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from auth import AuthenticationError, Authenticator
from config import Settings
from conversation_context import RECENT_MESSAGE_LIMIT, refresh_rolling_summary
from deepseek import DeepSeekClient, DeepSeekError, safe_fallback
from device_protocol import (
    HEARTBEAT_INTERVAL_MS,
    HEARTBEAT_MISS_LIMIT,
    LATEST_PROTOCOL_VERSION,
    SUPPORTED_PROTOCOL_VERSIONS,
    ProtocolError,
    SlidingWindowRateLimiter,
    decode_text_frame,
    display_text_command,
    envelope,
    persona_commands,
    validate_event,
)
from persona import build_system_prompt, load_persona
from service_store import ServiceStore
from touch_reply import (
    TOUCH_CONVERSATION_ID,
    TOUCH_REPLY_SYSTEM_SUFFIX,
    TOUCH_REPLY_TIMEOUT_SECONDS,
    TouchReaction,
    TouchReplyCoordinator,
    constrain_touch_result,
    touch_fallback_result,
)


COMMAND_HISTORY_LIMIT = 32
COMMAND_HISTORY_FIELDS = (
    "id", "name", "source", "gesture", "intensity",
    "status", "reason", "issued_at", "sent_at", "updated_at",
)
ACTION_GESTURE_INTENSITIES = {
    "nod": 1.0,
    "shake": 1.0,
    "look_left": 1.0,
    "look_right": 1.0,
    "look_up": 1.0,
    "look_down": 1.0,
    "nuzzle": 1.0,
}
TOUCH_GESTURES = {
    "tap": "nod",
    "long_hold": "nuzzle",
    "stroke": "nuzzle",
}
TOUCH_GESTURE_INTENSITY = 0.2


class LoginRequest(BaseModel):
    password: str = Field(min_length=1, max_length=256)


class ChatRequest(BaseModel):
    message: str = Field(min_length=1, max_length=4000)
    conversation_id: str = Field(default="default", min_length=1, max_length=100)


class ActionRequest(BaseModel):
    gesture: str = Field(min_length=1, max_length=32)


class MemoryRequest(BaseModel):
    content: str = Field(min_length=1, max_length=2000)
    category: str = Field(default="fact", max_length=64)
    importance: float = Field(default=0.5, ge=0.0, le=1.0)


class MemoryUpdate(BaseModel):
    content: str | None = Field(default=None, max_length=2000)
    category: str | None = Field(default=None, max_length=64)
    importance: float | None = Field(default=None, ge=0.0, le=1.0)
    status: str | None = None


def create_app(settings: Settings | None = None, *, store: ServiceStore | None = None,
               client: DeepSeekClient | None = None) -> FastAPI:
    settings = settings or Settings.from_env()
    store = store or ServiceStore(settings.database_url)
    client = client or DeepSeekClient(api_key=settings.deepseek_api_key, model=settings.deepseek_model)
    auth = Authenticator(settings.admin_password, settings.auth_secret, settings.token_ttl_seconds)
    @asynccontextmanager
    async def lifespan(_: FastAPI):
        store.create_schema()
        try:
            yield
        finally:
            tasks = tuple(getattr(app.state, "touch_reply_tasks", ()))
            for task in tasks:
                task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)

    app = FastAPI(title="StackChan Gateway", docs_url=None, redoc_url=None, lifespan=lifespan)
    app.state.settings = settings
    app.state.store = store
    app.state.client = client
    app.state.auth = auth
    app.state.device_socket: WebSocket | None = None
    app.state.device_state = {
        "online": False,
        "battery": None,
        "last_seen": None,
        "last_heartbeat": None,
        "last_ack": None,
        "protocol": None,
    }
    app.state.pending_commands: dict[str, dict[str, Any]] = {}
    app.state.command_history: deque[dict[str, Any]] = deque(maxlen=COMMAND_HISTORY_LIMIT)
    app.state.device_send_lock = asyncio.Lock()
    app.state.touch_reply_coordinator = TouchReplyCoordinator()
    app.state.touch_reply_tasks: set[asyncio.Task[None]] = set()
    app.state.touch_reply_status = "idle"
    app.state.touch_reply_error: str | None = None
    index_path = Path(__file__).resolve().parent / "web" / "index.html"
    app.mount("/static", StaticFiles(directory=index_path.parent), name="static")

    @app.middleware("http")
    async def security_headers(request: Request, call_next):
        response = await call_next(request)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Referrer-Policy"] = "same-origin"
        response.headers["Cache-Control"] = "no-store"
        return response

    def require_owner(authorization: str | None = Header(default=None), stackchan_session: str | None = Cookie(default=None)) -> dict[str, Any]:
        token = stackchan_session
        if authorization and authorization.lower().startswith("bearer "):
            token = authorization[7:].strip()
        if not token:
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="login required")
        try:
            return auth.verify(token)
        except AuthenticationError as exc:
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid or expired session") from exc

    @app.get("/health")
    async def health() -> dict[str, Any]:
        return {"status": "ok", "deepseek_configured": client.configured, "production_secrets_ready": settings.is_ready_for_production()}

    @app.get("/")
    async def index() -> FileResponse:
        return FileResponse(index_path, media_type="text/html; charset=utf-8")

    @app.post("/api/login")
    async def login(request: LoginRequest) -> JSONResponse:
        try:
            token = auth.authenticate(request.password)
        except AuthenticationError as exc:
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid credentials") from exc
        response = JSONResponse({"ok": True})
        response.set_cookie("stackchan_session", token, httponly=True, secure=True, samesite="lax", max_age=settings.token_ttl_seconds)
        return response

    @app.post("/api/action")
    async def action(request: ActionRequest, _: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        intensity = ACTION_GESTURE_INTENSITIES.get(request.gesture)
        if intensity is None:
            raise HTTPException(status_code=422, detail="unsupported gesture")
        protocol_version = app.state.device_state["protocol"] or LATEST_PROTOCOL_VERSION
        command = envelope({
            "name": "play_gesture",
            "gesture": request.gesture,
            "intensity": intensity,
        }, protocol_version=protocol_version)
        if settings.motion_locked:
            record = reject_server_command(command, "server_motion_locked", source="web_action")
        else:
            record = await broadcast(command, source="web_action")
        return {
            "gesture": request.gesture,
            "intensity": intensity,
            "motion_locked": settings.motion_locked,
            "command": record,
        }

    @app.post("/api/chat")
    async def chat(request: ChatRequest, _: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        conversation_id = request.conversation_id
        store.ensure_conversation(conversation_id)
        recent = store.recent_messages(conversation_id, RECENT_MESSAGE_LIMIT)
        memories = store.relevant_memories(request.message)
        summary = refresh_rolling_summary(store, conversation_id)
        prompt = build_system_prompt(
            load_persona(settings.persona_path), summary, memories,
            motion_locked=settings.motion_locked,
        )
        messages = [{"role": item["role"], "content": item["content"]} for item in recent]
        messages.append({"role": "user", "content": request.message.strip()})
        try:
            result = client.complete(prompt, messages)
            service_status = "ok"
        except DeepSeekError as exc:
            result = safe_fallback(reason="deepseek_error")
            result["_model_attempts"] = exc.attempts
            service_status = "degraded"
        model_output_status = result.pop("_normalization_status", "ok")
        model_attempts = int(result.pop("_model_attempts", 0 if service_status == "degraded" else 1))
        result.pop("_fallback_reason", None)
        if model_output_status == "fallback":
            service_status = "degraded"
        persisted_messages = [
            store.add_message(conversation_id, "user", request.message.strip()),
            store.add_message(conversation_id, "assistant", result["reply"]),
        ]
        for candidate in result.get("memory_candidates", []):
            store.add_memory(candidate, source="model", status="candidate")
        protocol_version = app.state.device_state["protocol"] or LATEST_PROTOCOL_VERSION
        blocked_motion_command = None
        if settings.motion_locked and result["gesture"] != "still":
            blocked_motion_command = envelope({
                "name": "play_gesture",
                "gesture": result["gesture"],
                "intensity": result["intensity"],
            }, protocol_version=protocol_version)
            result["gesture"] = "still"
            result["intensity"] = 0.0
        payloads = persona_commands(result["emotion"], result["gesture"], result["intensity"])
        if protocol_version >= 2:
            # Expression first restores the face; full-screen text must be the final
            # visual command so the same reply does not immediately dismiss itself.
            payloads.append(display_text_command(result["reply"]))
        commands = [envelope(command, protocol_version=protocol_version) for command in payloads]
        for command in commands:
            await broadcast(command, source="chat")
        motion_rejection = None
        if blocked_motion_command is not None:
            motion_rejection = reject_server_command(
                blocked_motion_command, "server_motion_locked", source="chat")
        return {**result, "conversation_id": conversation_id, "service_status": service_status,
                "model_output_status": model_output_status, "model_attempts": model_attempts,
                "commands": commands, "messages": persisted_messages,
                "motion_rejection": motion_rejection}

    @app.get("/api/messages")
    async def messages(conversation_id: str = Query(default="default", min_length=1, max_length=100),
                       after_id: int | None = Query(default=None, ge=0),
                       limit: int = Query(default=100, ge=1, le=100),
                       _: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        if after_id is None:
            items = store.recent_messages(conversation_id, limit)
            has_more = False
            next_after_id = items[-1]["id"] if items else 0
        else:
            items, has_more = store.messages_after(conversation_id, after_id, limit)
            next_after_id = items[-1]["id"] if items else after_id
        return {
            "conversation_id": conversation_id,
            "items": items,
            "next_after_id": next_after_id,
            "has_more": has_more,
        }

    @app.get("/api/memories")
    async def memories(_: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        return {"items": store.list_memories()}

    @app.post("/api/memories", status_code=201)
    async def add_memory(request: MemoryRequest, _: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        return store.add_memory(request.content, category=request.category, importance=request.importance)

    @app.patch("/api/memories/{memory_id}")
    async def update_memory(memory_id: int, request: MemoryUpdate, _: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        try:
            return store.update_memory(memory_id, **request.model_dump(exclude_unset=True))
        except KeyError as exc:
            raise HTTPException(status_code=404, detail="memory not found") from exc

    @app.delete("/api/memories/{memory_id}")
    async def delete_memory(memory_id: int, _: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        try:
            return store.update_memory(memory_id, status="deleted")
        except KeyError as exc:
            raise HTTPException(status_code=404, detail="memory not found") from exc

    @app.get("/api/export")
    async def export_data(conversation_id: str = "default", _: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        return store.export(conversation_id)

    @app.get("/api/device")
    async def device_state(_: dict[str, Any] = Depends(require_owner)) -> dict[str, Any]:
        cleanup_pending_commands()
        return {
            **app.state.device_state,
            "pending_command_count": len(app.state.pending_commands),
            "command_history": [command_record_view(item) for item in reversed(app.state.command_history)],
            "server_policy": {"motion_locked": settings.motion_locked},
        }

    def now_iso() -> str:
        return datetime.now(timezone.utc).isoformat(timespec="seconds")

    def now_epoch_ms() -> int:
        return time.time_ns() // 1_000_000

    def epoch_ms_iso(value: int) -> str:
        return datetime.fromtimestamp(value / 1000, timezone.utc).isoformat(timespec="milliseconds")

    def command_record_view(record: dict[str, Any]) -> dict[str, Any]:
        return {field: record.get(field) for field in COMMAND_HISTORY_FIELDS}

    def new_command_record(command: dict[str, Any], *, source: str) -> dict[str, Any]:
        timestamp = now_iso()
        payload = command["command"]
        gesture = payload.get("gesture") if payload.get("name") == "play_gesture" else None
        intensity = payload.get("intensity") if gesture is not None else None
        record = {
            "id": command["id"],
            "name": payload["name"],
            "source": source,
            "gesture": gesture,
            "intensity": intensity,
            "status": "pending",
            "reason": None,
            "issued_at": epoch_ms_iso(command["issued_at_ms"]),
            "sent_at": None,
            "updated_at": timestamp,
        }
        app.state.command_history.append(record)
        return record

    def update_command_record(record: dict[str, Any], command_status: str,
                              reason: str | None = None, *, timestamp: str | None = None) -> None:
        record["status"] = command_status
        record["reason"] = reason
        record["updated_at"] = timestamp or now_iso()

    def reject_server_command(command: dict[str, Any], reason: str, *, source: str) -> dict[str, Any]:
        record = new_command_record(command, source=source)
        update_command_record(record, "rejected", reason)
        return command_record_view(record)

    def finish_pending_command(identifier: str, command_status: str,
                               reason: str | None = None) -> None:
        pending = app.state.pending_commands.pop(identifier, None)
        if pending is not None and pending.get("record") is not None:
            update_command_record(pending["record"], command_status, reason)

    def finish_all_pending(command_status: str, reason: str) -> None:
        for identifier in tuple(app.state.pending_commands):
            finish_pending_command(identifier, command_status, reason)

    def cleanup_pending_commands() -> None:
        current = now_epoch_ms()
        expired = [identifier for identifier, item in app.state.pending_commands.items()
                   if item["expires_at_ms"] <= current]
        for identifier in expired:
            finish_pending_command(identifier, "timeout", "ack_timeout")

    def negotiated_device_protocol(websocket: WebSocket) -> int | None:
        if websocket.query_params:
            return None
        authorization = websocket.headers.get("authorization", "")
        scheme, separator, token = authorization.partition(" ")
        device_id = websocket.headers.get("x-stackchan-device-id", "")
        protocol_text = websocket.headers.get("x-stackchan-protocol", "1")
        if (not settings.device_token or separator != " " or scheme.lower() != "bearer" or not token
                or not 1 <= len(device_id) <= 64 or not device_id.isascii()
                or protocol_text not in {str(value) for value in SUPPORTED_PROTOCOL_VERSIONS}):
            return None
        if not (secrets.compare_digest(token, settings.device_token)
                and secrets.compare_digest(device_id, settings.allowed_device_id)):
            return None
        return int(protocol_text)

    async def send_device_json(socket: WebSocket, payload: dict[str, Any]) -> None:
        async with app.state.device_send_lock:
            await socket.send_text(json.dumps(payload, ensure_ascii=False, allow_nan=False,
                                              separators=(",", ":")))

    def generate_touch_reply(reaction: TouchReaction) -> dict[str, Any]:
        store.ensure_conversation(TOUCH_CONVERSATION_ID)
        recent = store.recent_messages(TOUCH_CONVERSATION_ID, RECENT_MESSAGE_LIMIT)
        memories = store.relevant_memories(reaction.message)
        summary = refresh_rolling_summary(store, TOUCH_CONVERSATION_ID)
        prompt = (build_system_prompt(
            load_persona(settings.persona_path), summary, memories,
            motion_locked=settings.motion_locked,
        )
                  + TOUCH_REPLY_SYSTEM_SUFFIX)
        messages = [{"role": item["role"], "content": item["content"]} for item in recent]
        messages.append({"role": "user", "content": reaction.message})
        try:
            return constrain_touch_result(client.complete(prompt, messages))
        except DeepSeekError:
            return touch_fallback_result()

    async def complete_touch_reply(reaction: TouchReaction, socket: WebSocket,
                                   protocol_version: int) -> None:
        try:
            try:
                result = await asyncio.wait_for(
                    asyncio.to_thread(generate_touch_reply, reaction),
                    timeout=TOUCH_REPLY_TIMEOUT_SECONDS,
                )
            except Exception:
                result = touch_fallback_result()
            if app.state.device_socket is not socket or protocol_version < 2:
                app.state.touch_reply_status = "dropped"
                return
            store.add_message(TOUCH_CONVERSATION_ID, "user", reaction.message)
            store.add_message(TOUCH_CONVERSATION_ID, "assistant", result["reply"])
            payloads = [*persona_commands(result["emotion"], "still", 0.0)]
            touch_gesture = TOUCH_GESTURES.get(reaction.kind)
            if not settings.motion_locked and touch_gesture is not None:
                payloads.append({
                    "name": "play_gesture",
                    "gesture": touch_gesture,
                    "intensity": TOUCH_GESTURE_INTENSITY,
                })
            payloads.append(display_text_command(result["reply"]))
            for payload in payloads:
                await broadcast(envelope(payload, protocol_version=protocol_version), source="touch")
            app.state.touch_reply_status = "completed"
        except asyncio.CancelledError:
            app.state.touch_reply_status = "cancelled"
            raise
        except Exception as exc:
            app.state.touch_reply_status = "error"
            app.state.touch_reply_error = type(exc).__name__

    def start_touch_reply(reaction: TouchReaction, socket: WebSocket,
                          protocol_version: int) -> None:
        if any(not task.done() for task in app.state.touch_reply_tasks):
            return
        app.state.touch_reply_status = "generating"
        app.state.touch_reply_error = None
        task = asyncio.create_task(complete_touch_reply(reaction, socket, protocol_version))
        app.state.touch_reply_tasks.add(task)
        task.add_done_callback(app.state.touch_reply_tasks.discard)

    async def broadcast(command: dict[str, Any], *, source: str) -> dict[str, Any]:
        cleanup_pending_commands()
        record = new_command_record(command, source=source)
        socket = app.state.device_socket
        if socket is None:
            update_command_record(record, "disconnected", "device_disconnected")
            return command_record_view(record)
        if command.get("protocol") != app.state.device_state["protocol"]:
            update_command_record(record, "send_failed", "protocol_mismatch")
            return command_record_view(record)
        app.state.pending_commands[command["id"]] = {
            "expires_at_ms": command["expires_at_ms"],
            "record": record,
        }
        try:
            await send_device_json(socket, command)
        except Exception:
            finish_pending_command(command["id"], "send_failed", "send_error")
            if app.state.device_socket is socket:
                app.state.device_socket = None
                app.state.device_state["online"] = False
                app.state.device_state["protocol"] = None
                finish_all_pending("disconnected", "device_disconnected")
            return command_record_view(record)
        record["sent_at"] = now_iso()
        if record["status"] == "pending":
            update_command_record(record, "sent")
        return command_record_view(record)

    @app.websocket("/ws/device")
    async def device_socket(websocket: WebSocket) -> None:
        protocol_version = negotiated_device_protocol(websocket)
        if protocol_version is None:
            await websocket.close(code=1008)
            return
        await websocket.accept()
        previous = app.state.device_socket
        if previous is not None and previous is not websocket:
            finish_all_pending("disconnected", "device_reconnected")
            await previous.close(code=1012)
        app.state.device_socket = websocket
        finish_all_pending("disconnected", "device_reconnected")
        app.state.device_state.update({
            "online": True,
            "last_seen": now_iso(),
            "last_heartbeat": None,
            "last_ack": None,
            "protocol": protocol_version,
        })
        await send_device_json(websocket, {
            "protocol": protocol_version,
            "type": "welcome",
            "server_time_ms": now_epoch_ms(),
            "heartbeat_interval_ms": HEARTBEAT_INTERVAL_MS,
            "heartbeat_miss_limit": HEARTBEAT_MISS_LIMIT,
        })
        rate_limiter = SlidingWindowRateLimiter()
        heartbeat_timeout_ms = HEARTBEAT_INTERVAL_MS * HEARTBEAT_MISS_LIMIT
        heartbeat_deadline_ms = time.monotonic_ns() // 1_000_000 + heartbeat_timeout_ms
        try:
            while True:
                monotonic_now_ms = time.monotonic_ns() // 1_000_000
                remaining_seconds = max(0, heartbeat_deadline_ms - monotonic_now_ms) / 1000
                message = await asyncio.wait_for(
                    websocket.receive(),
                    timeout=remaining_seconds,
                )
                if message["type"] == "websocket.disconnect":
                    raise WebSocketDisconnect(message.get("code", 1000))
                if message.get("bytes") is not None:
                    await websocket.close(code=1003)
                    break
                text = message.get("text")
                try:
                    event = validate_event(decode_text_frame(text), protocol_version=protocol_version)
                except ProtocolError as exc:
                    await websocket.close(code=1009 if exc.code == "frame_too_large" else 1008)
                    break
                if not rate_limiter.allow(time.monotonic_ns() // 1_000_000):
                    await websocket.close(code=1008)
                    break
                app.state.device_state["last_seen"] = now_iso()
                if event["type"] == "battery":
                    app.state.device_state["battery"] = event["percent"]
                if event["type"] == "heartbeat":
                    app.state.device_state["last_heartbeat"] = now_iso()
                    heartbeat_deadline_ms = time.monotonic_ns() // 1_000_000 + heartbeat_timeout_ms
                    await send_device_json(websocket, {
                        "protocol": protocol_version,
                        "type": "heartbeat_ack",
                        "seq": event["seq"],
                        "server_time_ms": now_epoch_ms(),
                    })
                elif event["type"] == "command_ack":
                    pending = app.state.pending_commands.get(event["id"])
                    if pending is not None:
                        received_at = now_iso()
                        record = pending.get("record")
                        app.state.device_state["last_ack"] = {
                            "id": event["id"],
                            "status": event["status"],
                            "received_at": received_at,
                        }
                        if record is not None:
                            app.state.device_state["last_ack"]["name"] = record["name"]
                            app.state.device_state["last_ack"]["reason"] = event.get("reason")
                            update_command_record(record, event["status"], event.get("reason"),
                                                  timestamp=received_at)
                        if event["status"] in {"completed", "rejected", "expired", "busy"}:
                            app.state.pending_commands.pop(event["id"], None)
                    continue
                else:
                    # Device telemetry is deliberately bounded and not promoted to memory.
                    await send_device_json(websocket, {
                        "protocol": protocol_version,
                        "type": "event_ack",
                        "event": event["type"],
                    })
                    if protocol_version >= 2 and event["type"] in {"touch_start", "stroke", "touch_end"}:
                        reaction = app.state.touch_reply_coordinator.observe(
                            event,
                            time.monotonic_ns() // 1_000_000,
                        )
                        if reaction is not None:
                            start_touch_reply(reaction, websocket, protocol_version)
        except asyncio.TimeoutError:
            await websocket.close(code=1001)
        except WebSocketDisconnect:
            pass
        finally:
            if app.state.device_socket is websocket:
                app.state.device_socket = None
                app.state.device_state["online"] = False
                app.state.device_state["protocol"] = None
                finish_all_pending("disconnected", "device_disconnected")
                app.state.touch_reply_coordinator.reset_session()

    return app


app = create_app()
