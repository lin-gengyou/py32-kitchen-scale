# 构建依赖与资料来源

| 组成 | 固定版本 / 来源 | 安装方式 |
|---|---|---|
| PlatformIO Core | 6.1.19 | VS Code PlatformIO扩展 / Python安装 |
| PY32平台 | positron96/platform-py32f0，提交`32e2190799f693bae02227bb854ba4c272147c34` | platformio.ini自动获取 |
| HAL/CMSIS SDK | positron96/framework-py32f0sdk，提交`759979e94a89a97634c1fbee4439f62abe5b91dd` | platformio.ini自动获取 |
| ARM GCC | platformio/toolchain-gccarmnoneeabi 1.140201.0，GCC14.2.1 | PlatformIO包管理 |
| pyOCD | platformio/tool-pyocd ~2.3600.0 | PlatformIO包管理 |
| 普冉DFP | Puya.PY32F0xx_DFP.1.2.20.pack | tools/setup_pack.py |

DFP文件SHA-256：`bf3ddd933f28f4557c90a8070cbc834bd3869ff88e53a8b20d80e79c51f842c4`。

源码、启动文件与HAL通过上述SDK依赖安装。项目中的链接脚本基于对应SDK脚本调整，保留原文件头；当前布局为16KB程序和最后4KB配置扇区。第三方软件的使用条款随各自项目和下载包提供。

来源：

- https://github.com/positron96/platform-py32f0
- https://github.com/positron96/framework-py32f0sdk
- https://www.puyasemi.com/download.html?keywords=PY32F002A
- https://pyocd.io/docs/target_support.html
- https://docs.platformio.org/en/latest/core/userguide/cmd_run.html

项目原理图、需求/设计文档与PCB图放在`docs/`对应目录。两份项目手册对应固件1.0.2的2026-11-05源码快照。

2026-11-06更新连接配置：默认SWD改为100kHz，上传输出DEBUG日志，加入外部供电的J-Link选项与连接诊断入口。当前下载参数和ARM V9接线步骤见README及`upload-troubleshooting.md`。

## v1.1.0显示资料

HT1621时序依据[Holtek原厂手册](https://www.holtek.com/webapi/116711/HT1621_21Gv340.pdf)。GDC0689段名称表参照[WinnerMicro官方SDK](https://github.com/winnermicro/wm_iot_sdk/blob/master/components/driver/src/display/seg_lcd/wm_gdc0689.c)的公开COM/SEG表（Apache-2.0）；本项目自行实现HT1621写接口、字形编码和按本板网络转换的地址表。PCB映射及本机反馈见[lcd-mapping.md](lcd-mapping.md)。
