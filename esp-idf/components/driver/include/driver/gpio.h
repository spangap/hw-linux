/**
 * driver/gpio.h — the pins of a station that has none.
 *
 * A pin here is a row in a table: a level, an enabled flag, an interrupt type
 * and a handler. Everything that drives a pin on the chip drives a row
 * instead, and the one rule that makes the interrupt path behave as hardware
 * does is in gpio_shim.c: a pin whose interrupt is enabled and whose trigger
 * matches fires its handler at once, on the calling task.
 *
 * The pin numbering and the mode, pull and trigger enumerations are ESP-IDF's
 * own — the host target ships them in hal/gpio_types.h — so only the driver
 * surface itself is declared here, and only the part the firmware uses. Pin
 * numbers are indices into the table and carry no other meaning.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t        pin_bit_mask;
    gpio_mode_t     mode;
    gpio_pullup_t   pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void* arg);

esp_err_t gpio_config(const gpio_config_t* cfg);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int       gpio_get_level(gpio_num_t pin);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void* arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin);
esp_err_t gpio_sleep_sel_en(gpio_num_t pin);
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_wakeup_disable(gpio_num_t pin);

/**
 * Drive a pin from the other side — what a modelled peripheral uses to raise
 * its own interrupt line. Identical to gpio_set_level, named apart so the
 * firmware's own writes and a model's stay legible.
 */
void gpio_shim_set_level(int pin, uint32_t level);

#ifdef __cplusplus
}
#endif
