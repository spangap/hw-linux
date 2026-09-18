/**
 * gpio_shim.c — the pin table. See driver/gpio.h.
 *
 * The table exists for one behaviour: the interrupt path. On the chip a
 * level-triggered pin whose interrupt is enabled while its line is high fires
 * immediately, which is what lets a driver disable the interrupt, drain
 * whatever raised it, and re-enable — and be re-entered at once if the line
 * never went low. The shim reproduces exactly that, so the driver's interrupt
 * handling is the same code on the same edges as it is on hardware.
 *
 * A handler runs on the task that moved the line, which is what an interrupt
 * does. The pin table is read and written under a critical section; the
 * handler is called outside it, because a handler ends in a yield and a yield
 * belongs outside a section that has the tick masked.
 */
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint8_t         level;
    uint8_t         intr_enabled;
    gpio_int_type_t intr_type;
    gpio_isr_t      handler;
    void*           arg;
} pin_t;

static pin_t s_pins[GPIO_NUM_MAX];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static inline bool pin_valid(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

/* Does this pin's armed trigger match the transition just made? `changed` is
 * false when the level was merely re-asserted, which an edge trigger ignores
 * and a level trigger does not. */
static bool triggers(const pin_t* p, bool changed)
{
    if (!p->intr_enabled || !p->handler) return false;
    switch (p->intr_type) {
        case GPIO_INTR_POSEDGE:    return changed && p->level;
        case GPIO_INTR_NEGEDGE:    return changed && !p->level;
        case GPIO_INTR_ANYEDGE:    return changed;
        case GPIO_INTR_HIGH_LEVEL: return p->level != 0;
        case GPIO_INTR_LOW_LEVEL:  return p->level == 0;
        default:                   return false;
    }
}

/* Set a level and answer the handler it fired, if any. The caller invokes it
 * with nothing held. */
static void set_level(int pin, uint32_t level)
{
    gpio_isr_t fire = NULL;
    void*      arg  = NULL;

    if (!pin_valid(pin)) return;

    portENTER_CRITICAL(&mux);
    {
        pin_t* p = &s_pins[pin];
        bool changed = (p->level != (level ? 1 : 0));
        p->level = level ? 1 : 0;
        if (triggers(p, changed)) { fire = p->handler; arg = p->arg; }
    }
    portEXIT_CRITICAL(&mux);

    if (fire) fire(arg);
}

esp_err_t gpio_config(const gpio_config_t* cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if (!(cfg->pin_bit_mask & (1ULL << pin))) continue;
        portENTER_CRITICAL(&mux);
        s_pins[pin].intr_type = cfg->intr_type;
        if (cfg->intr_type == GPIO_INTR_DISABLE) s_pins[pin].intr_enabled = 0;
        portEXIT_CRITICAL(&mux);
    }
    return ESP_OK;
}

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode)
{
    (void)mode;
    return pin_valid(pin) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (!pin_valid(pin)) return ESP_ERR_INVALID_ARG;
    set_level(pin, level);
    return ESP_OK;
}

void gpio_shim_set_level(int pin, uint32_t level)
{
    set_level(pin, level);
}

int gpio_get_level(gpio_num_t pin)
{
    return pin_valid(pin) ? s_pins[pin].level : 0;
}

esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type)
{
    if (!pin_valid(pin)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&mux);
    s_pins[pin].intr_type = type;
    if (type == GPIO_INTR_DISABLE) s_pins[pin].intr_enabled = 0;
    portEXIT_CRITICAL(&mux);
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int intr_alloc_flags)
{
    (void)intr_alloc_flags;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void* arg)
{
    if (!pin_valid(pin)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&mux);
    s_pins[pin].handler = handler;
    s_pins[pin].arg     = arg;
    portEXIT_CRITICAL(&mux);
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{
    if (!pin_valid(pin)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&mux);
    s_pins[pin].handler      = NULL;
    s_pins[pin].arg          = NULL;
    s_pins[pin].intr_enabled = 0;
    portEXIT_CRITICAL(&mux);
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t pin)
{
    gpio_isr_t fire = NULL;
    void*      arg  = NULL;

    if (!pin_valid(pin)) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&mux);
    {
        pin_t* p = &s_pins[pin];
        p->intr_enabled = 1;
        /* A line that is already asserted under a level trigger is an
         * interrupt that is already pending. */
        if (triggers(p, false)) { fire = p->handler; arg = p->arg; }
    }
    portEXIT_CRITICAL(&mux);

    if (fire) fire(arg);
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t pin)
{
    if (!pin_valid(pin)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&mux);
    s_pins[pin].intr_enabled = 0;
    portEXIT_CRITICAL(&mux);
    return ESP_OK;
}

esp_err_t gpio_sleep_sel_dis(gpio_num_t pin) { return pin_valid(pin) ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t gpio_sleep_sel_en(gpio_num_t pin)  { return pin_valid(pin) ? ESP_OK : ESP_ERR_INVALID_ARG; }

esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type)
{
    (void)type;
    return pin_valid(pin) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t gpio_wakeup_disable(gpio_num_t pin)
{
    return pin_valid(pin) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
