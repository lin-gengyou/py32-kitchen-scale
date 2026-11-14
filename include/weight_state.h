#ifndef WEIGHT_STATE_H
#define WEIGHT_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WEIGHT_WAIT,                 /* 等待新鲜采样。 */
    WEIGHT_UNCALIBRATED,         /* 尚未标定。 */
    WEIGHT_OK,                   /* 正常重量。 */
    WEIGHT_RANGE,                /* 超出有效量程或数值范围。 */
    WEIGHT_OVERLOAD,             /* 达到 OL 门限。 */
    WEIGHT_SENSOR_ERROR,         /* 采集故障。 */
    WEIGHT_LOW_BATTERY,          /* 电池电压低，当前重量不作为正常测量发布。 */
    WEIGHT_POWER_ERROR,          /* 电池模式下ADC采样故障。 */
    WEIGHT_CALIBRATING,          /* 本机标定向导进行中，不发布普通重量。 */
    WEIGHT_SLEEP,                /* 软件待机。 */
} WeightStatus;

typedef enum {
    MOTION_EMPTY,                /* 已知空秤基线附近，持续稳定。 */
    MOTION_LOADING,              /* 动态加载。 */
    MOTION_CANDIDATE,            /* 正在积累稳定窗口。 */
    MOTION_STABLE,               /* 稳定称重。 */
    MOTION_UNLOADING,            /* 重量下降。 */
    MOTION_FAULT,                /* 故障。 */
    MOTION_SLEEP,                /* 待机。 */
} WeightMotion;

/* 屏幕与蓝牙共享的状态，内部重量保留毫克，最终发布时才按分段分度量化。 */
typedef struct {
    int32_t net_mg;              /* 未应用显示分度的净重。 */
    int32_t gross_mg;            /* 包含容器的毛重。 */
    int32_t tare_mg;             /* 皮重。 */
    int32_t display_mg;          /* 最后取整后的显示重量。 */
    uint32_t sequence;           /* 发送重量帧的序号。 */
    uint32_t unit;               /* 0 为 g，1 为 kg，内部值始终以 mg 表示。 */
    uint16_t division_mg;        /* 当前分度，100/500/1000mg。 */
    int8_t battery_percent;      /* -1 为未知，取得真实电池换算参数后再填入。 */
    bool stable;
    WeightStatus status;
    WeightMotion motion;
} WeightState;

extern WeightState weight_state;

#endif
