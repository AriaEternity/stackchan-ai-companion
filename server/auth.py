"""Small signed session token implementation for the single-owner service."""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import time


class AuthenticationError(ValueError):
    pass


def _b64encode(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def _b64decode(value: str) -> bytes:
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


class Authenticator:
    def __init__(self, password: str, secret: str, ttl_seconds: int):
        self.password = password
        self.secret = secret.encode("utf-8")
        self.ttl_seconds = ttl_seconds

    def authenticate(self, password: str) -> str:
        if not self.password or not hmac.compare_digest(password.encode("utf-8"), self.password.encode("utf-8")):
            raise AuthenticationError("invalid credentials")
        if not self.secret:
            raise AuthenticationError("service authentication is not configured")
        now = int(time.time())
        payload = {"sub": "owner", "iat": now, "exp": now + self.ttl_seconds}
        encoded = _b64encode(json.dumps(payload, separators=(",", ":")).encode("utf-8"))
        signature = _b64encode(hmac.new(self.secret, encoded.encode("ascii"), hashlib.sha256).digest())
        return f"{encoded}.{signature}"

    def verify(self, token: str) -> dict:
        try:
            encoded, signature = token.split(".", 1)
            expected = _b64encode(hmac.new(self.secret, encoded.encode("ascii"), hashlib.sha256).digest())
            if not self.secret or not hmac.compare_digest(signature, expected):
                raise AuthenticationError("invalid token")
            payload = json.loads(_b64decode(encoded))
            if payload.get("sub") != "owner" or int(payload.get("exp", 0)) < int(time.time()):
                raise AuthenticationError("expired token")
            return payload
        except (ValueError, TypeError, json.JSONDecodeError, UnicodeDecodeError) as exc:
            if isinstance(exc, AuthenticationError):
                raise
            raise AuthenticationError("invalid token") from exc
