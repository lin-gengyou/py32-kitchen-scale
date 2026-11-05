"""适配 PY32F002A 的 SDK 系统文件路径，并配置 CMSIS-DAP 上传命令。"""
from pathlib import Path

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
framework = Path(env.PioPlatform().get_package_dir("framework-py32f0sdk"))
# 供 src/system_clock.c 找到本平台漏选的 SDK 系统文件。
env.Append(CPPPATH=[str(framework / "CMSIS" / "Device" / "PY32F0xx" / "Source")])
uploader = Path(env.PioPlatform().get_package_dir("tool-pyocd")) / "pyocd.py"
pack = project / "packs" / "Puya.PY32F0xx_DFP.1.2.20.pack"
# 固定芯片和 DFP，使用 1MHz SWD。关闭自动解锁，保留受保护芯片的处理选择。
env.Replace(
    UPLOADCMD='"$PYTHONEXE" "{}" load --target py32f002ax5 '
    '--pack "{}" --erase sector --frequency 1000000 -O connect_mode=halt '
    '-O reset_type=sysresetreq -O auto_unlock=false "$SOURCE"'.format(
        uploader, pack
    )
)
