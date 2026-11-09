"""生成标准 HEX/BIN，并让正常构建及跳过编译的上传入口使用同一份 ELF。"""
Import("env")

elf = "$BUILD_DIR/${PROGNAME}.elf"
# 平台的 nobuild 入口固定引用 firmware.hex；显式注册转换依赖。
# 这样已有 ELF 时能直接生成 HEX，不必重新编译，也不会引用过期 HEX。
images = env.ElfToHex("$BUILD_DIR/${PROGNAME}.hex", elf)
images += env.ElfToBin("$BUILD_DIR/${PROGNAME}.bin", elf)
env.Alias("buildprog", images)
env.Depends("upload", images)
