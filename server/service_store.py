"""SQLAlchemy persistence shared by local tests and production PostgreSQL."""

from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from sqlalchemy import DateTime, Float, ForeignKey, Integer, String, Text, create_engine, func, select
from sqlalchemy.orm import DeclarativeBase, Mapped, Session, mapped_column, sessionmaker


def now() -> datetime:
    return datetime.now(timezone.utc)


class Base(DeclarativeBase):
    pass


class Conversation(Base):
    __tablename__ = "conversations"
    id: Mapped[str] = mapped_column(String(100), primary_key=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=now)


class Message(Base):
    __tablename__ = "messages"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    conversation_id: Mapped[str] = mapped_column(ForeignKey("conversations.id"), index=True)
    role: Mapped[str] = mapped_column(String(16))
    content: Mapped[str] = mapped_column(Text)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=now)


class ConversationSummary(Base):
    __tablename__ = "conversation_summaries"
    conversation_id: Mapped[str] = mapped_column(ForeignKey("conversations.id"), primary_key=True)
    content: Mapped[str] = mapped_column(Text)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=now, onupdate=now)


class MemoryItem(Base):
    __tablename__ = "memory_items"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    category: Mapped[str] = mapped_column(String(64), default="fact")
    content: Mapped[str] = mapped_column(Text)
    importance: Mapped[float] = mapped_column(Float, default=0.5)
    source: Mapped[str] = mapped_column(String(32), default="user")
    status: Mapped[str] = mapped_column(String(16), default="active", index=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=now)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=now, onupdate=now)


def _time(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat(timespec="seconds")


def _message_dict(item: Message) -> dict[str, Any]:
    return {"id": item.id, "role": item.role, "content": item.content, "created_at": _time(item.created_at)}


def _memory_dict(item: MemoryItem) -> dict[str, Any]:
    return {
        "id": item.id,
        "category": item.category,
        "content": item.content,
        "importance": item.importance,
        "source": item.source,
        "status": item.status,
        "created_at": _time(item.created_at),
        "updated_at": _time(item.updated_at),
    }


class ServiceStore:
    def __init__(self, database_url: str):
        if database_url.startswith("sqlite:///"):
            Path(database_url.removeprefix("sqlite:///")) .parent.mkdir(parents=True, exist_ok=True)
            connect_args = {"check_same_thread": False}
        else:
            connect_args = {}
        self.engine = create_engine(database_url, future=True, pool_pre_ping=True, connect_args=connect_args)
        self.sessions = sessionmaker(bind=self.engine, expire_on_commit=False, future=True)

    def create_schema(self) -> None:
        Base.metadata.create_all(self.engine)

    def ensure_conversation(self, conversation_id: str) -> None:
        with self.sessions.begin() as session:
            if not session.get(Conversation, conversation_id):
                session.add(Conversation(id=conversation_id))

    def add_message(self, conversation_id: str, role: str, content: str) -> dict[str, Any]:
        if role not in {"user", "assistant"}:
            raise ValueError("invalid message role")
        self.ensure_conversation(conversation_id)
        with self.sessions.begin() as session:
            item = Message(conversation_id=conversation_id, role=role, content=content)
            session.add(item)
            session.flush()
            return _message_dict(item)

    def recent_messages(self, conversation_id: str, limit: int = 20) -> list[dict[str, Any]]:
        limit = max(1, min(int(limit), 100))
        with self.sessions() as session:
            items = session.scalars(
                select(Message).where(Message.conversation_id == conversation_id).order_by(Message.id.desc()).limit(limit)
            ).all()
            return [_message_dict(item) for item in reversed(items)]

    def messages_after(self, conversation_id: str, after_id: int, limit: int = 100) -> tuple[list[dict[str, Any]], bool]:
        """Return one ascending, conversation-scoped message page after a stable ID cursor."""
        if isinstance(after_id, bool) or int(after_id) < 0:
            raise ValueError("after_id must be a non-negative integer")
        after_id = int(after_id)
        limit = max(1, min(int(limit), 100))
        with self.sessions() as session:
            items = session.scalars(
                select(Message)
                .where(Message.conversation_id == conversation_id, Message.id > after_id)
                .order_by(Message.id.asc())
                .limit(limit + 1)
            ).all()
            has_more = len(items) > limit
            return [_message_dict(item) for item in items[:limit]], has_more

    def messages_before_recent(self, conversation_id: str, *, keep_recent: int = 20,
                               limit: int = 80) -> list[dict[str, Any]]:
        keep_recent = max(0, int(keep_recent))
        limit = max(1, min(int(limit), 500))
        with self.sessions() as session:
            items = session.scalars(
                select(Message)
                .where(Message.conversation_id == conversation_id)
                .order_by(Message.id.desc())
                .offset(keep_recent)
                .limit(limit)
            ).all()
            return [_message_dict(item) for item in reversed(items)]

    def message_count(self, conversation_id: str) -> int:
        with self.sessions() as session:
            return int(session.scalar(
                select(func.count(Message.id)).where(Message.conversation_id == conversation_id)
            ) or 0)

    def get_summary(self, conversation_id: str) -> str:
        with self.sessions() as session:
            item = session.get(ConversationSummary, conversation_id)
            return item.content if item else ""

    def set_summary(self, conversation_id: str, content: str) -> None:
        self.ensure_conversation(conversation_id)
        with self.sessions.begin() as session:
            item = session.get(ConversationSummary, conversation_id)
            if item:
                item.content = content
            else:
                session.add(ConversationSummary(conversation_id=conversation_id, content=content))

    def add_memory(self, content: str, *, category: str = "fact", importance: float = 0.5,
                   source: str = "user", status: str = "active") -> dict[str, Any]:
        if status not in {"active", "candidate"}:
            raise ValueError("memory status must be active or candidate")
        content = content.strip()
        if not content:
            raise ValueError("memory content cannot be empty")
        with self.sessions.begin() as session:
            item = MemoryItem(category=category[:64], content=content, importance=max(0.0, min(float(importance), 1.0)),
                              source=source[:32], status=status)
            session.add(item)
            session.flush()
            return _memory_dict(item)

    def list_memories(self, *, include_deleted: bool = False) -> list[dict[str, Any]]:
        with self.sessions() as session:
            query = select(MemoryItem)
            if not include_deleted:
                query = query.where(MemoryItem.status != "deleted")
            items = session.scalars(query.order_by(MemoryItem.importance.desc(), MemoryItem.id.desc())).all()
            return [_memory_dict(item) for item in items]

    def get_memory(self, memory_id: int) -> dict[str, Any]:
        with self.sessions() as session:
            item = session.get(MemoryItem, memory_id)
            if not item:
                raise KeyError("memory not found")
            return _memory_dict(item)

    def update_memory(self, memory_id: int, *, content: str | None = None, status: str | None = None,
                      importance: float | None = None, category: str | None = None) -> dict[str, Any]:
        with self.sessions.begin() as session:
            item = session.get(MemoryItem, memory_id)
            if not item:
                raise KeyError("memory not found")
            if content is not None:
                if not content.strip():
                    raise ValueError("memory content cannot be empty")
                item.content = content.strip()
            if status is not None:
                if status not in {"active", "candidate", "deleted"}:
                    raise ValueError("invalid memory status")
                item.status = status
            if importance is not None:
                item.importance = max(0.0, min(float(importance), 1.0))
            if category is not None:
                item.category = category[:64]
            item.updated_at = now()
            session.flush()
            return _memory_dict(item)

    def relevant_memories(self, query: str, limit: int = 8) -> list[dict[str, Any]]:
        active = [item for item in self.list_memories() if item["status"] == "active"]
        text = query.strip().lower()
        words = {word for word in text.split() if len(word) > 1}
        tokens = words if len(words) > 1 else {text[index:index + 2] for index in range(max(0, len(text) - 1))}
        if not tokens:
            return active[:limit]
        scored = [(sum(token in item["content"].lower() for token in tokens), item["importance"], item) for item in active]
        scored.sort(key=lambda value: (value[0], value[1], value[2]["id"]), reverse=True)
        return [item for score, _, item in scored if score > 0][:limit]

    def export(self, conversation_id: str = "default") -> dict[str, Any]:
        return {
            "conversation_id": conversation_id,
            "messages": self.recent_messages(conversation_id, 100),
            "summary": self.get_summary(conversation_id),
            "memories": self.list_memories(include_deleted=True),
        }
