/* 社区平台没有自动匹配 PY32F002A 对应的系统文件，这里显式编译 SDK 的实现。
 * 该文件提供 SystemInit、SystemCoreClock 和 RCC 时钟表，并选择内部 8MHz HSI。
 * 本封装启用期间，系统文件只能编译一份，避免重复符号。
 * 使用内部时钟可让原理图上的 PF0/PF1 继续承担显示接口功能。 */
#include "system_py32f0xx.c"
