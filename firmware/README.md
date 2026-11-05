# 固件文件

`v1.0.2/blink.elf`是点灯目标，`v1.0.2/scale_basic.elf`是称重目标。`SHA256SUMS`记录文件哈希，`manifest.json`记录生成该版固件时的源码哈希。

平时在项目根目录通过PIO构建与上传，具体步骤见README。重新打包使用：

```powershell
pio run -e scale_py32f002a -e scale_basic
python tools/package_firmware.py --version v1.0.2
```

ELF包括Flash地址信息，pyOCD可直接加载。GitHub Actions也保存每次提交对应的ELF产物。
