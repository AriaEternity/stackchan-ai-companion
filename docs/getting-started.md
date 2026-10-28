# 运行与验证

支持 Python 3.12/3.13。先在本地创建独立虚拟环境：

```bash
python -m venv .venv
# Windows PowerShell
.venv\Scripts\Activate.ps1
# Linux/macOS 使用 source .venv/bin/activate
python -m pip install -r requirements-dev.txt
python tools/demo.py
```

演示使用 `local-demo` 登录，只监听 `127.0.0.1`。回复来自程序内的合成假模型；界面会保留设备离线状态。临时数据库不会读取任何既有运行资料，结束进程后由临时目录回收。演示不是生产服务，不能对公网开放。

## 启动真实网关

生产网关通过进程环境读取配置，`.env` 只是模板，不会自动加载。可通过 Compose 的 `env_file`，或自己的进程管理器注入。

| 变量 | 含义 |
| --- | --- |
| `DATABASE_URL` | SQLAlchemy SQLite 或 `postgresql+psycopg://...` URL |
| `STACKCHAN_ADMIN_PASSWORD` | 自设的访问密码 |
| `STACKCHAN_AUTH_SECRET` | 随机签名密钥 |
| `STACKCHAN_DEVICE_TOKEN` | 独立随机设备凭据，与设备一致 |
| `STACKCHAN_DEVICE_ID` | 自设设备标识，与设备一致 |
| `DEEPSEEK_API_KEY` | 自己的模型凭据 |
| `DEEPSEEK_MODEL` | 自己账户可用的模型名 |
| `DEEPSEEK_BASE_URL` | 可选，默认 `https://api.deepseek.com` |
| `PERSONA_PATH` | 自己的 UTF-8 角色文档；默认仅使用合成示例 |
| `STACKCHAN_MOTION_LOCKED` | 默认 `true`；发布样例始终锁定动作 |

模型名称沿用历史适配器的默认值，不能视为服务商当前可用性的保证。使用时显式设置 `DEEPSEEK_MODEL`。模型不可用时，服务返回明确的降级回复；本项目不在 CI 中发起真实模型请求。

从 `server/` 目录运行：

```bash
python -m uvicorn production_app:app --host 127.0.0.1 --port 8787 --no-access-log
```

网页登录使用 `Secure` Cookie，正式使用需 HTTPS 反向代理。浏览器演示对回环 HTTP 使用仅限演示的 Cookie 适配。正式服务保留原有安全 Cookie 配置。

`server/app.py` 是无认证的本地 SQLite 原型，仅供测试、开发与阅读，不作为公网入口。

## 离线测试

```bash
python -m unittest discover -s server -p "test_*.py" -v
python -m unittest discover -s tools -p "test_*.py" -v
python tools/check_public.py
```

测试均使用临时数据库、合成角色、假模型和假设备。100 轮上下文测试验证编排、持久化与检索结构，不代表真实模型回答质量评测。
