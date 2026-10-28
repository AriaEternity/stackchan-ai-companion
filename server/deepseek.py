"""Optional DeepSeek adapter with strict response normalization."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Any

from persona import ALLOWED_EMOTIONS, ALLOWED_GESTURES


class DeepSeekError(RuntimeError):
    def __init__(self, message: str, *, attempts: int = 0):
        super().__init__(message)
        self.attempts = attempts


class DeepSeekClient:
    def __init__(self, api_key: str | None = None, *, base_url: str | None = None,
                 model: str | None = None, timeout: float = 30):
        self.api_key = api_key or os.getenv("DEEPSEEK_API_KEY", "")
        self.base_url = (base_url or os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com")).rstrip("/")
        self.model = model or os.getenv("DEEPSEEK_MODEL", "deepseek-v4-pro")
        self.timeout = timeout

    @property
    def configured(self) -> bool:
        return bool(self.api_key)

    def complete(self, system_prompt: str, messages: list[dict[str, str]]) -> dict[str, Any]:
        if not self.configured:
            raise DeepSeekError("DEEPSEEK_API_KEY is not configured")
        last_reason = "invalid_structured_output"
        temperatures = (0.7, 0.2, 0.0)
        for attempt, temperature in enumerate(temperatures):
            try:
                content = self._request_content(
                    system_prompt if attempt == 0 else (
                        system_prompt
                        + "\n\n【格式重试】上一次响应无法通过结构校验。只输出完整 JSON 对象，reply 必须是非空字符串。"
                    ),
                    messages,
                    temperature=temperature,
                )
            except DeepSeekError as exc:
                if not exc.attempts:
                    exc.attempts = attempt + 1
                raise
            result = normalize_result(content)
            if result.get("_normalization_status") == "ok":
                result["_normalization_status"] = "ok" if attempt == 0 else "recovered"
                result["_model_attempts"] = attempt + 1
                result.pop("_fallback_reason", None)
                return result
            last_reason = str(result.get("_fallback_reason") or last_reason)
        raise DeepSeekError(
            f"DeepSeek returned invalid structured output after retries ({last_reason})",
            attempts=len(temperatures),
        )

    def _request_content(self, system_prompt: str, messages: list[dict[str, str]],
                         *, temperature: float) -> Any:
        payload = {
            "model": self.model,
            "messages": [{"role": "system", "content": system_prompt}, *messages],
            "temperature": temperature,
            "response_format": {"type": "json_object"},
        }
        request = urllib.request.Request(
            f"{self.base_url}/chat/completions",
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                body = json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, json.JSONDecodeError) as exc:
            raise DeepSeekError(f"DeepSeek request failed: {exc}") from exc
        try:
            return body["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as exc:
            raise DeepSeekError("DeepSeek response has no assistant content") from exc


def normalize_result(raw: Any) -> dict[str, Any]:
    """Validate model output before it can reach UI or device command code."""
    parsed: Any = raw
    if isinstance(raw, str):
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            return safe_fallback(raw, reason="invalid_json")
    if not isinstance(parsed, dict):
        return safe_fallback("", reason="not_object")
    reply = parsed.get("reply")
    if not isinstance(reply, str) or not reply.strip():
        return safe_fallback("", reason="missing_reply")
    emotion = parsed.get("emotion") if parsed.get("emotion") in ALLOWED_EMOTIONS else "neutral"
    gesture = parsed.get("gesture") if parsed.get("gesture") in ALLOWED_GESTURES else "still"
    try:
        intensity = max(0.0, min(float(parsed.get("intensity", 0.5)), 1.0))
    except (TypeError, ValueError):
        intensity = 0.5
    candidates = parsed.get("memory_candidates", [])
    if not isinstance(candidates, list):
        candidates = []
    candidates = [value.strip() for value in candidates if isinstance(value, str) and value.strip()][:5]
    return {"reply": reply.strip(), "emotion": emotion, "gesture": gesture,
            "intensity": intensity, "memory_candidates": candidates,
            "_normalization_status": "ok"}


def safe_fallback(detail: str = "", *, reason: str = "service_error") -> dict[str, Any]:
    return {
        "reply": detail.strip() or "我现在暂时无法连接到对话服务，请稍后再试。",
        "emotion": "neutral",
        "gesture": "still",
        "intensity": 0.0,
        "memory_candidates": [],
        "_normalization_status": "fallback",
        "_fallback_reason": reason,
    }
