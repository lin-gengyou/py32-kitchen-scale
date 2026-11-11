# 历史文档：v1.0.2 蓝牙与串口协议 · 版本 2

**本文对应v1.0.2。v1.1.0使用板上SW4与段码LCD，未启用串口/BLE命令。当前操作见[按键指南](button-guide.md)。**

固件入口为 `src/scale_basic.c`，版本 `1.0.2`。MCU 与 ECB01C 通过 USART1 连接，PA2=TX、PA3=RX、9600bps、8N1。BLE 使用 FFF1 Notify 上报、FFF2 Write 下发；首次连接时通过服务发现核对实际特征。

## 1. 收发与组帧

1. 扫描、连接设备，发现服务，订阅 FFF1。
2. 将命令转换为 ASCII，追加实际 `0D 0A` 两字节后写入 FFF2。
3. 通知片段追加到接收缓冲区；每遇到 CRLF 取出完整一行，余下片段继续积累。
4. 一次通知中可包含多行，一行也可跨多个通知。按行分别识别 `W`、`S`、`INFO`、`OK`、`ACK`、`ERR`、`RAW` 和 `CMDS`。
5. 每次只下发一条命令，收到对应完整回复后再发下一条。

命令名大小写精确匹配，参数间使用表中空格；`UNIT g` 和 `UNIT kg` 的单位参数为小写。接收长度上限 47 个 ASCII 字符，行结束字节另计。固件支持 CR、LF 和 CRLF。超长行、控制字符或 UART 接收错误使整行失效，行末返回 `ERR,COMMAND`。

## 2. 重量帧

```text
W,<sequence>,<net_g>,<stable>,<tare_g>,<battery_percent>,<status>\r\n
W,1024,136.4,1,120.0,-1,OK\r\n
W,1025,,0,,-1,OL\r\n
```

上文 `\r\n` 为可视化写法，线路实际发送 CRLF。

| 字段 | 类型与含义 |
|---|---|
| `sequence` | 本次运行的 uint32 消息序号，每生成 W 帧递增，允许回绕 |
| `net_g` | 正常时为应用分度后、保留一位小数的净重，允许负值 |
| `stable` | 1=稳定；0=动态或无效 |
| `tare_g` | 正常时为皮重，保留一位小数 |
| `battery_percent` | 当前 `-1`=未知；接收端显示 `--` |
| `status` | `WAIT`、`UNCAL`、`OK`、`RANGE`、`OL`、`ERR`、`SLEEP` |

协议中的重量始终使用 g；`UNIT` 控制统一重量状态中的显示单位。`OK` 以外的 W 帧将净重与皮重字段置空，接收端先看状态再处理数值。

显示分度按绝对净重选择，使用 100/500/1000mg；向上超过 200.5g 切换到 0.5g，向下低于 199.5g 返回 0.1g；向上超过 1001g 切换到 1g，向下低于 999g 返回 0.5g。

| 数字状态 | 文本 | 含义 |
|---|---|---|
| 0 | WAIT | 启动、唤醒或故障恢复后等待有效采样 |
| 1 | UNCAL | 尚无有效标定点 |
| 2 | OK | 正常重量，稳定性另看 stable |
| 3 | RANGE | 毛重 >5000g 且 <5250g，或负向/换算范围异常 |
| 4 | OL | 毛重 ≥5250g |
| 5 | ERR | HX711 超时、满码或读取异常 |
| 6 | SLEEP | 软件待机 |

## 3. 全部命令与成功应答

`<...>` 表示要填入的参数。成功应答后均带 CRLF。

| 命令 | 参数与条件 | 成功应答或结果 | 持久性 |
|---|---|---|---|
| `HELP` | 查询入口 | `CMDS,...` 命令列表 | 查询 |
| `INFO` | 设备元数据 | `INFO,FW=1.0.2,ID=...,CAL_DATE=...,PROTO=2,CAP_G=5000,OL_G=5250,BAT=-1,WDG=...` | 查询 |
| `STATUS` | 运行状态 | `S,READY=...,STABLE=...,CAL_POINTS=...,TARE_COUNTS=...,STATUS=...,DIV_MG=...,EMPTY=...,AUTO_OFF_S=...,SLEEP=...,BUZZER=...,UNIT=...` | 查询 |
| `READ` | 单次读数 | 一条 W 帧 | 查询 |
| `RAW` | 数据新鲜且足够 | `RAW,<ADC计数>` | 查询 |
| `ZERO` | 数据新鲜且稳定，已标定时毛重在 ±0.1g 内 | `OK,ZERO`；清皮重、更新运行零点 | RAM；后续 CAL 保存零点 |
| `CAL <g>` | 1～5000 整数克，先 ZERO，数据新鲜且稳定，最多3点 | `OK,CAL,SAVED` | Flash |
| `CALRESET` | 重新标定入口 | `OK,CALRESET` | Flash；其余用户设置保留 |
| `TARE,<seq>` | 已标定、新鲜、稳定、毛重≤5000g | `ACK,TARE,<seq>` | RAM，带请求去重 |
| `TARE` | 同上，供手动联调 | `OK,TARE` | RAM |
| `UNTARE` | 清除当前容器皮重 | `OK,UNTARE` | RAM |
| `STABLE <counts>` | 1～1000000，单位ADC计数；重新积累窗口 | `OK,STABLE` | Flash |
| `RATE <ms>` | 100～200 整数，默认200 | `OK,RATE` | RAM |
| `STREAM 0` / `STREAM 1` | 停止 / 恢复周期 W 帧，默认1 | `OK,STREAM` | RAM |
| `BUZZER OFF` | 静音，默认值 | `OK,BUZZER` | Flash |
| `BUZZER ACTIVE` | 有源蜂鸣器 | `OK,BUZZER` | Flash |
| `BUZZER PASSIVE` | 无源约500Hz | `OK,BUZZER` | Flash |
| `UNIT g` / `UNIT kg` | 显示单位，默认g | `OK,UNIT` | Flash |
| `AUTO_OFF <s>` | 1～86400秒，0关闭，默认120 | `OK,AUTO_OFF` | Flash |
| `DEVICE <id>` | 1～15字符：字母、数字、`-`、`_` | `OK,DEVICE` | Flash |
| `CALDATE <YYYYMMDD>` | 2000～9999年，日期有效 | `OK,CALDATE` | Flash |
| `WAKE` | 主动唤醒、重建采样 | `OK,WAKE` | RAM |
| `LIMIT 5000` | 固定5kg量程的兼容入口 | `OK,LIMIT` | 固定量程 |

`READY` 表示样本足够且新鲜；`TARE_COUNTS` 为皮重对应的 ADC 偏移；`DIV_MG` 为当前分度；`EMPTY` 为近零候选状态。`WDG=1` 表示看门狗初始化成功。`ID=UNSET`、`CAL_DATE=0` 表示元数据等待录入。

## 4. 去皮序号、超时与重连

```text
客户端 → TARE,42
设备   → ERR,TARE,42,UNSTABLE

等待秤台稳定后，发起新请求：
客户端 → TARE,43
设备   → ACK,TARE,43

同一请求因超时重发：
客户端 → TARE,43
设备   → ACK,TARE,43
```

序号使用完整 uint32（0～4294967295），新请求递增，最大值后回绕到0。设备缓存最近一个请求的成功或失败应答；重发该序号直接重发缓存，重量变化后需要再次尝试时使用新序号。旧序号返回 `ERR,TARE,<seq>,STALE_SEQ`。序号解析失败返回 `ERR,TARE,?,BAD_SEQ`。

客户端为同一请求保留原序号，可先用1秒作为超时重试配置。BLE 重连后继续客户端请求序号，订阅通知并查询 INFO / STATUS；设备复位时重新读取标定与运行状态。消息序号和去皮请求序号分别管理。

## 5. 错误应答

普通错误为 `ERR,<原因>`，序号去皮为 `ERR,TARE,<seq>,<原因>`。

| 原因 | 处理 |
|---|---|
| WAIT_SAMPLES | 等待新鲜有效样本；持续出现时检查HX711链路 |
| UNSTABLE | 等秤台稳定，再使用新序号去皮 |
| NEED_CAL | 空秤ZERO后录入CAL点 |
| NOT_EMPTY | 移走负载；查看空秤基线，必要时重新标定 |
| OVERLOAD | 减载，容器计入5kg总负载 |
| CAL_NEEDS_ZERO_1_TO_5000G | 完成ZERO，输入1～5000整数克 |
| CAL_SPAN_OR_ORDER | 检查传感器接线、砝码和点顺序 |
| CAL_MAX_3_POINTS | 按三点流程重新标定 |
| FLASH | 本次配置保存失败，原有效配置继续运行 |
| STALE_SEQ / BAD_SEQ | 修正请求序号及其格式 |
| RATE_100_TO_200_MS | 将上报间隔设为100～200整数 |
| STABLE_COUNTS / BUZZER_MODE / AUTO_OFF_SECONDS / UNIT / DEVICE_ID / CALDATE | 按命令表修正参数 |
| LIMIT_FIXED_5000 | 使用LIMIT 5000 |
| COMMAND | 按ASCII和CRLF重新发送完整命令 |

## 6. 保存、待机与上位机界面

Flash 保存标定零点/曲线、STABLE、BUZZER、UNIT、AUTO_OFF、DEVICE、CALDATE。复位后皮重清零、RATE=200、STREAM=1、命令缓存重建。软件待机保留RAM设置，唤醒后重新积累采样。

任何下行 UART 数据都重置无操作计时，并可唤醒设备。设备上行通知本身保留待机计时。测试120秒自动待机时，暂停客户端的周期 READ / STATUS 请求，观察主动 W 帧。

上位机界面至少显示净重、g/kg单位、稳定状态、去皮状态、连接状态和重量错误。电量字段为-1时显示`--`。RK3566应用的后续工作包括扫描/重连、CRLF缓冲、命令队列、TARE请求序号以及8小时连接测试。
