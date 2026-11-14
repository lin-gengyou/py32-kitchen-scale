# v2.0.0 ECB01C 蓝牙协议与客户端

秤板独立完成采样、标定、设置和显示。蓝牙提供实时重量、远端去皮、唤醒及重连同步，与板上按键共用同一套称重状态。

## 接口

| 接口 | 值 |
|---|---|
| MCU USART1 | PA2 TX、PA3 RX，AF1，9600bps / 8N1 |
| ECB01C BLE服务 | 0000fff0-0000-1000-8000-00805f9b34fb |
| 通知 / 下发 | FFF1 Notify / FFF2 Write |
| 默认广播名 | BT_XXXXXXXXXXXX |
| 上报 | 随菜单06的100～200ms刷新周期，5～10Hz |
| 本机开关 | 菜单20：0关闭MCU通信，1开启，默认1 |

接口依据：[易加物联ECB01C技术手册V1.2第7页](https://offer-product.oss-cn-beijing.aliyuncs.com/product/offer/attachment/2215891934535/file/subPdf_561358_264786_20230530-174416186.pdf#page=7)。模块在手机3分钟没有下行数据后主动断开；配套客户端每5秒发送PING。

## 使用客户端

1. 按[板上操作指南](button-guide.md)用20g完成标定，菜单20选1。
2. 在仓库根目录执行 `python -m http.server 8765 --directory web --bind 127.0.0.1`，用Chrome或Edge打开 `http://localhost:8765`。
3. 点“连接蓝牙秤”，选择实物设备；页面订阅FFF1并用STATUS同步去皮序号。
4. 放容器，等“稳定”，点击“去皮 / 归零净重”，加入物品读取净重。
5. 页面支持g、kg、oz、lb:oz显示。板上g/kg设置保存在MCU；网页转换只影响当前页面。
6. 软件待机后点击“唤醒秤”，等待重新建立采样。

断线时清空读数，按1、2、4、8、15秒退避重连，每次重新获取服务/特性并订阅。3秒没有重量帧时隐藏旧数值。主动断开停止重连。演示模式有明确标记。

Web Bluetooth使用安全上下文和用户点击触发设备选择：电脑用localhost；Android平板用支持Web Bluetooth的Chrome和HTTPS地址；iPad可用原生BLE应用按本页协议接入。机制见[Chrome官方文档](https://developer.chrome.com/docs/capabilities/bluetooth)。

## 重量帧

```text
W,<seq>,<net_g>,<stable>,<tare_g>,<battery>,<status>\r\n
W,1024,136.4,1,120.0,-1,OK\r\n
W,1025,,0,,-1,OL\r\n
```

行末实际发送0D、0A。净重/皮重始终用g、一位小数，允许负数；seq为递增uint32、允许回绕。stable=0/1，只有OK帧含有效重量。外部供电的battery=-1，电池模式为0～100的电压估算百分比。

| status | 含义 |
|---|---|
| WAIT | 等待新鲜采样 |
| UNCAL | 尚无标定 |
| OK | 正常，稳定性另看stable |
| RANGE | 超出正常5kg范围 |
| OL | 毛重达到5250g |
| ERR | HX711超时、满码或读取异常 |
| LOWBAT | 所选电池类型电压低 |
| POWERERR | 电池模式下ADC电压采样异常 |
| CALIBRATING | 本机标定向导进行中 |
| SLEEP | 软件待机 |

## 全部远端指令

命令使用ASCII并追加CRLF；收到对应完整应答后再发下一条。

| 下发 | 回复 | 动作 |
|---|---|---|
| PING | OK,PING | 心跳；保持软件待机计时 |
| STATUS | S,SEEN=0,TARE_SEQ=0 | 最近去皮请求；SEEN=1表示缓存有效 |
| TARE,42 | ACK,TARE,42 或 ERR,TARE,42,原因 | 稳定去皮，与SW4短按共用逻辑 |
| WAKE | OK,WAKE | 唤醒，重新建立采样 |

本机菜单涵盖全部设置。标定、量程与电源配置通过[板上菜单](button-guide.md)操作。

## 去皮序号与重连

每个新操作使用新uint32序号。设备缓存最近一个请求及其完整成功/失败应答；相同序号直接回复缓存，较旧序号返回STALE_SEQ。最大值后回绕0，新旧距离须小于2³¹。

TARE,42因不稳失败后，重复42仍返回原失败结果；稳定后用新请求43。43的ACK丢失时重发43，不会再次去皮或蜂鸣。缓存保留至主控复位，软件待机/菜单关闭再开启蓝牙会保留缓存。重连先STATUS，下一次用户操作采用缓存序号加1。

客户端1800ms无应答时最多重试2次，相同操作保留原序号。连接中断终止在途操作；重连显示实际秤状态后，由用户发起新的去皮。

| 原因 | 含义 |
|---|---|
| NEED_CAL | 先用菜单07标定 |
| WAIT_SAMPLES | 新鲜有效样本不足 |
| UNSTABLE | 尚不稳定 |
| OVERLOAD | 总负载超过5000g |
| POWER | 电池模式电压低/采样异常 |
| BUSY | 本机正在标定 |
| STALE_SEQ | 请求早于最近序号 |
| ERR,COMMAND | 指令/参数/uint32格式无效 |
| ERR,FRAME | 超长或非ASCII可打印字符使整行失效 |

## 实现入口

接收环形队列64字节、命令最多31字符，RX中断只入队；主循环组帧执行。应答队列2条，完整发送一行后再切帧。接收错误丢弃至换行再同步。接收端处理任意分包/粘包。

- 固件：src/ble_link.c；统一回调scale_remote_tare、scale_remote_wake。
- 客户端：web/app.mjs；纯协议与重试：web/protocol.mjs。
- C回归：`python tools/run_host_checks.py`。
- 协议回归：`node --test tests/web/protocol.test.mjs`。
- 可选模拟GATT界面：安装Playwright后 `node tests/web/ui.test.mjs`，使用本机Edge新建无头会话。

合成8小时检查为288000帧；浏览器界面检查使用模拟GATT。实物无线链路8小时项目单独按需求验收。
