"""适配 SDK，并按 PIO 环境选择 PWLINK2 Lite 或队友 J-Link 的上传接口。"""
from pathlib import Path

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
framework = Path(env.PioPlatform().get_package_dir("framework-py32f0sdk"))
# 供 src/system_clock.c 找到本平台漏选的 SDK 系统文件。
env.Append(CPPPATH=[str(framework / "CMSIS" / "Device" / "PY32F0xx" / "Source")])
uploader = Path(env.PioPlatform().get_package_dir("tool-pyocd")) / "pyocd.py"
pack = project / "packs" / "Puya.PY32F0xx_DFP.1.2.20.pack"
# 所有 Upload 入口先准备同一份校验过的 DFP，避免首次下载依赖手动运行脚本。
env.AddPreAction(
    "upload",
    env.VerboseAction(
        '"$PYTHONEXE" "{}"'.format(project / "tools" / "setup_pack.py"),
        "Preparing Puya device pack",
    ),
)
# 杜邦线首次联调使用 100kHz，可在 platformio.ini 中调整。
frequency = int(env.GetProjectOption("custom_swd_frequency", "100000"))
if frequency <= 0:
    raise ValueError("custom_swd_frequency 必须为正整数，单位 Hz")
probe_type = env.GetProjectOption("custom_probe_type", "cmsisdap").strip().lower()
if probe_type not in ("cmsisdap", "jlink"):
    raise ValueError("custom_probe_type 请选择 cmsisdap 或 jlink")
# --uid 的类型前缀只枚举指定接口；芯片目标始终为 py32f002ax5。
command = (
    '"$PYTHONEXE" "{}" load -vv --target py32f002ax5 --uid {}: '
    '--pack "{}" --erase sector --frequency {} -O connect_mode=halt '
    '-O reset_type=sysresetreq -O auto_unlock=false'.format(
        uploader, probe_type, pack, frequency
    )
)
if probe_type == "jlink":
    # 秤板独立供电：关闭 J-Link 的目标供电请求；ARM V9 内部短接另按硬件说明设置。
    # 跳过 pyOCD 0.36 在 open() 前关闭弹窗的调用，适配 J-Link DLL 9.14+；见 PR #1927。
    command += ' -O jlink.power=false -O jlink.non_interactive=false'
env.Replace(UPLOADCMD=command + ' "$SOURCE"')
