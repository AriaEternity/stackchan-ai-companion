"""SQLite persistence for conversations, summaries, and memory items."""

from __future__ import annotations

import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class MemoryStore:
    def __init__(self, database_path: str | Path):
        self.database_path = str(database_path)
        Path(self.database_path).parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(self.database_path, check_same_thread=False)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.execute("PRAGMA foreign_keys=ON")
        self._create_schema()

    def _create_schema(self) -> None:
        self.connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS conversations (
                id TEXT PRIMARY KEY,
                created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                conversation_id TEXT NOT NULL REFERENCES conversations(id),
                role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant')),
                content TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_messages_conversation
                ON messages(conversation_id, id);
            CREATE TABLE IF NOT EXISTS conversation_summaries (
                conversation_id TEXT PRIMARY KEY REFERENCES conversations(id),
                content TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS memory_items (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                category TEXT NOT NULL DEFAULT 'fact',
                content TEXT NOT NULL,
                importance REAL NOT NULL DEFAULT 0.5,
                source TEXT NOT NULL DEFAULT 'user',
                status TEXT NOT NULL DEFAULT 'active'
                    CHECK(status IN ('active', 'candidate', 'deleted')),
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_memory_status ON memory_items(status);
            """
        )
        self.connection.commit()

    def close(self) -> None:
        self.connection.close()

    def ensure_conversation(self, conversation_id: str) -> str:
        self.connection.execute(
            "INSERT OR IGNORE INTO conversations(id, created_at) VALUES (?, ?)",
            (conversation_id, _now()),
        )
        self.connection.commit()
        return conversation_id

    def add_message(self, conversation_id: str, role: str, content: str) -> dict[str, Any]:
        if role not in {"system", "user", "assistant"}:
            raise ValueError("invalid message role")
        self.ensure_conversation(conversation_id)
        created_at = _now()
        cursor = self.connection.execute(
            "INSERT INTO messages(conversation_id, role, content, created_at) VALUES (?, ?, ?, ?)",
            (conversation_id, role, content, created_at),
        )
        self.connection.commit()
        return {"id": cursor.lastrowid, "role": role, "content": content, "created_at": created_at}

    def recent_messages(self, conversation_id: str, limit: int = 20) -> list[dict[str, Any]]:
        limit = max(1, min(int(limit), 100))
        rows = self.connection.execute(
            "SELECT id, role, content, created_at FROM messages "
            "WHERE conversation_id = ? ORDER BY id DESC LIMIT ?",
            (conversation_id, limit),
        ).fetchall()
        return [dict(row) for row in reversed(rows)]

    def messages_before_recent(self, conversation_id: str, *, keep_recent: int = 20,
                               limit: int = 80) -> list[dict[str, Any]]:
        keep_recent = max(0, int(keep_recent))
        limit = max(1, min(int(limit), 500))
        rows = self.connection.execute(
            "SELECT id, role, content, created_at FROM messages "
            "WHERE conversation_id = ? ORDER BY id DESC LIMIT ? OFFSET ?",
            (conversation_id, limit, keep_recent),
        ).fetchall()
        return [dict(row) for row in reversed(rows)]

    def message_count(self, conversation_id: str) -> int:
        row = self.connection.execute(
            "SELECT COUNT(*) AS count FROM messages WHERE conversation_id = ?",
            (conversation_id,),
        ).fetchone()
        return int(row["count"])

    def get_summary(self, conversation_id: str) -> str:
        row = self.connection.execute(
            "SELECT content FROM conversation_summaries WHERE conversation_id = ?",
            (conversation_id,),
        ).fetchone()
        return row["content"] if row else ""

    def set_summary(self, conversation_id: str, content: str) -> None:
        self.ensure_conversation(conversation_id)
        self.connection.execute(
            "INSERT INTO conversation_summaries(conversation_id, content, updated_at) "
            "VALUES (?, ?, ?) ON CONFLICT(conversation_id) DO UPDATE SET content=excluded.content, updated_at=excluded.updated_at",
            (conversation_id, content, _now()),
        )
        self.connection.commit()

    def add_memory(
        self,
        content: str,
        *,
        category: str = "fact",
        importance: float = 0.5,
        source: str = "user",
        status: str = "active",
    ) -> dict[str, Any]:
        if status not in {"active", "candidate"}:
            raise ValueError("memory status must be active or candidate")
        content = content.strip()
        if not content:
            raise ValueError("memory content cannot be empty")
        importance = max(0.0, min(float(importance), 1.0))
        now = _now()
        cursor = self.connection.execute(
            "INSERT INTO memory_items(category, content, importance, source, status, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (category, content, importance, source, status, now, now),
        )
        self.connection.commit()
        return self.get_memory(cursor.lastrowid)

    def get_memory(self, memory_id: int) -> dict[str, Any]:
        row = self.connection.execute(
            "SELECT id, category, content, importance, source, status, created_at, updated_at "
            "FROM memory_items WHERE id = ?",
            (memory_id,),
        ).fetchone()
        if not row:
            raise KeyError("memory not found")
        return dict(row)

    def list_memories(self, *, include_deleted: bool = False) -> list[dict[str, Any]]:
        query = "SELECT id, category, content, importance, source, status, created_at, updated_at FROM memory_items"
        params: tuple[Any, ...] = ()
        if not include_deleted:
            query += " WHERE status != 'deleted'"
        query += " ORDER BY importance DESC, id DESC"
        return [dict(row) for row in self.connection.execute(query, params).fetchall()]

    def relevant_memories(self, query: str, limit: int = 8) -> list[dict[str, Any]]:
        """Small local lexical matcher; replace with embeddings when the server is deployed."""
        memories = [item for item in self.list_memories() if item["status"] == "active"]
        normalized_query = query.strip().lower()
        tokens = {token for token in normalized_query.split() if len(token.strip()) > 1}
        # Chinese text often has no whitespace. Bigrams provide a deterministic
        # local fallback until the production service adds embeddings/search.
        if len(normalized_query) > 1 and (len(tokens) == 1 and tokens == {normalized_query} or not tokens):
            tokens = {normalized_query[index:index + 2] for index in range(len(normalized_query) - 1)}
        if not tokens:
            return memories[:limit]
        scored = []
        for item in memories:
            haystack = item["content"].lower()
            score = sum(token in haystack for token in tokens)
            scored.append((score, item["importance"], item))
        scored.sort(key=lambda value: (value[0], value[1], value[2]["id"]), reverse=True)
        return [item for score, _, item in scored if score > 0][:limit]

    def update_memory(self, memory_id: int, *, content: str | None = None, status: str | None = None,
                      importance: float | None = None, category: str | None = None) -> dict[str, Any]:
        self.get_memory(memory_id)
        fields = []
        values: list[Any] = []
        if content is not None:
            if not content.strip():
                raise ValueError("memory content cannot be empty")
            fields.append("content = ?")
            values.append(content.strip())
        if status is not None:
            if status not in {"active", "candidate", "deleted"}:
                raise ValueError("invalid memory status")
            fields.append("status = ?")
            values.append(status)
        if importance is not None:
            fields.append("importance = ?")
            values.append(max(0.0, min(float(importance), 1.0)))
        if category is not None:
            fields.append("category = ?")
            values.append(category)
        if fields:
            fields.append("updated_at = ?")
            values.append(_now())
            values.append(memory_id)
            self.connection.execute(f"UPDATE memory_items SET {', '.join(fields)} WHERE id = ?", values)
            self.connection.commit()
        return self.get_memory(memory_id)
