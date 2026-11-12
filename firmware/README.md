# 固件文件

当前版本为 **[v1.1.1](v1.1.1)**，完整目标使用SW4按键与HT1621B段码LCD。`scale_basic.*`为完整固件，`blink.*`为点灯诊断。

每版均提供ELF/HEX/BIN、`SHA256SUMS`文件哈希及`manifest.json`源码哈希。ELF/HEX包含地址；BIN从`0x08000000`写入。按扇区擦除保留`0x08004000`之后的配置区。

```powershell
pio run -e scale_py32f002a -e scale_basic
python tools/package_firmware.py --version v1.1.1
```

v1.0.2保留为串口命令版历史固件。当前按键操作见[新版指南](../docs/button-guide.md)。
