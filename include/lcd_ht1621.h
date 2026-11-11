#ifndef SCALE_LCD_HT1621_H
#define SCALE_LCD_HT1621_H
#include <stdbool.h>
#include <stdint.h>

/* 板载 HT1621B：PB2=CS，PA5=WR，PA7=DATA，PF1=RD（写操作时保持高）。 */
void lcd_init(void);
void lcd_power(bool enabled);
void lcd_clear(void);
void lcd_text(const char *title, const char *value, uint8_t decimal_mask);
void lcd_number(const char *title, int32_t value, unsigned decimals);
void lcd_weight(int32_t milligrams, bool kilograms, bool stable, bool tare);
void lcd_test_all(void);
void lcd_flush(void);

#ifdef SCALE_HOST_TEST
const uint8_t *lcd_test_frame(void);
#endif
#endif
