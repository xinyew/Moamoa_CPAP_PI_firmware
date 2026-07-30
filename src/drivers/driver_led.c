/*
 * Debug LED driver — four active-low LEDs (P0.19, P0.21, P0.20, P0.22).
 */

#include "driver_led.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led, LOG_LEVEL_INF);

static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
};

int drv_led_init(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        if (!gpio_is_ready_dt(&leds[i])) {
            LOG_ERR("LED%u GPIO not ready", (unsigned)(i + 1));
            return -ENODEV;
        }

        int ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_ERR("LED%u configure failed: %d", (unsigned)(i + 1), ret);
            return ret;
        }
    }

    return 0;
}

int drv_led_set(uint8_t idx, bool on)
{
    if (idx >= ARRAY_SIZE(leds)) {
        return -EINVAL;
    }
    return gpio_pin_set_dt(&leds[idx], on ? 1 : 0);
}

int drv_led_toggle(uint8_t idx)
{
    if (idx >= ARRAY_SIZE(leds)) {
        return -EINVAL;
    }
    return gpio_pin_toggle_dt(&leds[idx]);
}
