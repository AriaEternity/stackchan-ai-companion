from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from provision_companion import DEVICE_ID_PATTERN, TOKEN_PATTERN, build_frame, load_ascii_value, wipe


class ProvisionCompanionTests(unittest.TestCase):
    def test_load_build_and_wipe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            device_file = root / "device.txt"
            token_file = root / "token.txt"
            device_file.write_text("stackchan-k151-test\n", encoding="ascii")
            token_file.write_text("test_token.1234\n", encoding="ascii")
            device_id = load_ascii_value(device_file, DEVICE_ID_PATTERN, "device ID")
            token = load_ascii_value(token_file, TOKEN_PATTERN, "device token")
            frame = build_frame(device_id, token)
            self.assertEqual(frame, b"SC-PROVISION-1 stackchan-k151-test test_token.1234\n")
            wipe(frame)
            self.assertEqual(set(frame), {0})

    def test_invalid_values_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            value_file = Path(directory) / "value.txt"
            value_file.write_text("contains space", encoding="ascii")
            with self.assertRaises(ValueError):
                load_ascii_value(value_file, DEVICE_ID_PATTERN, "device ID")
            value_file.write_text("bad:token", encoding="ascii")
            with self.assertRaises(ValueError):
                load_ascii_value(value_file, TOKEN_PATTERN, "device token")


if __name__ == "__main__":
    unittest.main()
