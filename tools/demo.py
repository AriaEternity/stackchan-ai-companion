"""只在回环地址运行的合成演示；不连接设备或真实模型。"""
from __future__ import annotations

import argparse
from pathlib import Path
import secrets
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server'))

from config import Settings
from deepseek import DeepSeekClient, normalize_result
from production_app import create_app


class DemoClient(DeepSeekClient):
    def __init__(self):
        super().__init__(api_key='synthetic-demo-not-a-key')

    def complete(self, system_prompt, messages):
        return normalize_result({
            'reply': '你好，我是小满。这是本地合成演示：消息会显示在网页里，设备当前离线。你也可以打开右上角的记忆面板，添加一条示例偏好。',
            'emotion': 'happy', 'gesture': 'still', 'intensity': 0.0,
            'memory_candidates': [],
        })


def demo_settings(database: Path) -> Settings:
    return Settings(
        database_url=f'sqlite:///{database.as_posix()}', redis_url='',
        admin_password='local-demo', auth_secret=secrets.token_urlsafe(48),
        device_token=secrets.token_urlsafe(32), allowed_device_id='demo-device-001',
        deepseek_api_key='', deepseek_model='synthetic-demo',
        persona_path=ROOT / 'examples/persona.md', motion_locked=True,
    )


def main():
    import uvicorn
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8787)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='stackchan-public-demo-') as directory:
        app = create_app(demo_settings(Path(directory) / 'demo.sqlite3'), client=DemoClient())
        # 仅此回环演示允许 HTTP Cookie；正式网关继续使用 Secure Cookie。
        @app.middleware('http')
        async def local_cookie(request, call_next):
            response = await call_next(request)
            if request.url.path == '/api/login':
                headers = response.headers.getlist('set-cookie')
                if headers:
                    del response.headers['set-cookie']
                    for value in headers:
                        response.headers.append('set-cookie', value.replace('; Secure', ''))
            return response
        print(f'Synthetic demo: http://127.0.0.1:{args.port} | password: local-demo')
        uvicorn.run(app, host='127.0.0.1', port=args.port, access_log=False)


if __name__ == '__main__':
    main()
