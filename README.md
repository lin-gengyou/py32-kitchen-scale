# 5kg 厨房电子秤 · PY32F002A

**v2.0.1：SW4独立操作、HT1621B段码LCD、ECB01C蓝牙。** 默认外部供电、单点20g标定、空秤稳定无操作120秒软件待机。21项菜单在板上完成称重、去皮归零、标定和所有设备设置；配套浏览器客户端查看重量并远程去皮。

开发环境：**VS Code + PlatformIO + C / 普冉HAL**。仓库包含源码、板型、链接脚本、固定依赖、下载脚本、原理图、需求文档、操作指南、蓝牙客户端和固件。

![PCB](docs/images/pcb-3d.png)

## 先操作起来

| SW4在称重页的操作 | 功能 |
|---|---|
| 短按 | 稳定去皮 |
| 双击 | g/kg切换并保存 |
| 按住1.5～不足3秒松开 | 空秤归零、清除皮重 |
| 按住至少3秒松开 | 进入01～21设置菜单 |
| 待机时按一下 | 唤醒，重新采样 |

菜单：短按下一项、约2秒长按进入/确认、双击返回。数字编辑：短按改闪烁位、双击移位、约2秒长按确认、3秒长按取消。SW1是复位键，SW3是电源开关。

**[完整操作指南：21项菜单、20g标定、电压与蓝牙](docs/button-guide.md)**

## 功能

- HX711 A/128，3点中值、动态3点/稳定8点平均，按标定换算0.2g门限、持续500ms判稳，退出滞回与慢漂锚点。
- 5000g容量、5250g超载；按毛重保护，负净重、0.5g小负载保留；0.1/0.5/1g分段分度。
- 稳定去皮、空秤归零、清皮、g/kg，1～3点正负方向标定；默认1点20g，最后一点整体保存。
- 21项菜单：提示音、待机、判稳、刷新、ID/日期、原始计数、屏幕全段/RAM自检、电源、电压、Vref、缺段绕行、蓝牙、恢复默认。
- 外部电源默认电量未知；可选锂电/碱性/镍氢，电压估算百分比、低电滞回、ADC错误重试、待机关闭ADC。
- 成功1声、错误3声；静音/有源/无源500Hz；约4秒IWDG与调试暂停冻结。
- V4双页CRC参数，V1/V2/V3标定迁移，写入验证、坏页回退、相同设置免写。
- BLE 5～10Hz重量、带序号去皮与成功/失败去重、心跳、状态同步、唤醒。
- 浏览器客户端：实时重量、稳定/皮重/电源/故障、单位转换、去皮、唤醒、分包解析、超时重试、断线重连。

显示默认跳过实物数值区第三个缺段位，使用其余5位。菜单19可切回6位；负号移入上方，5000.0g仍完整显示。

## 1. 下载与构建

安装[VS Code](https://code.visualstudio.com/)、[Git](https://git-scm.com/downloads)及VS Code扩展PlatformIO IDE、C/C++。

```powershell
git clone https://github.com/lin-gengyou/py32-kitchen-scale.git
cd py32-kitchen-scale
code .
```

在VS Code执行 **PlatformIO: New Terminal**：

```powershell
python tools/setup_pack.py
pio run
```

默认构建完整固件scale_basic。Upload会自动准备芯片包。平台、HAL、GCC和DFP版本见[依赖](docs/third-party.md)。

| PIO环境 | 内容 | 烧录器 |
|---|---|---|
| scale_basic | 全部称重/按键/LCD/BLE功能，默认 | PWLINK2 Lite / CMSIS-DAP |
| scale_basic_jlink | 同一份完整固件 | ARM V9 / J-Link |
| scale_py32f002a | 点灯诊断，LED2每500ms翻转 | CMSIS-DAP |
| scale_py32f002a_jlink | 同一点灯固件 | J-Link |

Windows普通PowerShell可直接调用 `& "$env:USERPROFILE/.platformio/penv/Scripts/pio.exe" run`。完整固件约15KB Flash、2KB RAM（含向量表与预留堆栈），程序上限16KB、总RAM3KB；末尾4KB为独立配置区。编译采用GCC14的Oz与LTO。

## 2. 接线与烧录

秤板使用自身电源。PWLINK2 Lite先在[PowerWriter客户端](https://www.powerwriter.com/index/index/products.html?c=files&p=&t=Client)选择接口电平→外部输入→应用→保存全局上电设置，再断电接线。

从屏幕端向板外侧数H4：

| 位置 | 主板 | 烧录器 |
|---|---|---|
| 第1根 / H4-4 | VCC5V | VREF/TVCC参考输入 |
| 第2根 / H4-3 | PA14 | SWCLK |
| 第3根 / H4-2 | PA13 | SWDIO |
| 第4根 / H4-1 | GND | GND |

烧录器独立电源输出脚空置。ARM V9按[烧录说明](docs/upload-troubleshooting.md)处理内部Vtref/VOUT短接。完整PCB定位见[硬件说明](docs/hardware.md)。

```powershell
# PWLINK2 Lite
pio run -e scale_basic -t upload
# 队友的J-Link
pio run -e scale_basic_jlink -t upload
```

也可在PlatformIO → Project Tasks → 对应环境点Build/Upload。下载使用Puya DFP、100kHz SWD、扇区擦除，保留独立配置区。完成后复位运行，LCD先自检再显示2.0.1。

## 3. 20g标定与第一次称重

1. SW4按住3秒松开进菜单，短按6次到07，按约2秒松开进入。
2. 默认点数1，按约2秒确认。
3. 清空秤盘，ZERO出现S时按约2秒采集空秤。
4. 默认质量20g，按约2秒确认。
5. 放20g砝码，C1g出现S后按约2秒采集保存。
6. 移走读约0.0，再放20g检查约20.0。

5kg量程保持不变。两点/三点标定支持编辑递增总质量。容器称重：先放空容器等S→短按去皮→加入物品。操作细节见[完整指南](docs/button-guide.md)。

## 4. 蓝牙客户端

仓库根目录运行：

```powershell
python -m http.server 8765 --directory web --bind 127.0.0.1
```

在Chrome/Edge打开 **http://localhost:8765**，点“连接蓝牙秤”，选择BT_设备。显示稳定后可远端去皮；断线自动重新发现服务并订阅。页面也提供明确标识的演示模式。

服务FFF0，FFF1通知、FFF2写入，MCU USART1为9600/8N1。外部电源模式battery=-1，页面显示未知。Android平板通过支持Web Bluetooth的Chrome及HTTPS地址使用；原生应用可按[完整蓝牙协议](docs/protocol.md)接入。

## 5. 更新与现成固件

```powershell
git pull --ff-only
pio run -e scale_basic -t upload
```

当前文件：[firmware/v2.0.1](firmware/v2.0.1)，对应[GitHub Release](https://github.com/lin-gengyou/py32-kitchen-scale/releases/tag/v2.0.1)。

- scale_basic.hex / .elf包含地址；scale_basic.bin起始地址0x08000000。
- blink.*为点灯诊断；SHA256SUMS核对固件，manifest.json核对源码。
- 旧版本与1.0.2 PDF保留为历史快照，当前操作使用v2.0.1指南。

## 6. 彩屏怎么用

彩屏规划为“重量主界面＋20g标定向导＋完整设置＋故障说明”，与段码屏共用SW4及称重状态。客户端服务启动后打开 **http://localhost:8765/tft-preview.html** 体验界面。接口、分块刷新、共享总线和字库方案见[彩屏设计](docs/color-display.md)。当前屏幕控制器型号待确认，发布固件主显示使用段码LCD。

## 7. 代码入口与检查

| 文件 | 内容 |
|---|---|
| src/scale_basic.c | HX711、滤波、换算、去皮归零、待机、报警与调度 |
| src/scale_ui.inc | 实体键、21菜单、数字编辑、原子标定向导 |
| src/lcd_ht1621.c | PCB段映射、5/6位布局、负号与实际RAM读回 |
| src/battery.c | PB1/内部参考、异步ADC、电源类型、百分比与低电滞回 |
| src/ble_link.c | USART中断缓冲、ASCII协议、去重、无交错发送 |
| src/scale_config.c | 双页CRC、范围校验、V1/V2迁移 |
| web/ | 终端客户端、纯协议、彩屏预览 |
| tests/host/、tests/web/ | 固件逻辑、存储截断、协议与界面回归 |

```powershell
python tools/run_host_checks.py
node --test tests/web/protocol.test.mjs
pio run -e scale_py32f002a -e scale_basic -e scale_py32f002a_jlink -e scale_basic_jlink
python tools/package_firmware.py --version v2.0.1
```

Windows主机检查可加 `--cc C:/msys64/ucrt64/bin/gcc.exe`。已覆盖合成8小时蓝牙/24小时称重调度；整机精度、偏载、漂移与无线长测按[需求对照](REQUIREMENTS.md)逐项测量。20g单点完成当前校准流程，大量程精度使用对应质量标准砝码检验。

[全部操作](docs/button-guide.md) · [需求](REQUIREMENTS.md) · [版本变更](CHANGELOG.md) · [团队开发](docs/team-workflow.md) · [段码定位](docs/lcd-mapping.md)
