"""Runtime configuration. Secrets are supplied by environment variables only."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


def _motion_locked_from_env() -> bool:
    value = os.getenv("STACKCHAN_MOTION_LOCKED")
    if value is None:
        return True
    return value.strip().lower() not in {"0", "false", "no", "off"}


@dataclass(frozen=True)
class Settings:
    database_url: str
    redis_url: str
    admin_password: str
    auth_secret: str
    device_token: str
    allowed_device_id: str
    deepseek_api_key: str
    deepseek_model: str
    persona_path: Path
    token_ttl_seconds: int = 60 * 60 * 24 * 14
    motion_locked: bool = True

    @classmethod
    def from_env(cls) -> "Settings":
        # The same module runs from the repository and from /app in Docker.
        root = Path(__file__).resolve().parent
        default_persona = root / "persona_owner.md" if (root / "persona_owner.md").exists() else root.parent / "examples" / "persona.md"
        return cls(
            database_url=os.getenv("DATABASE_URL", f"sqlite:///{root / 'data' / 'stackchan.sqlite3'}"),
            redis_url=os.getenv("REDIS_URL", ""),
            admin_password=os.getenv("STACKCHAN_ADMIN_PASSWORD", ""),
            auth_secret=os.getenv("STACKCHAN_AUTH_SECRET", ""),
            device_token=os.getenv("STACKCHAN_DEVICE_TOKEN", ""),
            allowed_device_id=os.getenv("STACKCHAN_DEVICE_ID", "demo-device-001"),
            deepseek_api_key=os.getenv("DEEPSEEK_API_KEY", ""),
            deepseek_model=os.getenv("DEEPSEEK_MODEL", "deepseek-v4-pro"),
            persona_path=Path(os.getenv("PERSONA_PATH", str(default_persona))),
            motion_locked=_motion_locked_from_env(),
        )

    def is_ready_for_production(self) -> bool:
        return bool(self.admin_password and self.auth_secret and self.device_token and self.deepseek_api_key)
