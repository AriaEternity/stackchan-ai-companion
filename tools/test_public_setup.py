"""公开副本的示例与本地演示边界。"""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from demo import ROOT, DemoClient, demo_settings
from config import Settings
from fastapi.testclient import TestClient
from production_app import create_app


class PublicSetupTests(unittest.TestCase):
    def test_default_config_uses_synthetic_example_and_locked_motion(self):
        with patch.dict('os.environ', {}, clear=True):
            settings = Settings.from_env()
        self.assertEqual(settings.persona_path, ROOT / 'examples/persona.md')
        self.assertTrue(settings.motion_locked)
        self.assertFalse(settings.is_ready_for_production())
        self.assertIn('合成示例', settings.persona_path.read_text(encoding='utf-8'))

    def test_demo_uses_fake_model_and_keeps_device_offline(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = demo_settings(Path(directory) / 'demo.sqlite3')
            self.assertEqual(settings.deepseek_api_key, '')
            app = create_app(settings, client=DemoClient())
            with TestClient(app, base_url='https://testserver') as client:
                self.assertEqual(client.post('/api/login', json={'password': 'wrong'}).status_code, 401)
                response = client.post('/api/login', json={'password': 'local-demo'})
                self.assertIn('Secure', response.headers['set-cookie'])
                self.assertEqual(response.status_code, 200)
                chat = client.post('/api/chat', json={'message': '合成测试消息'})
                self.assertEqual(chat.status_code, 200)
                self.assertIn('本地合成演示', chat.json()['reply'])
                device = client.get('/api/device').json()
                self.assertFalse(device['online'])
                self.assertTrue(device['server_policy']['motion_locked'])
            app.state.store.engine.dispose()


if __name__ == '__main__':
    unittest.main()
