# ARM V9 / J-Link 首次连接与日志采集

按常见的“秤板自行供电、ARM V9出厂设置”开始。此次照片中的下载器标注支持1.8～5V调试，同时写明VOUT输出3.3V、Vtref与VOUT出厂默认内部短接。

## 1. 先设置硬件参考电压

1. 断开下载器USB与秤板电源。
2. 按厂家说明断开下载器内部Vtref/VOUT短接开关；断电用万用表核对1脚与2脚已不再直接短接。开关位置取决于该下载器硬件版本。
3. 连接下表四根线，使用秤板自身电源。下载器2脚VOUT与19脚5V电源输出保持空置。
4. 给秤板上电，再连接下载器USB。测量下载器1脚Vtref对GND，读数应接近主控实际5V。

| 主板H4针号 / 信号 | 此照片20针ARM V9下载器 |
|---|---|
| H4-4 / VCC5V | 1脚Vtref，作为目标参考输入 |
| H4-3 / PA14 SWCLK | 9脚TCK/SWCLK |
| H4-2 / PA13 SWDIO | 7脚TMS/SWDIO |
| H4-1 / GND | 4脚GND，或该下载器其他标为GND的脚 |

按实际接口的1脚标记与丝印找位置。照片这款2脚为VOUT；同为20针外形的其他设备按各自接口定义接线。内部物理短接需要硬件开关断开，pyOCD选项`jlink.power=false`的作用是禁止连接过程主动请求开启目标供电。

## 2. 更新项目并准备工具

```powershell
git pull --ff-only
pio run -e scale_py32f002a
python tools/setup_pack.py
```

J-Link类下载器安装对应J-Link驱动包，让pyOCD通过J-Link后端枚举设备。可以先在J-Link软件中查看目标电压、选择SWD和100kHz检查连接，再关闭占用下载器的软件，回到PlatformIO。

## 3. 先读信息，后上传

```powershell
python tools/swd_diagnose.py --list
python tools/swd_diagnose.py --uid jlink:
```

CMSIS-DAP使用`python tools/swd_diagnose.py`。有多个下载器时，在`--uid`后指定实际序号。脚本从PlatformIO包管理器定位pyOCD，兼容用户目录和`C:/.platformio`等Core目录；也可用`--python`指定PlatformIO的Python解释器。

诊断依次读取：

| 阶段 | 结果与定位作用 |
|---|---|
| USB列表 | 名称、后端和序号，判断电脑是否枚举到下载器 |
| 目标初始化 | DEBUG日志中的DP/AP、内核与错误位置，判断SWD访问进度 |
| `0xE000ED00` | CPUID，读取内核身份信息 |
| `0x20000000` | RAM开头16字节，检查RAM读取 |
| `0x08000000` | Flash开头16字节，检查程序区读取 |

连接模式为halt，诊断会暂停内核；结束后按SW1复位恢复应用。脚本只执行上述读取与连接初始化，Flash擦除和程序写入在下一步Upload执行。日志保存在`logs/swd-日期时间-UTC.log`，读取不完整时脚本返回失败。

完成硬件核对后上传点灯版：

```powershell
pio run -v -e scale_py32f002a -t upload 2>&1 | Tee-Object upload.log
```

当前参数：100kHz SWD、`connect_mode=halt`、`reset_type=sysresetreq`、`auto_unlock=false`、`jlink.power=false`、`jlink.non_interactive=false`。要降低到50kHz，在`platformio.ini`中设置`custom_swd_frequency = 50000`；连接正常后可提高速度。

### J-Link 软件兼容设置

项目固定的pyOCD 0.36会先调用`disable_dialog_boxes()`，再调用J-Link的`open()`。J-Link DLL 9.14及以上版本在这个顺序下存在已知连接问题，上游在[PR #1927](https://github.com/pyocd/pyOCD/pull/1927)中修复。上传和诊断脚本设置`jlink.non_interactive=false`，允许SDK显示对话框，从而跳过这次提前调用。

两个脚本同时设置`jlink.power=false`，跳过`SupplyPower = 1`命令。旧上传脚本没有覆盖pyOCD的供电默认值；不支持软件供电控制的下载器可能在此处抛出J-Link异常，随后被转换为`Memory transfer fault`。

## 4. 按日志定位

| 现象 | 下一步 |
|---|---|
| USB列表没有J-Link | 查数据线、USB识别、J-Link驱动和软件占用 |
| Vtref约3.3V而秤板主控为5V | 查Vtref/VOUT内部短接和参考端接法 |
| `No ACK` / 无有效DPIDR | 查供电、共地、IO/CLK、目标参考与信号电平 |
| `Memory transfer fault (Unspecified error.)` | 查看DEBUG日志中的前一操作和调用栈；J-Link的通用错误会转换为该异常 |
| CPUID/RAM可读，Flash读或擦写失败 | 进一步查目标保护状态、DFP算法与Flash访问阶段 |
| 复位后可连接，运行后失败 | 检查板上原程序的低功耗与SWD引脚复用；核对NRST连接条件 |

H4四线中没有NRST。自动under-reset连接需要把下载器15脚nRESET接到已确认的主控NRST网络。照片的3脚nTRST是JTAG复位，与15脚CPU复位区分使用。

固件可编译与实物连接是两个阶段。此次截图已完成点灯版编译，后续以完整SWD诊断日志定位失败阶段。

参考：[pyOCD J-Link后端](https://pyocd.io/docs/debug_probes.html#j-link)、[会话参数](https://pyocd.io/docs/options.html)、[SEGGER 20针SWD接口](https://www.segger.com/products/debug-probes/j-link/technology/interface-description/)。
