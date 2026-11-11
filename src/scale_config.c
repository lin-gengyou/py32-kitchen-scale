/* 两页轮换保存。新页完成写入并通过版本、字段与 CRC 校验后才成为有效记录。 */
#include "scale_config.h"
#include "firmware_version.h"
#include "py32f0xx_hal.h"

#include <stddef.h>
#include <string.h>

/* 链接器预留最后 4KB 扇区，代码与参数分开，上传时使用扇区擦除。 */
#ifndef SCALE_CONFIG_BASE
#define SCALE_CONFIG_BASE ((uintptr_t)0x08004000U)
#endif
#define CONFIG_PAGE_SIZE 128U
#define CONFIG_MAGIC 0x53434C35U
#define CONFIG_VERSION 2U
#define FIRMWARE_VERSION SCALE_FIRMWARE_VERSION_CODE

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t firmware_version;
    ScaleConfig config;
    uint32_t crc;
} ConfigRecord;

_Static_assert(sizeof(ConfigRecord) <= CONFIG_PAGE_SIZE, "Configuration exceeds a Flash page");

/* 软件 CRC32，覆盖记录头与配置，检测掉电造成的半页写入和数据损坏。 */
static uint32_t config_crc(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

/* 未找到有效记录时采用明确默认值，等待空秤确认和标定。 */
void scale_config_defaults(ScaleConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->stable_counts = 64;
    config->auto_off_seconds = 120;
    config->display_interval_ms = 200;
    strcpy(config->device_id, "UNSET");
}

/* CRC 校验之外检查物理范围与标定单调性，避免非法系数进入除法换算。 */
bool scale_config_valid(const ScaleConfig *config)
{
    if (config->display_interval_ms < 100U || config->display_interval_ms > 200U ||
        config->point_count > 3U || config->stable_counts == 0U ||
        config->stable_counts > 1000000U || config->auto_off_seconds > 86400U ||
        config->buzzer_mode > 2U || config->unit > 1U ||
        config->zero_counts < -8388608 || config->zero_counts > 8388607 ||
        config->device_id[0] == '\0' || memchr(config->device_id, '\0', sizeof(config->device_id)) == NULL) {
        return false;
    }
    int64_t previous_span = 0;
    uint32_t previous_grams = 0;
    int direction = config->spans[0] < 0 ? -1 : 1;
    for (unsigned i = 0; i < config->point_count; ++i) {
        int64_t span = (int64_t)config->spans[i] * direction;
        if (span - previous_span < 64 || span > 16777215 ||
            config->grams[i] <= previous_grams || config->grams[i] > 5000U) {
            return false;
        }
        previous_span = span;
        previous_grams = config->grams[i];
    }
    return true;
}

/* V1 的字段前缀原样保留：升级后恢复旧标定，并给新增刷新周期填 200ms。 */
static bool record_read(const ConfigRecord *record, ScaleConfig *result)
{
    if (record->magic != CONFIG_MAGIC) { return false; }
    ScaleConfig candidate;
    if (record->version == 1U) {
        const size_t prefix = offsetof(ScaleConfig, display_interval_ms);
        const size_t length = offsetof(ConfigRecord, config) + prefix;
        uint32_t stored_crc;
        memcpy(&stored_crc, (const uint8_t *)record + length, sizeof(stored_crc));
        if (stored_crc != config_crc(record, length)) { return false; }
        memset(&candidate, 0, sizeof(candidate));
        memcpy(&candidate, &record->config, prefix);
        candidate.display_interval_ms = 200;
    } else if (record->version == CONFIG_VERSION &&
               record->crc == config_crc(record, offsetof(ConfigRecord, crc))) {
        candidate = record->config;
    } else { return false; }
    if (!scale_config_valid(&candidate)) { return false; }
    if (result != NULL) { *result = candidate; }
    return true;
}

static bool record_valid(const ConfigRecord *record) { return record_read(record, NULL); }

/* 返回最新有效页编号，-1 表示两页均无有效记录。代数比较支持序号回绕。 */
static int current_slot(void)
{
    const ConfigRecord *first = (const ConfigRecord *)SCALE_CONFIG_BASE;
    const ConfigRecord *second = (const ConfigRecord *)(SCALE_CONFIG_BASE + CONFIG_PAGE_SIZE);
    bool first_valid = record_valid(first);
    bool second_valid = record_valid(second);
    if (!first_valid) {
        return second_valid ? 1 : -1;
    }
    uint32_t difference = second->generation - first->generation;
    return second_valid && difference != 0U && difference < 0x80000000U ? 1 : 0;
}

/* 上电恢复最新记录，坏页自动回退到另一份；没有记录时恢复默认值。 */
bool scale_config_load(ScaleConfig *config)
{
    int slot = current_slot();
    if (slot < 0) {
        scale_config_defaults(config);
        return false;
    }
    const ConfigRecord *record = (const ConfigRecord *)(SCALE_CONFIG_BASE + (unsigned)slot * CONFIG_PAGE_SIZE);
    return record_read(record, config);
}

/* 先擦写备用页，始终保留当前有效页。重复保存相同配置时跳过擦写。 */
bool scale_config_save(const ScaleConfig *config)
{
    if (!scale_config_valid(config)) {
        return false;
    }
    int active = current_slot();
    uint32_t generation = 1;
    if (active >= 0) {
        const ConfigRecord *old = (const ConfigRecord *)(SCALE_CONFIG_BASE + (unsigned)active * CONFIG_PAGE_SIZE);
        ScaleConfig restored;
        if (record_read(old, &restored) && memcmp(&restored, config, sizeof(*config)) == 0) {
            return true;
        }
        generation = old->generation + 1U;
    }
    unsigned target = active == 0 ? 1U : 0U;
    uintptr_t address = SCALE_CONFIG_BASE + target * CONFIG_PAGE_SIZE;
    uint32_t page[CONFIG_PAGE_SIZE / sizeof(uint32_t)];
    memset(page, 0xFF, sizeof(page));
    ConfigRecord record = {
        .magic = CONFIG_MAGIC,
        .version = CONFIG_VERSION,
        .generation = generation,
        .firmware_version = FIRMWARE_VERSION,
        .config = *config,
    };
    record.crc = config_crc(&record, offsetof(ConfigRecord, crc));
    memcpy(page, &record, sizeof(record));

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGEERASE,
        .PageAddress = address,
        .NbPages = 1,
    };
    uint32_t page_error;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &page_error);
    if (status == HAL_OK) {
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_PAGE, address, page);
    }
    /* 固定版本 SDK 的编程错误出口可能保留软件锁，串行调用结束后恢复该锁。 */
    pFlash.Lock = HAL_UNLOCKED;
    HAL_FLASH_Lock();
    /* 以读回的完整记录为提交结果，兼容数据已写完但驱动随后报告错误的情况。 */
    return record_valid((const ConfigRecord *)address) &&
           memcmp((const void *)address, &record, sizeof(record)) == 0;
}
