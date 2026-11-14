#ifndef SCALE_CONFIG_H
#define SCALE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* 标定与用户设置。皮重和运行时零点跟踪不写入 Flash。 */
typedef struct {
    int32_t zero_counts;
    int32_t spans[3];
    uint32_t grams[3];
    uint32_t point_count;
    uint32_t stable_counts;      /* 0自动0.2g；正数为手动窗口峰峰计数。 */
    uint32_t auto_off_seconds;
    uint32_t buzzer_mode;
    uint32_t unit;               /* 0 为 g，1 为 kg；由板上按键切换。 */
    uint32_t calibration_date;   /* YYYYMMDD，0 表示尚未录入。 */
    char device_id[16];
    uint32_t display_interval_ms; /* 段码屏刷新周期，100～200ms。 */
    uint32_t battery_profile;     /* 0外部电源，1单锂电，2双碱性，3双镍氢。 */
    uint32_t battery_reference_mv; /* 内部参考标称1200mV，可按电压实测校准。 */
    uint32_t lcd_missing_mask;    /* 0完整屏，4跳过已知故障的数值第三位。 */
    uint32_t bluetooth_enabled;   /* ECB01C串口透传启用，所有操作仍可本机完成。 */
} ScaleConfig;

void scale_config_defaults(ScaleConfig *config);
bool scale_config_valid(const ScaleConfig *config);
bool scale_config_load(ScaleConfig *config);
bool scale_config_save(const ScaleConfig *config);

#endif
