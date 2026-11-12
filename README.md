# 5kg 厨房电子秤 · PY32F002A

**v1.1.1：板上 SW4 按键操作，HT1621B 段码 LCD 显示。** 称重、去皮、归零、单位、标定和设置均在秤板完成。串口和蓝牙未启用，彩屏片选保持空闲。

开发环境为 **VS Code + PlatformIO + C / 普冉 HAL**。仓库包含完整源码、自定义板型、链接脚本、固定版本依赖、烧录脚本、操作说明、原理图和可直接下载的固件。

![电子秤 PCB 3D 图](docs/images/pcb-3d.png)

## 马上操作

操作键是 **SW4 / PB3**；**SW1 是硬件复位键**。

| 在称重页操作 SW4 | 功能 |
|---|---|
| 短按一次 | 稳定后去皮 |
| 连按两次 | g / kg 切换并保存 |
| 按住 1.5～不足 3 秒，再松开 | 空秤归零，同时清除皮重 |
| 按住至少 3 秒，再松开 | 进入数字设置菜单，初始显示 `01` |
| 待机时按一下 | 唤醒；这次按键只唤醒 |

**菜单里：短按下一项，按住 1.5～不足 3 秒松开进入/确认，双击返回。** 数字编辑时短按改当前闪烁位，双击移位，长按确认；按住 3 秒松开取消。

全部菜单、屏幕缩写、逐位改数字和三点标定步骤见 **[v1.1.1 完整按键操作指南](docs/button-guide.md)**。

## 功能

- HX711 A 通道、128 倍增益，三点中值滤波、动态 3 点 / 稳定 8 点平均，稳定窗口保持 500ms。
- 最大称量 5000g，5000g 以上显示 `rAnGE`，达到 5250g 显示 `OL`；去皮后仍按总负载判断。
- 0.1g / 0.5g / 1g 分段分度，保留负净重和真实 0.5g 小负载。
- 稳定去皮、空秤归零、清除皮重、g/kg 切换。
- 板上完成 1～3 点标定；默认三点 500g、2500g、5000g，各点质量可编辑。最后一点成功后整体保存，中途取消保留原标定。
- 板上设置静音/有源/无源蜂鸣器、待机秒数、判稳阈值、刷新周期、设备编号、标定日期。
- 板上查看 HX711 原始计数、固件版本、设备信息与标定点数，执行屏幕全亮/数字/空白自检，并实际读回控制器RAM。
- 默认空秤稳定、无皮重且无操作 120 秒后软件待机，关闭 LCD 和 HX711；SW4 唤醒。
- 双页 CRC32 参数保存，兼容 v1.0.2 标定记录，约 4 秒独立看门狗。

## 1. 下载与构建

安装 [VS Code](https://code.visualstudio.com/)、[Git](https://git-scm.com/downloads) 和 VS Code 扩展 **PlatformIO IDE**、**C/C++**。

```powershell
git clone https://github.com/lin-gengyou/py32-kitchen-scale.git
cd py32-kitchen-scale
code .
```

在 VS Code 执行 **PlatformIO: New Terminal**，运行：

```powershell
python tools/setup_pack.py
pio run
```

默认目标为完整按键固件 `scale_basic`。平台、HAL、GCC 和 DFP 固定版本见 `platformio.ini` 与 [依赖说明](docs/third-party.md)。

Windows 普通 PowerShell 可直接调用 PlatformIO 自带程序：

```powershell
$pio = "$env:USERPROFILE/.platformio/penv/Scripts/pio.exe"
& $pio run -e scale_basic
& $pio run -e scale_basic -t upload
```

| 环境 | 用途 | 烧录器接口 |
|---|---|---|
| `scale_basic`，默认 | 完整按键 + LCD 称重固件 | PWLINK2 Lite / CMSIS-DAP |
| `scale_basic_jlink` | 同一份完整固件 | ARM V9 / J-Link |
| `scale_py32f002a` | 独立点灯诊断，LED2 每 500ms 翻转 | CMSIS-DAP |
| `scale_py32f002a_jlink` | 同一份点灯诊断 | J-Link |

也可在 PlatformIO → Project Tasks → 对应环境点击 **Build / Upload**。完整固件文件生成在 `.pio/build/scale_basic/`，同目录提供 ELF、HEX、BIN。

本版完整固件约使用 **12.4KB Flash、1.6KB RAM**，程序区域上限 16KB，整片 SRAM 3KB。末尾 4KB Flash 为配置区。

## 2. 接线与烧录

秤板使用自身电源，SWD 接口电平匹配主控 5V。PWLINK2 Lite 在 [PowerWriter 客户端](https://www.powerwriter.com/index/index/products.html?c=files&p=&t=Client) 选择 **接口电平 → 外部输入 → 应用 → 保存全局上电设置**，再断电接线。

从屏幕端向板外侧数 H4：

| 位置 | 主板信号 | 烧录器信号 |
|---|---|---|
| 第 1 根，H4-4 | VCC5V | VREF / TVCC 参考输入 |
| 第 2 根，H4-3 | PA14 | SWCLK |
| 第 3 根，H4-2 | PA13 | SWDIO |
| 第 4 根，H4-1 | GND | GND |

烧录器独立电源输出脚空置。ARM V9 的 Vtref/VOUT 内部短接处理和连接步骤见 [烧录说明](docs/upload-troubleshooting.md)，完整接口见 [硬件说明](docs/hardware.md)。

PWLINK2 Lite 上传：

```powershell
pio run -e scale_basic -t upload
```

队友使用 J-Link：

```powershell
pio run -e scale_basic_jlink -t upload
```

上传使用 Puya DFP、100kHz SWD、扇区擦除，保留配置区。完成后固件复位运行，先显示全段自检 2.5 秒，再显示版本 1.1.1，随后进入称重页。

## 3. 第一次使用与标定

1. 接好 H1 的 E+/E−/A+/A−，安装并放平秤台，上电。
2. 出现 `no CAL` 时，长按 SW4 至少 3 秒后松开，进入菜单，显示 `01`。
3. 短按翻到编号 `07`（标定），按住 1.5～不足 3 秒松开，选择标定点数；默认 3 点。
4. 长按确认，屏幕提示 `ZERO`。移走秤盘上的物品，等 `S` 稳定标记，再长按确认空秤。
5. 输入第一点质量，默认 500g；长按确认后放上对应砝码，等待 `S`，再长按采集。
6. 依次完成默认 2500g 和 5000g。各点使用秤盘上**总质量**，也可逐位输入其他递增质量。
7. 显示 `SAvEd` 后返回称重页，移去砝码开始使用。蜂鸣器模式通过 `bEEP` 菜单选择并保存。

逐步图例式文字说明见 [按键指南](docs/button-guide.md)。

## 4. 更新与下载现成固件

```powershell
git pull --ff-only
pio run -e scale_basic -t upload
```

现成文件位于 **[firmware/v1.1.1](firmware/v1.1.1)**：

- `scale_basic.hex` / `.elf`：包含写入地址。
- `scale_basic.bin`：写入起始地址 `0x08000000`。
- `blink.*`：点灯诊断。
- `SHA256SUMS`：固件哈希；`manifest.json`：对应源码哈希。

v1.0.2 文件保留为历史版本，其串口操作说明和两份旧 PDF 对应旧版。当前操作以本页及 v1.1.1 按键指南为准。

## 5. 代码从哪里读

| 文件 | 内容 |
|---|---|
| `src/scale_basic.c` | HX711、滤波、稳定、换算、去皮归零、待机、报警与主循环 |
| `src/scale_ui.inc` | SW4 消抖、短按/双击/长按、菜单、数字编辑、标定向导与页面 |
| `src/lcd_ht1621.c` | HT1621B 时序、PCB 段线映射、数字与字母、重量和自检 |
| `src/scale_config.c` | 参数有效性、双页 CRC32、V1 记录升级 |
| `include/weight_state.h` | 称重统一状态 |
| `include/firmware_version.h` | 固件版本号与版本显示 |
| `tools/pio_upload.py` | CMSIS-DAP / J-Link 上传脚本 |
| `tests/host/check.c` | 实体按键时序、菜单、标定、LCD 编码和 Flash 模拟检查 |

```powershell
python tools/run_host_checks.py
pio run -e scale_py32f002a -e scale_basic -e scale_py32f002a_jlink -e scale_basic_jlink
python tools/package_firmware.py --version v1.1.1
```

[需求与功能对照](REQUIREMENTS.md) · [版本变更](CHANGELOG.md) · [团队开发](docs/team-workflow.md) · [段码接线与调试](docs/lcd-mapping.md)
