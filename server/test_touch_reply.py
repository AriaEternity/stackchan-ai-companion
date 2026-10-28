from __future__ import annotations

import unittest

from touch_reply import TouchReplyCoordinator, constrain_touch_result, touch_fallback_result


class TouchReplyTests(unittest.TestCase):
    def test_tap_long_hold_and_stroke_are_aggregated_on_touch_end(self):
        coordinator = TouchReplyCoordinator(cooldown_ms=0)

        self.assertIsNone(coordinator.observe({"type": "touch_start"}, 100))
        tap = coordinator.observe({"type": "touch_end"}, 300)
        self.assertEqual(tap.kind, "tap")

        coordinator.observe({"type": "touch_start"}, 1_000)
        long_hold = coordinator.observe({"type": "touch_end"}, 2_500)
        self.assertEqual(long_hold.kind, "long_hold")

        coordinator.observe({"type": "touch_start"}, 3_000)
        coordinator.observe({"type": "stroke", "direction": "left_to_right"}, 3_200)
        stroke = coordinator.observe({"type": "touch_end"}, 3_500)
        self.assertEqual(stroke.kind, "stroke")
        self.assertNotIn("left", stroke.message)

    def test_cooldown_suppresses_model_replies_but_accepts_new_sessions(self):
        coordinator = TouchReplyCoordinator(cooldown_ms=15_000)
        coordinator.observe({"type": "touch_start"}, 0)
        self.assertIsNotNone(coordinator.observe({"type": "touch_end"}, 100))
        coordinator.observe({"type": "touch_start"}, 1_000)
        self.assertIsNone(coordinator.observe({"type": "touch_end"}, 1_100))
        coordinator.observe({"type": "touch_start"}, 16_000)
        self.assertIsNotNone(coordinator.observe({"type": "touch_end"}, 16_100))

    def test_invalid_sequences_stale_sessions_and_time_regression_are_safe(self):
        coordinator = TouchReplyCoordinator(cooldown_ms=0, max_session_ms=1_000)
        self.assertIsNone(coordinator.observe({"type": "touch_end"}, 100))
        self.assertIsNone(coordinator.observe({"type": "stroke"}, 200))
        coordinator.observe({"type": "touch_start"}, 300)
        self.assertIsNone(coordinator.observe({"type": "touch_end"}, 1_500))
        coordinator.observe({"type": "touch_start"}, 2_000)
        self.assertIsNone(coordinator.observe({"type": "touch_end"}, 1_900))
        self.assertIsNone(coordinator.observe({"type": "touch_start"}, -1))

    def test_rolling_budget_bounds_model_calls_and_recovers(self):
        coordinator = TouchReplyCoordinator(
            cooldown_ms=0,
            reply_limit=2,
            reply_window_ms=1_000,
        )
        for start, expected in ((0, True), (100, True), (200, False), (1_100, True)):
            coordinator.observe({"type": "touch_start"}, start)
            reaction = coordinator.observe({"type": "touch_end"}, start + 10)
            self.assertEqual(reaction is not None, expected)

    def test_touch_results_cannot_create_memory_or_motion(self):
        constrained = constrain_touch_result({
            "reply": "收到。",
            "emotion": "happy",
            "gesture": "nuzzle",
            "intensity": 1.0,
            "memory_candidates": ["保存摸头"],
        })
        self.assertEqual(constrained["gesture"], "still")
        self.assertEqual(constrained["intensity"], 0.0)
        self.assertEqual(constrained["memory_candidates"], [])
        self.assertEqual(touch_fallback_result()["reply"], "neko感觉到了。")


if __name__ == "__main__":
    unittest.main()
