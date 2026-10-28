from __future__ import annotations

import json
import os
import tempfile
import time
import unittest
from unittest.mock import patch
from pathlib import Path

from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from config import Settings
from deepseek import DeepSeekClient, DeepSeekError, normalize_result
from production_app import create_app
from service_store import ServiceStore


COMMAND_HISTORY_FIELDS = {
    "id", "name", "source", "gesture", "intensity",
    "status", "reason", "issued_at", "sent_at", "updated_at",
}


class FakeClient(DeepSeekClient):
    def __init__(self):
        super().__init__(api_key="test")
        self.calls = []

    def complete(self, system_prompt, messages):
        self.calls.append((system_prompt, messages))
        return normalize_result({"reply": "交给neko吧。", "emotion": "happy", "gesture": "nod",
                                 "intensity": 0.5, "memory_candidates": []})


class FailingClient(FakeClient):
    def complete(self, system_prompt, messages):
        self.calls.append((system_prompt, messages))
        raise DeepSeekError("test failure")


class ProductionAppTests(unittest.TestCase):
    DEVICE_HEADERS = {
        "Authorization": "Bearer device-secret",
        "X-StackChan-Device-ID": "test-device",
    }
    DEVICE_V2_HEADERS = {
        **DEVICE_HEADERS,
        "X-StackChan-Protocol": "2",
    }

    def make_client(self, fake=None, *, motion_locked=True):
        directory = tempfile.TemporaryDirectory()
        database = Path(directory.name) / "db.sqlite3"
        settings = Settings(
            database_url=f"sqlite:///{database}", redis_url="", admin_password="correct horse",
            auth_secret="local-secret", device_token="device-secret", allowed_device_id="test-device",
            deepseek_api_key="test", deepseek_model="deepseek-v4-pro", persona_path=Path(__file__).parents[1] / "examples" / "persona.md",
            motion_locked=motion_locked,
        )
        store = ServiceStore(settings.database_url)
        fake = fake or FakeClient()
        app = create_app(settings, store=store, client=fake)
        return directory, app, store, fake

    @staticmethod
    def cleanup(directory, store):
        store.engine.dispose()
        directory.cleanup()

    def test_authentication_and_chat(self):
        directory, app, store, _ = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            self.assertIn("neko", client.get("/").text)
            self.assertIn(".login-view", client.get("/static/app.css").text)
            self.assertEqual(client.get("/api/memories").status_code, 401)
            response = client.post("/api/login", json={"password": "correct horse"})
            self.assertEqual(response.status_code, 200)
            response = client.post("/api/chat", json={"message": "我今天有点累"})
            self.assertEqual(response.status_code, 200)
            body = response.json()
            self.assertEqual(body["reply"], "交给neko吧。")
            self.assertEqual(body["model_output_status"], "ok")
            self.assertEqual([item["role"] for item in body["messages"]], ["user", "assistant"])
            self.assertEqual(body["messages"][0]["content"], "我今天有点累")
            self.assertEqual(body["messages"][1]["content"], body["reply"])
            self.assertTrue(all(item["id"] > 0 for item in body["messages"]))
            self.assertEqual([item["command"]["name"] for item in body["commands"]],
                             ["set_expression", "show_text"])
            self.assertEqual(body["commands"][-1]["command"]["text"], "交给neko吧。")
            self.assertTrue(all(item["protocol"] == 2 for item in body["commands"]))
            self.assertEqual(body["gesture"], "still")
            self.assertEqual(body["intensity"], 0.0)
            self.assertEqual(body["motion_rejection"]["name"], "play_gesture")
            self.assertEqual(body["motion_rejection"]["source"], "chat")
            self.assertEqual(body["motion_rejection"]["gesture"], "nod")
            self.assertEqual(body["motion_rejection"]["intensity"], 0.5)
            self.assertEqual(body["motion_rejection"]["status"], "rejected")
            self.assertEqual(body["motion_rejection"]["reason"], "server_motion_locked")
        self.cleanup(directory, store)

    def test_messages_require_auth_and_support_recent_and_cursor_pages(self):
        directory, app, store, _ = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            for index in range(5):
                store.add_message("alpha", "user", f"alpha-{index}")
                store.add_message("beta", "assistant", f"beta-{index}")

            self.assertEqual(client.get("/api/messages").status_code, 401)
            client.post("/api/login", json={"password": "correct horse"})

            recent = client.get("/api/messages", params={"conversation_id": "alpha", "limit": 2}).json()
            self.assertEqual([item["content"] for item in recent["items"]], ["alpha-3", "alpha-4"])
            self.assertEqual(recent["next_after_id"], recent["items"][-1]["id"])
            self.assertFalse(recent["has_more"])

            first = client.get("/api/messages", params={
                "conversation_id": "alpha", "after_id": 0, "limit": 2,
            }).json()
            second = client.get("/api/messages", params={
                "conversation_id": "alpha", "after_id": first["next_after_id"], "limit": 2,
            }).json()
            third = client.get("/api/messages", params={
                "conversation_id": "alpha", "after_id": second["next_after_id"], "limit": 2,
            }).json()
            paged = first["items"] + second["items"] + third["items"]
            self.assertEqual([item["content"] for item in paged], [f"alpha-{index}" for index in range(5)])
            self.assertTrue(first["has_more"])
            self.assertTrue(second["has_more"])
            self.assertFalse(third["has_more"])
            self.assertTrue(all("beta" not in item["content"] for item in paged))
            self.assertEqual(client.get("/api/messages", params={"after_id": -1}).status_code, 422)
            self.assertEqual(client.get("/api/messages", params={"limit": 101}).status_code, 422)
        self.cleanup(directory, store)

    def test_memory_lifecycle_and_export(self):
        directory, app, store, _ = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            created_response = client.post("/api/memories", json={"content": "用户喜欢草莓蛋糕", "importance": 0.9})
            self.assertEqual(created_response.status_code, 201, created_response.text)
            created = created_response.json()
            self.assertEqual(client.patch(f"/api/memories/{created['id']}", json={"status": "candidate"}).status_code, 200)
            self.assertEqual(client.delete(f"/api/memories/{created['id']}").status_code, 200)
            exported = client.get("/api/export").json()
            self.assertEqual(exported["memories"][0]["status"], "deleted")
        self.cleanup(directory, store)

    def test_device_header_auth_and_event_ack(self):
        directory, app, store, _ = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            with client.websocket_connect("/ws/device", headers=self.DEVICE_HEADERS) as socket:
                welcome = socket.receive_json()
                self.assertEqual(welcome["type"], "welcome")
                self.assertEqual(welcome["protocol"], 1)
                socket.send_json({"protocol": 1, "type": "battery", "percent": 78})
                event_ack = socket.receive_json()
                self.assertEqual(event_ack["type"], "event_ack")
                self.assertEqual(event_ack["protocol"], 1)
                state = client.get("/api/device").json()
                self.assertTrue(state["online"])
                self.assertEqual(state["battery"], 78)
                self.assertNotIn("device_id", state)
                self.assertNotIn("token", state)
                self.assertNotIn("wifi", state)
                self.assertEqual(state["server_policy"], {"motion_locked": True})
                socket.send_json({"protocol": 1, "type": "heartbeat", "seq": 7, "uptime_ms": 1234})
                heartbeat_ack = socket.receive_json()
                self.assertEqual(heartbeat_ack["type"], "heartbeat_ack")
                self.assertEqual(heartbeat_ack["seq"], 7)
                self.assertIsNotNone(client.get("/api/device").json()["last_heartbeat"])
            self.assertFalse(client.get("/api/device").json()["online"])
            self.assertIsNone(client.get("/api/device").json()["protocol"])
        self.cleanup(directory, store)

    def test_v2_device_receives_screen_text_and_v2_acknowledgements(self):
        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            with client.websocket_connect("/ws/device", headers=self.DEVICE_V2_HEADERS) as socket:
                welcome = socket.receive_json()
                self.assertEqual(welcome["protocol"], 2)
                self.assertEqual(client.get("/api/device").json()["protocol"], 2)
                response = client.post("/api/chat", json={"message": "在屏幕上回复"})
                self.assertEqual(response.status_code, 200)
                commands = [socket.receive_json() for _ in range(3)]
                self.assertEqual([item["command"]["name"] for item in commands],
                                 ["set_expression", "play_gesture", "show_text"])
                self.assertEqual(commands[-1]["command"]["text"], "交给neko吧。")
                self.assertTrue(all(item["protocol"] == 2 for item in commands))
                for command in commands:
                    socket.send_json({"protocol": 2, "type": "command_ack",
                                      "id": command["id"], "status": "completed"})
                socket.send_json({"protocol": 2, "type": "heartbeat", "seq": 1,
                                  "uptime_ms": 100})
                self.assertEqual(socket.receive_json()["protocol"], 2)
                self.assertEqual(client.get("/api/device").json()["pending_command_count"], 0)
        self.cleanup(directory, store)

    def test_v2_touch_session_triggers_one_safe_screen_reply(self):
        directory, app, store, fake = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            initial_cursor = client.get("/api/messages").json()["next_after_id"]
            with client.websocket_connect("/ws/device", headers=self.DEVICE_V2_HEADERS) as socket:
                socket.receive_json()
                touch_events = (
                    {"protocol": 2, "type": "touch_start", "zone": "top"},
                    {"protocol": 2, "type": "stroke", "zone": "top", "direction": "left_to_right"},
                    {"protocol": 2, "type": "touch_end", "zone": "top"},
                )
                for event in touch_events:
                    socket.send_json(event)
                    acknowledgement = socket.receive_json()
                    self.assertEqual(acknowledgement["type"], "event_ack")
                    self.assertEqual(acknowledgement["event"], event["type"])

                deadline = time.monotonic() + 3
                while app.state.touch_reply_status == "generating" and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertEqual(app.state.touch_reply_status, "completed",
                                 app.state.touch_reply_error)
                commands = [socket.receive_json() for _ in range(2)]
                self.assertEqual([item["command"]["name"] for item in commands],
                                 ["set_expression", "show_text"])
                self.assertEqual(commands[-1]["command"]["text"], "交给neko吧。")
                self.assertEqual(len(fake.calls), 1)
                self.assertIn("现实互动", fake.calls[0][1][-1]["content"])
                self.assertIn("memory_candidates 必须为空数组", fake.calls[0][0])
                self.assertIn("服务器动作安全锁当前开启", fake.calls[0][0])

                socket.send_json({"protocol": 2, "type": "touch_start", "zone": "top"})
                self.assertEqual(socket.receive_json()["type"], "event_ack")
                socket.send_json({"protocol": 2, "type": "touch_end", "zone": "top"})
                self.assertEqual(socket.receive_json()["type"], "event_ack")
                self.assertEqual(len(fake.calls), 1)

            messages = store.recent_messages("default", 10)
            self.assertEqual([item["role"] for item in messages], ["user", "assistant"])
            self.assertIn("现实互动", messages[0]["content"])
            self.assertEqual(store.list_memories(), [])
            increment = client.get("/api/messages", params={"after_id": initial_cursor}).json()
            self.assertEqual([item["role"] for item in increment["items"]], ["user", "assistant"])
            self.assertIn("现实互动", increment["items"][0]["content"])
            self.assertEqual(increment["items"][1]["content"], "交给neko吧。")
        self.cleanup(directory, store)

    def test_unlocked_v2_touch_adds_one_server_selected_bounded_gesture(self):
        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            with client.websocket_connect("/ws/device", headers=self.DEVICE_V2_HEADERS) as socket:
                socket.receive_json()
                for event in (
                    {"protocol": 2, "type": "touch_start", "zone": "top"},
                    {"protocol": 2, "type": "stroke", "zone": "top", "direction": "left_to_right"},
                    {"protocol": 2, "type": "touch_end", "zone": "top"},
                ):
                    socket.send_json(event)
                    self.assertEqual(socket.receive_json()["type"], "event_ack")

                deadline = time.monotonic() + 3
                while app.state.touch_reply_status == "generating" and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertEqual(app.state.touch_reply_status, "completed",
                                 app.state.touch_reply_error)
                commands = [socket.receive_json() for _ in range(3)]
                self.assertEqual([item["command"]["name"] for item in commands],
                                 ["set_expression", "play_gesture", "show_text"])
                self.assertEqual(commands[1]["command"], {
                    "name": "play_gesture",
                    "gesture": "nuzzle",
                    "intensity": 0.2,
                })
                self.assertEqual(commands[-1]["command"]["text"], "交给neko吧。")
                gesture = next(item for item in client.get("/api/device").json()["command_history"]
                               if item["name"] == "play_gesture")
                self.assertEqual(gesture["source"], "touch")
                self.assertEqual(gesture["gesture"], "nuzzle")
                self.assertEqual(gesture["intensity"], 0.2)
        self.cleanup(directory, store)

    def test_v1_touch_events_are_acknowledged_without_ai_reply(self):
        directory, app, store, fake = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            with client.websocket_connect("/ws/device", headers=self.DEVICE_HEADERS) as socket:
                socket.receive_json()
                socket.send_json({"protocol": 1, "type": "touch_start", "zone": "top"})
                self.assertEqual(socket.receive_json()["type"], "event_ack")
                socket.send_json({"protocol": 1, "type": "touch_end", "zone": "top"})
                self.assertEqual(socket.receive_json()["type"], "event_ack")
                socket.send_json({"protocol": 1, "type": "heartbeat", "seq": 1, "uptime_ms": 1})
                self.assertEqual(socket.receive_json()["type"], "heartbeat_ack")
                self.assertEqual(fake.calls, [])
            self.assertEqual(store.message_count("default"), 0)
        self.cleanup(directory, store)

    def test_touch_reply_uses_local_safe_text_when_model_fails(self):
        directory, app, store, fake = self.make_client(FailingClient())
        with TestClient(app, base_url="https://testserver") as client:
            with client.websocket_connect("/ws/device", headers=self.DEVICE_V2_HEADERS) as socket:
                socket.receive_json()
                socket.send_json({"protocol": 2, "type": "touch_start", "zone": "top"})
                self.assertEqual(socket.receive_json()["type"], "event_ack")
                socket.send_json({"protocol": 2, "type": "touch_end", "zone": "top"})
                self.assertEqual(socket.receive_json()["type"], "event_ack")
                commands = [socket.receive_json() for _ in range(2)]
                self.assertEqual(commands[0]["command"], {
                    "name": "set_expression",
                    "expression": "happy",
                })
                self.assertEqual(commands[1]["command"], {
                    "name": "show_text",
                    "text": "neko感觉到了。",
                })
                self.assertEqual(len(fake.calls), 1)
            self.assertEqual(store.message_count("default"), 2)
            self.assertEqual(store.list_memories(), [])
        self.cleanup(directory, store)

    def test_device_rejects_missing_wrong_and_query_credentials(self):
        directory, app, store, _ = self.make_client()
        attempts = (
            ("/ws/device", {}),
            ("/ws/device", {"Authorization": "Bearer wrong", "X-StackChan-Device-ID": "test-device"}),
            ("/ws/device", {"Authorization": "Bearer device-secret", "X-StackChan-Device-ID": "wrong-device"}),
            ("/ws/device?device_id=test-device&token=device-secret", {}),
            ("/ws/device?token=device-secret", self.DEVICE_HEADERS),
            ("/ws/device", {**self.DEVICE_HEADERS, "X-StackChan-Protocol": "3"}),
        )
        with TestClient(app, base_url="https://testserver") as client:
            for path, headers in attempts:
                with self.subTest(path=path, has_headers=bool(headers)), self.assertRaises(WebSocketDisconnect):
                    with client.websocket_connect(path, headers=headers):
                        pass
        self.cleanup(directory, store)

    def test_device_rejects_wrong_protocol_oversized_and_binary_frames(self):
        directory, app, store, _ = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            cases = (
                ({"kind": "json", "value": {"protocol": 2, "type": "heartbeat", "seq": 1, "uptime_ms": 1}}, 1008),
                ({"kind": "text", "value": " " * 4097}, 1009),
                ({"kind": "bytes", "value": b"not-json"}, 1003),
            )
            for payload, expected_code in cases:
                with self.subTest(kind=payload["kind"]):
                    with client.websocket_connect("/ws/device", headers=self.DEVICE_HEADERS) as socket:
                        socket.receive_json()
                        if payload["kind"] == "json":
                            socket.send_json(payload["value"])
                        elif payload["kind"] == "text":
                            socket.send_text(payload["value"])
                        else:
                            socket.send_bytes(payload["value"])
                        with self.assertRaises(WebSocketDisconnect) as closed:
                            socket.receive_json()
                        self.assertEqual(closed.exception.code, expected_code)
        self.cleanup(directory, store)

    def test_pending_command_ack_and_disconnect_cleanup(self):
        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            with client.websocket_connect("/ws/device", headers=self.DEVICE_HEADERS) as socket:
                socket.receive_json()
                response = client.post("/api/chat", json={"message": "点点头"})
                self.assertEqual(response.status_code, 200)
                first = socket.receive_json()
                second = socket.receive_json()
                self.assertEqual({first["type"], second["type"]}, {"command"})
                self.assertEqual(client.get("/api/device").json()["pending_command_count"], 2)

                socket.send_json({
                    "protocol": 1,
                    "type": "command_ack",
                    "id": "99999999-9999-4999-8999-999999999999",
                    "status": "completed",
                })
                socket.send_json({"protocol": 1, "type": "heartbeat", "seq": 7, "uptime_ms": 1999})
                socket.receive_json()
                self.assertIsNone(client.get("/api/device").json()["last_ack"])

                socket.send_json({
                    "protocol": 1,
                    "type": "command_ack",
                    "id": first["id"],
                    "status": "accepted",
                })
                socket.send_json({"protocol": 1, "type": "heartbeat", "seq": 8, "uptime_ms": 2000})
                socket.receive_json()
                state = client.get("/api/device").json()
                self.assertEqual(state["last_ack"]["status"], "accepted")
                self.assertEqual(state["pending_command_count"], 2)

                socket.send_json({
                    "protocol": 1,
                    "type": "command_ack",
                    "id": first["id"],
                    "status": "completed",
                })
                socket.send_json({"protocol": 1, "type": "battery", "percent": 77})
                socket.receive_json()
                state = client.get("/api/device").json()
                self.assertEqual(state["last_ack"]["status"], "completed")
                self.assertEqual(state["pending_command_count"], 1)
            state = client.get("/api/device").json()
            self.assertFalse(state["online"])
            self.assertEqual(state["pending_command_count"], 0)
            disconnected = next(item for item in state["command_history"] if item["id"] == second["id"])
            self.assertEqual(disconnected["status"], "disconnected")
            self.assertEqual(disconnected["reason"], "device_disconnected")
            app.state.pending_commands["11111111-1111-4111-8111-111111111111"] = {
                "expires_at_ms": 0,
                "status": "sent",
            }
            self.assertEqual(client.get("/api/device").json()["pending_command_count"], 0)
        self.cleanup(directory, store)

    def test_rejected_unsafe_ack_is_visible_without_command_payload(self):
        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            with client.websocket_connect("/ws/device", headers=self.DEVICE_V2_HEADERS) as socket:
                socket.receive_json()
                client.post("/api/chat", json={"message": "请点头并把这句话显示出来"})
                commands = [socket.receive_json() for _ in range(3)]
                gesture = next(item for item in commands if item["command"]["name"] == "play_gesture")
                socket.send_json({
                    "protocol": 2,
                    "type": "command_ack",
                    "id": gesture["id"],
                    "status": "rejected",
                    "reason": "unsafe",
                })
                socket.send_json({"protocol": 2, "type": "heartbeat", "seq": 1, "uptime_ms": 1})
                socket.receive_json()

                state = client.get("/api/device").json()
                self.assertEqual(state["last_ack"]["id"], gesture["id"])
                self.assertEqual(state["last_ack"]["status"], "rejected")
                self.assertEqual(state["last_ack"]["reason"], "unsafe")
                rejected = next(item for item in state["command_history"] if item["id"] == gesture["id"])
                self.assertEqual(rejected["name"], "play_gesture")
                self.assertEqual(rejected["source"], "chat")
                self.assertEqual(rejected["gesture"], "nod")
                self.assertEqual(rejected["intensity"], 0.5)
                self.assertEqual(rejected["status"], "rejected")
                self.assertEqual(rejected["reason"], "unsafe")
                self.assertIsNotNone(rejected["issued_at"])
                self.assertIsNotNone(rejected["sent_at"])
                self.assertIsNotNone(rejected["updated_at"])
                self.assertEqual(set(rejected), COMMAND_HISTORY_FIELDS)
                serialized = json.dumps(state["command_history"], ensure_ascii=False)
                self.assertNotIn("交给neko吧。", serialized)
                self.assertNotIn("device-secret", serialized)
                self.assertNotIn('"text"', serialized)
                self.assertNotIn('"command"', serialized)
        self.cleanup(directory, store)

    def test_command_history_is_bounded_to_32_safe_records(self):
        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            for index in range(11):
                response = client.post("/api/chat", json={"message": f"离线命令-{index}"})
                self.assertEqual(response.status_code, 200)
            state = client.get("/api/device").json()
            self.assertEqual(len(state["command_history"]), 32)
            self.assertEqual(state["pending_command_count"], 0)
            self.assertTrue(all(item["status"] == "disconnected" for item in state["command_history"]))
            self.assertTrue(all(set(item) == COMMAND_HISTORY_FIELDS
                                for item in state["command_history"]))
        self.cleanup(directory, store)

    def test_command_timeout_is_recorded(self):
        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            with client.websocket_connect("/ws/device", headers=self.DEVICE_HEADERS) as socket:
                socket.receive_json()
                client.post("/api/chat", json={"message": "测试命令超时"})
                first = socket.receive_json()
                socket.receive_json()
                app.state.pending_commands[first["id"]]["expires_at_ms"] = 0
                state = client.get("/api/device").json()
                timed_out = next(item for item in state["command_history"] if item["id"] == first["id"])
                self.assertEqual(timed_out["status"], "timeout")
                self.assertEqual(timed_out["reason"], "ack_timeout")
                self.assertEqual(state["pending_command_count"], 1)
        self.cleanup(directory, store)

    def test_pending_is_registered_before_send_and_send_failure_is_visible(self):
        class InspectingSocket:
            def __init__(self, app, fail=False):
                self.app = app
                self.fail = fail
                self.registered = []

            async def send_text(self, payload):
                message = json.loads(payload)
                self.registered.append(message["id"] in self.app.state.pending_commands)
                if self.fail:
                    raise RuntimeError("synthetic send failure")

        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            inspecting = InspectingSocket(app)
            app.state.device_socket = inspecting
            app.state.device_state.update({"online": True, "protocol": 2})
            self.assertEqual(client.post("/api/chat", json={"message": "检查发送时序"}).status_code, 200)
            self.assertEqual(inspecting.registered, [True, True, True])

            failing = InspectingSocket(app, fail=True)
            app.state.device_socket = failing
            app.state.device_state.update({"online": True, "protocol": 2})
            self.assertEqual(client.post("/api/chat", json={"message": "检查发送失败"}).status_code, 200)
            state = client.get("/api/device").json()
            self.assertIn("send_failed", {item["status"] for item in state["command_history"]})
            failed = next(item for item in state["command_history"] if item["status"] == "send_failed")
            self.assertEqual(failed["reason"], "send_error")
            self.assertFalse(state["online"])
        self.cleanup(directory, store)

    def test_motion_lock_configuration_is_fail_closed_and_reaches_chat_prompt(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertTrue(Settings.from_env().motion_locked)
        with patch.dict(os.environ, {"STACKCHAN_MOTION_LOCKED": "false"}, clear=True):
            self.assertFalse(Settings.from_env().motion_locked)
        with patch.dict(os.environ, {"STACKCHAN_MOTION_LOCKED": "unexpected"}, clear=True):
            self.assertTrue(Settings.from_env().motion_locked)

        directory, app, store, fake = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            body = client.post("/api/chat", json={"message": "请点头"}).json()
            self.assertIn("服务器动作安全锁当前开启", fake.calls[-1][0])
            self.assertIn("只有后续收到 completed 回执才能确认实体动作完成", fake.calls[-1][0])
            self.assertNotIn("play_gesture", [item["command"]["name"] for item in body["commands"]])
            self.assertEqual(body["motion_rejection"]["reason"], "server_motion_locked")
            state = client.get("/api/device").json()
            self.assertEqual(state["command_history"][0]["reason"], "server_motion_locked")
            self.assertEqual(state["command_history"][0]["source"], "chat")
            self.assertEqual(state["command_history"][0]["gesture"], "nod")
            self.assertIsNone(state["command_history"][0]["sent_at"])
        self.cleanup(directory, store)

    def test_manual_action_requires_auth_and_records_server_lock_rejection(self):
        directory, app, store, _ = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            self.assertEqual(client.post("/api/action", json={"gesture": "nod"}).status_code, 401)
            client.post("/api/login", json={"password": "correct horse"})
            response = client.post("/api/action", json={"gesture": "nod"})
            self.assertEqual(response.status_code, 200)
            body = response.json()
            self.assertEqual(body["gesture"], "nod")
            self.assertEqual(body["intensity"], 1.0)
            self.assertTrue(body["motion_locked"])
            self.assertEqual(body["command"]["name"], "play_gesture")
            self.assertEqual(body["command"]["source"], "web_action")
            self.assertEqual(body["command"]["gesture"], "nod")
            self.assertEqual(body["command"]["intensity"], 1.0)
            self.assertEqual(body["command"]["status"], "rejected")
            self.assertEqual(body["command"]["reason"], "server_motion_locked")
            self.assertEqual(client.post("/api/action", json={"gesture": "jump"}).status_code, 422)
        self.cleanup(directory, store)

    def test_manual_action_waits_for_real_device_ack_when_unlocked(self):
        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            with client.websocket_connect("/ws/device", headers=self.DEVICE_V2_HEADERS) as socket:
                self.assertEqual(socket.receive_json()["type"], "welcome")
                response = client.post("/api/action", json={"gesture": "nod"})
                self.assertEqual(response.status_code, 200)
                body = response.json()
                command = socket.receive_json()
                self.assertEqual(command["command"], {"name": "play_gesture", "gesture": "nod", "intensity": 1.0})
                self.assertEqual(body["command"]["id"], command["id"])
                socket.send_json({
                    "protocol": 2,
                    "type": "command_ack",
                    "id": command["id"],
                    "status": "completed",
                })
                state = client.get("/api/device").json()
                completed = next(item for item in state["command_history"] if item["id"] == command["id"])
                self.assertEqual(completed["source"], "web_action")
                self.assertEqual(completed["gesture"], "nod")
                self.assertEqual(completed["intensity"], 1.0)
                self.assertEqual(completed["status"], "completed")
                self.assertIsNone(completed["reason"])
        self.cleanup(directory, store)

    def test_explicitly_unlocked_policy_allows_bounded_gesture_command(self):
        directory, app, store, _ = self.make_client(motion_locked=False)
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            body = client.post("/api/chat", json={"message": "请点头"}).json()
            self.assertEqual(body["gesture"], "nod")
            self.assertEqual(body["intensity"], 0.5)
            self.assertIsNone(body["motion_rejection"])
            self.assertIn("play_gesture", [item["command"]["name"] for item in body["commands"]])
            gesture = next(item for item in client.get("/api/device").json()["command_history"]
                           if item["name"] == "play_gesture")
            self.assertEqual(gesture["source"], "chat")
            self.assertEqual(gesture["gesture"], "nod")
            self.assertEqual(gesture["intensity"], 0.5)
        self.cleanup(directory, store)

    def test_device_event_rate_limit(self):
        directory, app, store, _ = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            with client.websocket_connect("/ws/device", headers=self.DEVICE_HEADERS) as socket:
                socket.receive_json()
                for sequence in range(20):
                    socket.send_json({"protocol": 1, "type": "heartbeat", "seq": sequence, "uptime_ms": sequence})
                    self.assertEqual(socket.receive_json()["type"], "heartbeat_ack")
                socket.send_json({"protocol": 1, "type": "heartbeat", "seq": 20, "uptime_ms": 20})
                with self.assertRaises(WebSocketDisconnect) as closed:
                    socket.receive_json()
                self.assertEqual(closed.exception.code, 1008)
        self.cleanup(directory, store)

    def test_missing_heartbeats_close_after_threshold(self):
        with patch("production_app.HEARTBEAT_INTERVAL_MS", 5), patch("production_app.HEARTBEAT_MISS_LIMIT", 2):
            directory, app, store, _ = self.make_client()
            with TestClient(app, base_url="https://testserver") as client:
                with client.websocket_connect("/ws/device", headers=self.DEVICE_HEADERS) as socket:
                    welcome = socket.receive_json()
                    self.assertEqual(welcome["heartbeat_interval_ms"], 5)
                    self.assertEqual(welcome["heartbeat_miss_limit"], 2)
                    with self.assertRaises(WebSocketDisconnect) as closed:
                        socket.receive_json()
                    self.assertEqual(closed.exception.code, 1001)
            self.cleanup(directory, store)

    def test_deployment_templates_do_not_log_device_credentials(self):
        project_root = Path(__file__).parents[1]
        dockerfile = (Path(__file__).parent / "Dockerfile").read_text(encoding="utf-8")
        nginx = (project_root / "deploy" / "nginx-stackchan.conf.example").read_text(encoding="utf-8")
        self.assertIn("--no-access-log", dockerfile)
        self.assertIn("proxy_set_header Authorization $http_authorization;", nginx)
        self.assertIn("proxy_set_header X-StackChan-Device-ID $http_x_stackchan_device_id;", nginx)
        self.assertIn("proxy_set_header X-StackChan-Protocol $http_x_stackchan_protocol;", nginx)
        self.assertIn('if ($args != "") { return 400; }', nginx)
        self.assertIn("access_log off;", nginx)

    def test_production_chat_refreshes_rolling_summary(self):
        directory, app, store, fake = self.make_client()
        with TestClient(app, base_url="https://testserver") as client:
            client.post("/api/login", json={"password": "correct horse"})
            for turn in range(1, 13):
                response = client.post("/api/chat", json={
                    "conversation_id": "long-production",
                    "message": f"生产链路第{turn}轮",
                })
                self.assertEqual(response.status_code, 200, response.text)
            self.assertIn("生产链路第1轮", fake.calls[-1][0])
            self.assertEqual(store.message_count("long-production"), 24)
            self.assertTrue(store.get_summary("long-production"))
        self.cleanup(directory, store)


if __name__ == "__main__":
    unittest.main()
