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
    uint32_t stable_counts;
    uint32_t auto_off_seconds;
    uint32_t buzzer_mode;
    uint32_t unit;               /* 0 为 g，1 为 kg。蓝牙重量帧统一以 g 传输。 */
    uint32_t calibration_date;   /* YYYYMMDD，0 表示尚未录入。 */
    char device_id[16];
} ScaleConfig;

void scale_config_defaults(ScaleConfig *config);
bool scale_config_valid(const ScaleConfig *config);
bool scale_config_load(ScaleConfig *config);
bool scale_config_save(const ScaleConfig *config);

#endif
