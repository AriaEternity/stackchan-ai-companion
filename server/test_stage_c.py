from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from app import App
from deepseek import DeepSeekClient, DeepSeekError, normalize_result, safe_fallback
from memory_store import MemoryStore
from persona import build_system_prompt


class FakeClient(DeepSeekClient):
    def __init__(self):
        super().__init__(api_key="test")
        self.calls = []

    def complete(self, system_prompt, messages):
        self.calls.append((system_prompt, messages))
        return normalize_result(json.dumps({
            "reply": "记住了。", "emotion": "happy", "gesture": "nod",
            "intensity": 2, "memory_candidates": ["用户喜欢海边", ""],
        }))


class SequencedClient(DeepSeekClient):
    def __init__(self, responses):
        super().__init__(api_key="test")
        self.responses = list(responses)
        self.temperatures = []

    def _request_content(self, system_prompt, messages, *, temperature):
        self.temperatures.append(temperature)
        return self.responses.pop(0)


class InvalidClient(DeepSeekClient):
    def __init__(self):
        super().__init__(api_key="test")

    def complete(self, system_prompt, messages):
        return safe_fallback(reason="invalid_json")


class StageCTests(unittest.TestCase):
    def test_store_persists_messages_summary_and_memories(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "db.sqlite3"
            store = MemoryStore(path)
            store.add_message("c1", "user", "你好")
            store.set_summary("c1", "用户刚开始使用机器人")
            store.add_memory("用户喜欢蓝色", importance=0.9)
            store.close()
            restored = MemoryStore(path)
            self.assertEqual(restored.recent_messages("c1")[0]["content"], "你好")
            self.assertEqual(restored.get_summary("c1"), "用户刚开始使用机器人")
            self.assertEqual(restored.relevant_memories("蓝色")[0]["content"], "用户喜欢蓝色")
            restored.close()

    def test_prompt_contains_three_context_layers(self):
        prompt = build_system_prompt("名字：小满", "用户今天很累", [{"category": "preference", "content": "喜欢茶"}])
        self.assertIn("名字：小满", prompt)
        self.assertIn("用户今天很累", prompt)
        self.assertIn("喜欢茶", prompt)
        self.assertIn("memory_candidates", prompt)
        self.assertIn("服务器动作安全锁当前开启", prompt)
        self.assertIn("不是设备遥测", prompt)
        self.assertIn("gesture 设为 still", prompt)

        unlocked = build_system_prompt("名字：小满", "", [], motion_locked=False)
        self.assertIn("服务器动作安全锁当前关闭", unlocked)
        self.assertIn("不代表设备已经执行", unlocked)

    def test_chinese_query_can_match_memory_without_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            store = MemoryStore(Path(directory) / "db.sqlite3")
            store.add_memory("用户喜欢海边", importance=0.8)
            self.assertEqual(store.relevant_memories("我喜欢海边")[0]["content"], "用户喜欢海边")
            store.close()

    def test_model_output_is_whitelisted_and_candidates_are_pending(self):
        self.assertEqual(normalize_result("not json")["gesture"], "still")
        self.assertEqual(normalize_result("not json")["_normalization_status"], "fallback")
        with tempfile.TemporaryDirectory() as directory:
            fake = FakeClient()
            app = App(database_path=Path(directory) / "db.sqlite3", client=fake)
            result = app.chat({"conversation_id": "test", "message": "我喜欢海边"})
            self.assertEqual(result["gesture"], "nod")
            self.assertEqual(result["intensity"], 1.0)
            self.assertEqual(app.store.list_memories()[0]["status"], "candidate")
            self.assertEqual(len(app.store.recent_messages("test")), 2)
            app.store.close()

    def test_invalid_structured_output_retries_once(self):
        client = SequencedClient(["", json.dumps({
            "reply": "第二次成功。", "emotion": "happy", "gesture": "nod",
            "intensity": 0.5, "memory_candidates": [],
        })])
        result = client.complete("persona", [{"role": "user", "content": "你好"}])
        self.assertEqual(result["reply"], "第二次成功。")
        self.assertEqual(result["_normalization_status"], "recovered")
        self.assertEqual(result["_model_attempts"], 2)
        self.assertEqual(client.temperatures, [0.7, 0.2])

    def test_repeated_invalid_structured_output_is_an_error(self):
        client = SequencedClient(["", "{}", "[]"])
        with self.assertRaises(DeepSeekError) as context:
            client.complete("persona", [{"role": "user", "content": "你好"}])
        self.assertEqual(context.exception.attempts, 3)

    def test_third_structured_output_attempt_can_recover(self):
        client = SequencedClient(["", "{}", json.dumps({
            "reply": "第三次成功。", "emotion": "neutral", "gesture": "still",
            "intensity": 0.2, "memory_candidates": [],
        })])
        result = client.complete("persona", [{"role": "user", "content": "你好"}])
        self.assertEqual(result["reply"], "第三次成功。")
        self.assertEqual(result["_normalization_status"], "recovered")
        self.assertEqual(result["_model_attempts"], 3)
        self.assertEqual(client.temperatures, [0.7, 0.2, 0.0])

    def test_gateway_marks_normalization_fallback_as_degraded(self):
        with tempfile.TemporaryDirectory() as directory:
            app = App(database_path=Path(directory) / "db.sqlite3", client=InvalidClient())
            result = app.chat({"message": "你好"})
            self.assertEqual(result["service_status"], "degraded")
            self.assertEqual(result["model_output_status"], "fallback")
            app.store.close()

    def test_missing_api_key_degrades_without_writing_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            app = App(database_path=Path(directory) / "db.sqlite3", client=DeepSeekClient(api_key=""))
            result = app.chat({"message": "你好"})
            self.assertEqual(result["service_status"], "degraded")
            self.assertEqual(result["gesture"], "still")
            app.store.close()

    def test_rolling_summary_carries_messages_outside_recent_window(self):
        with tempfile.TemporaryDirectory() as directory:
            fake = FakeClient()
            app = App(database_path=Path(directory) / "db.sqlite3", client=fake)
            for turn in range(1, 13):
                app.chat({"conversation_id": "long", "message": f"第{turn}轮内容"})
            system_prompt, model_messages = fake.calls[-1]
            self.assertIn("第1轮内容", system_prompt)
            self.assertNotIn("第1轮内容", [item["content"] for item in model_messages])
            self.assertIn("仅作为历史对话资料", system_prompt)
            self.assertIn("摄像头、麦克风和语音功能当前未启用", system_prompt)
            app.store.close()


if __name__ == "__main__":
    unittest.main()
