# 队友协作与代码使用

## 1. 第一次加入

1. 克隆仓库，安装README中的开发环境。
2. 运行两个目标的Build，按README完成点灯、称重固件下载和20g默认单点标定。
3. 阅读v2.0.1按键指南与硬件连接说明，按下面的分工入口开发。

| 方向 | 起始文件 / 接口 | 交付内容 |
|---|---|---|
| 称重与滤波 | `src/scale_basic.c`：sample_add、weight_update、gross_milligrams | 固件改动与砝码测量记录 |
| 按键、蜂鸣、待机 | src/scale_ui.inc的key_poll；主文件buzzer_poll、power_poll | 状态转换与实物操作记录 |
| 彩屏与段码屏 | `include/weight_state.h`、硬件文档屏幕接口 | HT1621健康位显示；按docs/color-display.md接入彩屏 |
| 电池检测 | PB1 BAT-ADC、WeightState.battery_percent | src/battery.c电压换算、曲线与低电提示 |
| 手机 / RK3566 | `docs/protocol.md` | web/现有客户端与RK3566应用接入、重连 |
| 整机验收 | `REQUIREMENTS.md`、`docs/measurements/` | 全量程、重复、偏载、漂移和连续运行记录 |

## 2. 每次开发

```powershell
git switch main
git pull --ff-only
git switch -c feature/功能名称
```

修改对应文件，在本机检查：

```powershell
pio run -e scale_py32f002a -e scale_basic
python tools/run_host_checks.py
node --test tests/web/protocol.test.mjs
```

Windows的GCC可通过`--cc`指定，具体命令见README。完成实物检查后，在`docs/measurements/`填入设备、版本、动作与结果。

```powershell
git add src include tools tests docs README.md platformio.ini
git commit -m "说明具体改动和行为"
git push -u origin feature/功能名称
```

有仓库写权限的队友推送分支并创建Pull Request。通过公开链接参与的队友可先Fork仓库，再从自己的分支创建Pull Request。

## 3. Pull Request内容

- 改动的触发场景和结果，例如“容器去皮后加载5001g总负载进入RANGE”。
- 修改的接口、参数、协议版本和默认值。
- 两个目标的构建结果、模拟检查结果、实物测量记录。
- 上位机与固件共同调整时，注明对应分支和联调顺序。

## 4. 发布固件与文档

1. 在`main`上构建两个目标并运行模拟检查。
2. 更新include/firmware_version.h、按键指南、需求对照与版本变更。
3. 运行`python tools/package_firmware.py --version 版本号`，生成ELF与SHA-256清单。
4. 以docs/button-guide.md维护当前操作；两份1.0.2 PDF作为历史快照保留。
5. 将代码和对应固件作为同一版发布，队友通过板上InFo菜单核对设备版本。

## 5. 两种平台工作分配

v2.0.0由MCU完成采样、滤波、标定、按键菜单、段码显示、电压监测、蜂鸣、待机、参数保存与BLE协议。web/客户端提供重量显示、远端去皮、重试和重连；RK3566应用可复用docs/protocol.md接口。
