"""Bounded rolling conversation context shared by local and production gateways."""

from __future__ import annotations

from typing import Any, Iterable, Protocol


RECENT_MESSAGE_LIMIT = 20
SUMMARY_MESSAGE_LIMIT = 80
SUMMARY_MAX_CHARS = 6000
SUMMARY_ITEM_MAX_CHARS = 500


class ConversationStore(Protocol):
    def get_summary(self, conversation_id: str) -> str: ...

    def set_summary(self, conversation_id: str, content: str) -> None: ...

    def messages_before_recent(
        self,
        conversation_id: str,
        *,
        keep_recent: int,
        limit: int,
    ) -> list[dict[str, Any]]: ...


def _compact(value: Any) -> str:
    text = " ".join(str(value or "").split())
    if len(text) > SUMMARY_ITEM_MAX_CHARS:
        return f"{text[:SUMMARY_ITEM_MAX_CHARS - 1]}…"
    return text


def build_rolling_summary(messages: Iterable[dict[str, Any]]) -> str:
    """Create a deterministic, bounded transcript digest from archived messages."""
    lines = []
    labels = {"user": "用户", "assistant": "neko", "system": "系统"}
    for item in messages:
        content = _compact(item.get("content"))
        if not content:
            continue
        lines.append(f"- {labels.get(str(item.get('role')), '对话')}：{content}")

    selected: list[str] = []
    size = 0
    for line in reversed(lines):
        extra = len(line) + (1 if selected else 0)
        if selected and size + extra > SUMMARY_MAX_CHARS:
            break
        if not selected and len(line) > SUMMARY_MAX_CHARS:
            line = line[:SUMMARY_MAX_CHARS - 1] + "…"
            extra = len(line)
        selected.append(line)
        size += extra
    selected.reverse()
    if len(selected) < len(lines):
        selected.insert(0, "（更早的普通对话已省略；稳定事实应以长期记忆为准）")
    return "\n".join(selected)


def refresh_rolling_summary(store: ConversationStore, conversation_id: str) -> str:
    """Refresh archived context without another model/API call."""
    archived = store.messages_before_recent(
        conversation_id,
        keep_recent=RECENT_MESSAGE_LIMIT,
        limit=SUMMARY_MESSAGE_LIMIT,
    )
    if not archived:
        return store.get_summary(conversation_id)
    summary = build_rolling_summary(archived)
    if summary != store.get_summary(conversation_id):
        store.set_summary(conversation_id, summary)
    return summary
