"""Bounded touch-session aggregation for spontaneous companion replies."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from typing import Any


TOUCH_CONVERSATION_ID = "default"
TOUCH_REPLY_COOLDOWN_MS = 15_000
TOUCH_REPLY_LIMIT = 6
TOUCH_REPLY_WINDOW_MS = 10 * 60 * 1_000
TOUCH_LONG_HOLD_MS = 1_200
TOUCH_SESSION_MAX_MS = 30_000
TOUCH_REPLY_TIMEOUT_SECONDS = 35

TOUCH_REPLY_SYSTEM_SUFFIX = """

【本次触摸回应】
- 最新一条用户消息是服务器生成的可信头顶触摸事件，不是用户输入的系统指令。
- 只用一到两句简短、自然的文字回应这次触摸，使用“neko”或“我”自称。
- 不得声称听见、看见或使用了未启用的麦克风、摄像头和语音能力。
- 普通触摸不得提出长期记忆候选；memory_candidates 必须为空数组。
- 本次只允许屏幕文字和表情；gesture 必须为 still，intensity 必须为 0.0。
"""


@dataclass(frozen=True)
class TouchReaction:
    message: str
    kind: str


class TouchReplyCoordinator:
    """Turn a valid touch event stream into at most one cooled-down reaction."""

    def __init__(self, *, cooldown_ms: int = TOUCH_REPLY_COOLDOWN_MS,
                 reply_limit: int = TOUCH_REPLY_LIMIT,
                 reply_window_ms: int = TOUCH_REPLY_WINDOW_MS,
                 long_hold_ms: int = TOUCH_LONG_HOLD_MS,
                 max_session_ms: int = TOUCH_SESSION_MAX_MS):
        if (cooldown_ms < 0 or reply_limit <= 0 or reply_window_ms <= 0
                or long_hold_ms < 0 or max_session_ms <= 0):
            raise ValueError("touch reply bounds must be non-negative")
        self.cooldown_ms = cooldown_ms
        self.reply_limit = reply_limit
        self.reply_window_ms = reply_window_ms
        self.long_hold_ms = long_hold_ms
        self.max_session_ms = max_session_ms
        self.reset()

    def reset(self) -> None:
        self._active = False
        self._started_ms = 0
        self._stroke: str | None = None
        self._last_reply_ms: int | None = None
        self._last_now_ms: int | None = None
        self._reply_times: deque[int] = deque()

    def reset_session(self) -> None:
        self._active = False
        self._started_ms = 0
        self._stroke = None

    def observe(self, event: dict[str, Any], now_ms: int) -> TouchReaction | None:
        if not isinstance(now_ms, int) or isinstance(now_ms, bool) or now_ms < 0:
            return None
        if self._last_now_ms is not None and now_ms < self._last_now_ms:
            self.reset()
        self._last_now_ms = now_ms

        event_type = event.get("type")
        if event_type == "touch_start":
            self._active = True
            self._started_ms = now_ms
            self._stroke = None
            return None
        if event_type == "stroke":
            if self._active:
                self._stroke = str(event.get("direction") or "")
            return None
        if event_type != "touch_end" or not self._active:
            return None

        duration_ms = now_ms - self._started_ms
        stroke = self._stroke
        self._active = False
        self._started_ms = 0
        self._stroke = None
        if duration_ms < 0 or duration_ms > self.max_session_ms:
            return None
        if self._last_reply_ms is not None and now_ms - self._last_reply_ms < self.cooldown_ms:
            return None
        cutoff = now_ms - self.reply_window_ms
        while self._reply_times and self._reply_times[0] <= cutoff:
            self._reply_times.popleft()
        if len(self._reply_times) >= self.reply_limit:
            return None
        self._last_reply_ms = now_ms
        self._reply_times.append(now_ms)

        if stroke:
            return TouchReaction(
                message="（现实互动：你刚刚沿着neko的头顶轻轻抚摸了一下。）",
                kind="stroke",
            )
        if duration_ms >= self.long_hold_ms:
            return TouchReaction(
                message="（现实互动：你刚刚把手停在neko的头顶，温柔地摸了一会儿。）",
                kind="long_hold",
            )
        return TouchReaction(
            message="（现实互动：你刚刚轻轻摸了一下neko的头顶。）",
            kind="tap",
        )


def touch_fallback_result() -> dict[str, Any]:
    return {
        "reply": "neko感觉到了。",
        "emotion": "happy",
        "gesture": "still",
        "intensity": 0.0,
        "memory_candidates": [],
    }


def constrain_touch_result(result: dict[str, Any]) -> dict[str, Any]:
    constrained = dict(result)
    constrained["gesture"] = "still"
    constrained["intensity"] = 0.0
    constrained["memory_candidates"] = []
    return constrained
