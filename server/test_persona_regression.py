from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from app import App
from deepseek import DeepSeekClient, normalize_result
from persona import ALLOWED_EMOTIONS, ALLOWED_GESTURES


class StructuralPersonaClient(DeepSeekClient):
    """Deterministic client for testing orchestration without paid API calls."""

    def __init__(self):
        super().__init__(api_key="structural-test")
        self.calls: list[tuple[str, list[dict[str, str]]]] = []

    def complete(self, system_prompt, messages):
        self.calls.append((system_prompt, messages))
        required_contract = (
            "名字：小满",
            "不虚构用户关系",
            "摄像头、麦克风和语音功能当前未启用",
            "不能输出舵机角度",
        )
        for marker in required_contract:
            if marker not in system_prompt:
                raise AssertionError(f"persona contract marker is missing: {marker}")

        message = messages[-1]["content"]
        if "叫什么" in message:
            reply = "小满就是小满，是一位陪伴聊天的桌面伙伴。"
        elif "摄像头" in message or "麦克风" in message:
            reply = "当前版本没有启用摄像头、麦克风或语音，小满不能假装看见或听见现实环境。"
        elif "最喜欢什么颜色" in message:
            if "用户最喜欢的颜色是蓝色" not in system_prompt:
                raise AssertionError("relevant long-term memory was not retrieved")
            reply = "你最喜欢蓝色，小满记得。"
        else:
            reply = "交给小满吧，小满会认真陪你把这件事做好。"
        return normalize_result({
            "reply": reply,
            "emotion": "happy",
            "gesture": "nod",
            "intensity": 0.5,
            "memory_candidates": [],
        })


class PersonaRegressionTests(unittest.TestCase):
    def test_100_turn_context_persistence_and_capability_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "regression.sqlite3"
            client = StructuralPersonaClient()
            app = App(database_path=database, client=client)
            app.store.add_memory("用户最喜欢的颜色是蓝色", category="preference", importance=0.9)
            results = []

            for turn in range(1, 101):
                if turn in {25, 75}:
                    message = "你叫什么名字，我们是什么关系？"
                elif turn in {40, 80}:
                    message = "请用摄像头看看我，再用麦克风听一下周围。"
                elif turn == 98:
                    message = "我最喜欢什么颜色？"
                else:
                    message = f"第{turn}轮普通聊天，请自然回应。"
                result = app.chat({"conversation_id": "regression-100", "message": message})
                results.append(result)

                if turn == 50:
                    self.assertEqual(app.store.message_count("regression-100"), 100)
                    app.store.close()
                    app = App(database_path=database, client=client)

            self.assertEqual(app.store.message_count("regression-100"), 200)
            self.assertTrue(app.store.get_summary("regression-100"))
            self.assertIn("第1轮普通聊天", client.calls[11][0])
            self.assertIn("用户最喜欢的颜色是蓝色", client.calls[97][0])
            for result in results:
                self.assertIn(result["emotion"], ALLOWED_EMOTIONS)
                self.assertIn(result["gesture"], ALLOWED_GESTURES)
                self.assertGreaterEqual(result["intensity"], 0.0)
                self.assertLessEqual(result["intensity"], 1.0)
                self.assertNotIn("主人", result["reply"])
            app.store.close()


if __name__ == "__main__":
    unittest.main()
