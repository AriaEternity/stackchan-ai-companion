"""检查 Git 暂存/已跟踪文件的公开范围，不输出可能的秘密值。"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
RULES = {
    'private-location': re.compile(r'(?:D:[/\\]Neko|C:[/\\]Users|loveneko\.cn|8\.163\.23\.36|106\.14\.225\.97)', re.I),
    'credential': re.compile(r'(?:sk-[A-Za-z0-9_-]{16,}|gh[pousr]_[A-Za-z0-9]{20,}|-----BEGIN [A-Z ]*PRIVATE KEY-----)'),
}
REQUIRED = {
    'README.md', 'README.en.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md',
    'server/production_app.py', 'server/config.py', 'server/web/index.html',
    'firmware/main/companion/companion_config.cpp', 'firmware/LICENSE',
    'firmware/tests/CMakeLists.txt', 'examples/persona.md',
    'protocol/device-protocol-v1.md', 'protocol/device-protocol-v2.md',
    'deploy/nginx-stackchan.conf.example', 'docs/images/web-demo.png',
}


def main():
    paths = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z'], encoding='utf-8').split('\0')
    paths = {p for p in paths if p}
    errors = []
    total = 0
    for required in REQUIRED - paths:
        errors.append((required, 'required-file-missing'))
    for name in sorted(paths):
        path = Path(name)
        if any(p in {'.secrets', '.venv', '__pycache__', 'managed_components', '备份', '人设', '记忆', '报告'} for p in path.parts):
            errors.append((name, 'private-or-generated-directory'))
        if path.parts[0] in {'runtime', '.cache', 'backup', 'data'} or path.suffix.lower() in {'.pem', '.key', '.sqlite', '.sqlite3', '.db', '.elf', '.map', '.zip'}:
            errors.append((name, 'runtime-or-binary-artifact'))
        if path.name.startswith('.env') and path.name != '.env.example':
            errors.append((name, 'real-environment-file'))
        if path.suffix == '.bin' and not name.startswith('firmware/main/assets/assets_bin/'):
            errors.append((name, 'flash-or-nvs-binary'))
        content = subprocess.check_output(['git', '-C', str(ROOT), 'show', ':' + name])
        total += len(content)
        if len(content) > 5_000_000:
            errors.append((name, 'oversized-file'))
        try:
            value = content.decode('utf-8')
        except UnicodeDecodeError:
            continue
        for label, pattern in RULES.items():
            if pattern.search(value):
                errors.append((name, label))
    for name, label in errors:
        print(f'BLOCKED {label}: {name}', file=sys.stderr)
    print(f'Public scope: {len(paths)} files, {total} bytes, {len(errors)} findings')
    return bool(errors)


if __name__ == '__main__':
    sys.exit(main())
