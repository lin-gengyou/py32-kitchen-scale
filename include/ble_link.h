#ifndef SCALE_BLE_LINK_H
#define SCALE_BLE_LINK_H
#include <stdbool.h>
#include <stdint.h>
#include "weight_state.h"

void ble_configure(bool enabled);
void ble_poll(uint32_t now, const WeightState *weight, uint32_t period_ms);
void ble_uart_irq(void);
bool ble_link_active(uint32_t now);
/* 由称重应用实现，远端与实体键复用同一套去皮/唤醒检查。 */
const char *scale_remote_tare(uint32_t now);
void scale_remote_wake(uint32_t now);
#ifdef SCALE_HOST_TEST
void ble_test_receive(const char *text, uint32_t now);
int ble_test_next_tx(void);
#endif
#endif
