"""Minimal local gateway for stage C; production deployment will add auth/TLS/queues."""

from __future__ import annotations

import json
import os
import sys
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from deepseek import DeepSeekClient, DeepSeekError, safe_fallback  # noqa: E402
from conversation_context import RECENT_MESSAGE_LIMIT, refresh_rolling_summary  # noqa: E402
from memory_store import MemoryStore  # noqa: E402
from persona import build_system_prompt, load_persona  # noqa: E402


class App:
    def __init__(self, *, database_path: str | Path | None = None,
                 persona_path: str | Path | None = None, client: DeepSeekClient | None = None):
        self.store = MemoryStore(database_path or ROOT / "server" / "data" / "stackchan.sqlite3")
        self.persona_path = Path(persona_path or ROOT / "examples" / "persona.md")
        self.client = client or DeepSeekClient()

    def chat(self, payload: dict[str, Any]) -> dict[str, Any]:
        message = payload.get("message")
        if not isinstance(message, str) or not message.strip():
            raise ValueError("message must be a non-empty string")
        if len(message) > 4000:
            raise ValueError("message is too long (maximum 4000 characters)")
        conversation_id = str(payload.get("conversation_id") or "default")[:100]
        self.store.ensure_conversation(conversation_id)
        recent_before = self.store.recent_messages(conversation_id, RECENT_MESSAGE_LIMIT)
        memories = self.store.relevant_memories(message)
        summary = refresh_rolling_summary(self.store, conversation_id)
        prompt = build_system_prompt(load_persona(self.persona_path), summary, memories)
        model_messages = [{"role": item["role"], "content": item["content"]}
                          for item in recent_before if item["role"] in {"user", "assistant"}]
        model_messages.append({"role": "user", "content": message.strip()})
        try:
            result = self.client.complete(prompt, model_messages)
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
        self.store.add_message(conversation_id, "user", message.strip())
        self.store.add_message(conversation_id, "assistant", result["reply"])
        for candidate in result.get("memory_candidates", []):
            self.store.add_memory(candidate, source="model", status="candidate")
        result.update({"conversation_id": conversation_id, "service_status": service_status,
                       "model_output_status": model_output_status, "model_attempts": model_attempts})
        return result


def make_handler(app: App):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, status: int, payload: dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802
            path = urlparse(self.path).path
            if path == "/health":
                self._send(HTTPStatus.OK, {"status": "ok", "deepseek_configured": app.client.configured})
            elif path == "/api/memories":
                self._send(HTTPStatus.OK, {"items": app.store.list_memories()})
            else:
                self._send(HTTPStatus.NOT_FOUND, {"error": "not found"})

        def do_POST(self) -> None:  # noqa: N802
            path = urlparse(self.path).path
            try:
                length = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(length).decode("utf-8"))
                if path == "/api/chat":
                    self._send(HTTPStatus.OK, app.chat(payload))
                elif path == "/api/memories":
                    item = app.store.add_memory(payload.get("content", ""), category=payload.get("category", "fact"),
                                                importance=payload.get("importance", 0.5), source="user")
                    self._send(HTTPStatus.CREATED, item)
                else:
                    self._send(HTTPStatus.NOT_FOUND, {"error": "not found"})
            except (ValueError, TypeError, UnicodeDecodeError, json.JSONDecodeError) as exc:
                self._send(HTTPStatus.BAD_REQUEST, {"error": str(exc)})

        def log_message(self, format: str, *args: Any) -> None:
            return

    return Handler


def main() -> None:
    host = os.getenv("STACKCHAN_HOST", "127.0.0.1")
    port = int(os.getenv("STACKCHAN_PORT", "8787"))
    app = App()
    server = ThreadingHTTPServer((host, port), make_handler(app))
    print(f"StackChan gateway listening on http://{host}:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        app.store.close()
        server.server_close()


if __name__ == "__main__":
    main()
