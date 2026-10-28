"""兼容上游入口，转交到带归档哈希校验的下载器。"""
from pathlib import Path
import runpy

if __name__ == "__main__":
    runpy.run_path(str(Path(__file__).resolve().parents[1] / "tools/fetch_firmware_dependencies.py"), run_name="__main__")
