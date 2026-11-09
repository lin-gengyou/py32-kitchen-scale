"""采集烧录器信息并读取 CPUID/RAM/Flash；使用项目的 PlatformIO pyOCD。"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "packs/Puya.PY32F0xx_DFP.1.2.20.pack"
READS = ("e000ed00", "20000000", "08000000")


def locate_runtime(override):
    """查询 PIO 包管理器，兼容 C:/.platformio 和自定义 Core 目录。"""
    candidates = [override] if override else [sys.executable]
    if not override:
        for name in ("pio", "platformio"):
            executable = shutil.which(name)
            if executable:
                parent = Path(executable).parent
                candidates += [str(parent / "python.exe"), str(parent / "python")]
        home = Path.home() / ".platformio/penv"
        candidates += [str(home / "Scripts/python.exe"), str(home / "bin/python")]
    query = (
        "from platformio.package.manager.tool import ToolPackageManager;"
        "from pathlib import Path;import json,sys;"
        "p=ToolPackageManager().get_package('tool-pyocd');"
        "assert p is not None,'Run pio build to install tool-pyocd';"
        "print(json.dumps([sys.executable,str(Path(p.path)/'pyocd.py')]))"
    )
    for candidate in dict.fromkeys(filter(None, candidates)):
        if not Path(candidate).is_file():
            continue
        result = subprocess.run([candidate, "-c", query], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=20)
        if result.returncode == 0:
            try:
                runtime = json.loads(result.stdout.strip().splitlines()[-1])
                if Path(runtime[1]).is_file():
                    return runtime
            except (ValueError, IndexError, TypeError):
                continue
    raise RuntimeError("请先运行 pio run -e scale_py32f002a，再用 PlatformIO 终端运行本脚本。")


def all_reads_present(output):
    # pyOCD 0.36 commander 可能在命令失败后仍返回0，检查实际数据行。
    return all(re.search(r"(?im)^\s*" + address + r":\s+[0-9a-f]{8}\b", output)
               for address in READS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="只列出USB烧录器")
    parser.add_argument("--uid", default="cmsisdap:",
                        help="默认 cmsisdap: 用于 PWLINK2 Lite；jlink: 用于队友 J-Link；也可指定序号")
    parser.add_argument("--frequency", type=int, default=100000, help="SWD Hz，默认100000")
    parser.add_argument("--python", help="指定PlatformIO的Python解释器")
    parser.add_argument("--dry-run", action="store_true", help="打印命令，保持设备状态")
    args = parser.parse_args()
    if args.frequency <= 0:
        parser.error("frequency必须为正整数")
    python, entry = locate_runtime(args.python)
    inventory = [python, entry, "list", "--probes", "--color", "never"]
    connection = [python, entry, "commander", "--target", "py32f002ax5", "--pack", str(PACK),
                  "--frequency", str(args.frequency), "--connect", "halt", "--no-wait", "-vv",
                  "--color", "never", "-O", "reset_type=sysresetreq", "-O", "auto_unlock=false"]
    # 未限制类型的实际序号仍可能属于 J-Link，保留相应 SDK 兼容设置。
    if not args.uid.lower().startswith("cmsisdap:"):
        connection += ["-O", "jlink.power=false", "-O", "jlink.non_interactive=false"]
    if args.uid:
        connection += ["--uid", args.uid]
    for command in ("show probe-uid", "show target", "read32 0xe000ed00 4",
                    "read32 0x20000000 16", "read32 0x08000000 16"):
        connection += ["-c", command]
    commands = [inventory] if args.list else [inventory, connection]
    if args.dry_run:
        for command in commands:
            print(shlex.join(command))
        return 0
    folder = ROOT / "logs"
    folder.mkdir(exist_ok=True)
    log = folder / ("swd-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-UTC") + ".log")
    with log.open("w", encoding="utf-8") as stream:
        stream.write("SWD diagnostic: target=py32f002ax5; external target power; frequency="
                     + str(args.frequency) + " Hz\n")
        if not args.list:
            setup = subprocess.run([python, str(ROOT / "tools/setup_pack.py")], cwd=ROOT,
                                   capture_output=True, text=True, encoding="utf-8",
                                   errors="replace", timeout=90)
            stream.write(setup.stdout + setup.stderr)
            print(setup.stdout + setup.stderr, end="", flush=True)
            if setup.returncode:
                print("芯片包准备失败，日志：", log)
                return setup.returncode
        for index, command in enumerate(commands):
            print("USB烧录器列表：" if index == 0 else "读取CPUID、RAM和Flash；会暂停内核：", flush=True)
            stream.write("\n$ " + shlex.join(command) + "\n")
            stream.flush()
            try:
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                        encoding="utf-8", errors="replace", timeout=45,
                                        stdin=subprocess.DEVNULL)
            except subprocess.TimeoutExpired as error:
                for partial in (error.stdout, error.stderr):
                    if partial:
                        stream.write(partial.decode("utf-8", errors="replace")
                                     if isinstance(partial, bytes) else partial)
                stream.write("\nTIMEOUT after 45 seconds\n")
                print("本阶段连接超时，日志：", log)
                return 1
            output = result.stdout + result.stderr
            stream.write(output + "\nexit_code=" + str(result.returncode) + "\n")
            print(output, end="", flush=True)
            if result.returncode or (index == 1 and not all_reads_present(output)):
                print("读取未完成，把这份日志发回：", log)
                return result.returncode or 1
    print("日志：", log)
    if not args.list:
        print("CPUID、RAM、Flash读取完成；按SW1复位后可继续上传。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error))
