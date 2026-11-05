# 普冉芯片支持包

在项目根目录运行`python tools/setup_pack.py`。脚本从普冉官网下载、校验并写入本目录：

`Puya.PY32F0xx_DFP.1.2.20.pack`

该文件用于pyOCD识别PY32F002Ax5并加载Flash算法。下载地址和SHA-256固定在脚本内。固件Build获取平台/SDK，Upload使用这里的芯片包。
