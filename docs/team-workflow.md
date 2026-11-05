# 队友协作与代码使用

## 1. 第一次加入

1. 克隆仓库，安装README中的开发环境。
2. 运行两个目标的Build，按README完成点灯、称重固件下载和三点标定。
3. 阅读两份PDF中的硬件定位与代码图解，按下面的分工入口开发。

| 方向 | 起始文件 / 接口 | 交付内容 |
|---|---|---|
| 称重与滤波 | `src/scale_basic.c`：sample_add、weight_update、gross_milligrams | 固件改动与砝码测量记录 |
| 按键、蜂鸣、待机 | 同文件的key_poll、buzzer_poll、power_poll | 状态转换与实物操作记录 |
| 彩屏与段码屏 | `include/weight_state.h`、硬件文档屏幕接口 | 控制器/段映射、显示驱动、统一状态刷新 |
| 电池检测 | PB1 BAT-ADC、WeightState.battery_percent | ADC校准、电量曲线、低电提示 |
| 手机 / RK3566 | `docs/protocol.md` | BLE连接、通知组帧、命令队列、UI、重连 |
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
2. 更新固件版本、协议说明、需求对照和实物记录。
3. 运行`python tools/package_firmware.py --version 版本号`，生成ELF与SHA-256清单。
4. 更新两份PDF的代码图与说明；仓库内1.0.2手册是该版源码快照，行号在图下列出。
5. 将代码和对应固件作为同一版发布，队友通过INFO核对设备版本。

## 5. 两种平台工作分配

MCU负责ADC、滤波、标定、重量状态、控制、蜂鸣与参数存储。RK3566或手机负责扫描连接、自动重连、文本组帧、去皮请求序号、界面和记录。协议统一以g传输重量，显示单位由应用或本地显示任务完成换算。
