"""让 PIO 内存统计包含中断向量表，以及链接器预留的堆栈空间。"""
Import("env")

# Flash 统计代码和初始化数据，RAM 统计静态数据、SRAM 向量表及预留堆栈。
env.Replace(
    SIZEPROGREGEXP=r"^(?:\.isr_vector|\.text|\.data|\.rodata|\.text\.align|\.ARM\.exidx|\.ARM|\.ARM\.extab|\.preinit_array|\.init_array|\.fini_array)\s+(\d+).*",
    SIZEDATAREGEXP=r"^(?:\.ram_vector|\.data|\.bss|\.noinit|\._user_heap_stack)\s+(\d+).*",
)
