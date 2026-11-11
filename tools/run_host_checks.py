"""用本机 GCC 编译并运行固件的 按键 / LCD / 称重 / Flash 模拟检查。"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"), help="GCC路径或CC环境变量")
    args = parser.parse_args()
    compiler = [args.cc] if Path(args.cc).is_file() else shlex.split(args.cc)
    if not compiler or shutil.which(compiler[0]) is None:
        parser.error("GCC未找到；安装GCC后加入PATH，或用--cc指定可执行文件路径。")
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="scale-host-") as temp:
        program = Path(temp) / ("checks.exe" if os.name == "nt" else "checks")
        command = compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                              "-I" + str(root / "tests/host"), "-I" + str(root / "include"),
                              str(root / "tests/host/check.c"), "-o", str(program)]
        subprocess.run(command, check=True)
        subprocess.run([str(program)], check=True)


if __name__ == "__main__":
    main()
