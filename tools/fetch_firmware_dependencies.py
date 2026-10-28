"""下载、校验并安装锁定的固件依赖；不覆盖未知目录。"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'
MARKER = '.stackchan-verified.json'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_digest(root):
    entries = [(p.relative_to(root).as_posix(), digest(p)) for p in root.rglob('*') if p.is_file() and p.name != MARKER]
    return hashlib.sha256(json.dumps(sorted(entries)).encode()).hexdigest()


def main():
    dependencies = json.loads((ROOT / 'tools/firmware-dependencies.json').read_text(encoding='utf-8'))
    cache = FW / '.dependency-cache'
    cache.mkdir(exist_ok=True)
    for item in dependencies:
        target = (FW / item['target']).resolve()
        if not target.is_relative_to(FW.resolve()):
            raise ValueError('Dependency target escaped firmware directory')
        if target.exists():
            marker = target / MARKER
            state = json.loads(marker.read_text()) if marker.is_file() else {}
            if state.get('archive') != item['sha256'].lower() or state.get('tree') != tree_digest(target):
                raise ValueError('Existing dependency is unknown or changed: ' + item['target'])
            print('Verified existing ' + item['target'])
            continue
        archive = cache / (item['sha256'].lower() + '.zip')
        if not archive.exists():
            url = 'https://codeload.github.com/' + item['repo'] + '/zip/' + item['ref']
            with urllib.request.urlopen(url, timeout=120) as response:
                with archive.open('xb') as output:
                    shutil.copyfileobj(response, output)
        if digest(archive) != item['sha256'].lower():
            raise ValueError('Dependency archive checksum mismatch: ' + item['repo'])
        with tempfile.TemporaryDirectory(prefix='stackchan-dependency-') as directory:
            stage = Path(directory).resolve()
            with zipfile.ZipFile(archive) as bundle:
                for info in bundle.infolist():
                    dest = (stage / info.filename).resolve()
                    if not dest.is_relative_to(stage) or (info.external_attr >> 16) & 0o170000 == 0o120000:
                        raise ValueError('Unsafe archive member')
                bundle.extractall(stage)
            roots = list(stage.iterdir())
            if len(roots) != 1 or not roots[0].is_dir():
                raise ValueError('Unexpected dependency archive layout')
            source = roots[0]
            if item.get('patch'):
                patch = FW / item['patch']
                if digest(patch) != item['patch_sha256']:
                    raise ValueError('Patch checksum mismatch')
                env = {**os.environ, 'GIT_CEILING_DIRECTORIES': str(stage)}
                subprocess.run(['git', 'apply', '--check', str(patch)], cwd=source, env=env, check=True)
                subprocess.run(['git', 'apply', str(patch)], cwd=source, env=env, check=True)
            state = {'archive': item['sha256'].lower(), 'tree': tree_digest(source)}
            (source / MARKER).write_text(json.dumps(state) + '\n')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source, target)
        print('Installed ' + item['target'])


if __name__ == '__main__':
    main()
