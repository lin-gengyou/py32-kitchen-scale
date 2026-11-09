"""适配 SDK 系统文件，并配置外部供电、低速 SWD 的 pyOCD 上传命令。"""
from pathlib import Path

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
framework = Path(env.PioPlatform().get_package_dir("framework-py32f0sdk"))
# 供 src/system_clock.c 找到本平台漏选的 SDK 系统文件。
env.Append(CPPPATH=[str(framework / "CMSIS" / "Device" / "PY32F0xx" / "Source")])
uploader = Path(env.PioPlatform().get_package_dir("tool-pyocd")) / "pyocd.py"
pack = project / "packs" / "Puya.PY32F0xx_DFP.1.2.20.pack"
# 杜邦线首次联调使用 100kHz，可在 platformio.ini 中调整。
frequency = int(env.GetProjectOption("custom_swd_frequency", "100000"))
if frequency <= 0:
    raise ValueError("custom_swd_frequency 必须为正整数，单位 Hz")
# 秤板独立供电：禁止 J-Link 后端主动请求开启目标供电。
# 此选项不控制 ARM V9 的内部 Vtref/VOUT 物理短接开关。
# pyOCD 0.36 会在 open() 前调用 disable_dialog_boxes()，与 J-Link DLL 9.14+
# 存在已知兼容问题。允许 SDK 弹窗以跳过这次提前调用；见上游 PR #1927。
env.Replace(
    UPLOADCMD='"$PYTHONEXE" "{}" load -vv --target py32f002ax5 '
    '--pack "{}" --erase sector --frequency {} -O connect_mode=halt '
    '-O reset_type=sysresetreq -O auto_unlock=false -O jlink.power=false '
    '-O jlink.non_interactive=false "$SOURCE"'.format(
        uploader, pack, frequency
    )
)
