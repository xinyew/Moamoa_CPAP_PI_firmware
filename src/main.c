/*
 * KMM PMask Control — PPG + Pressure + Temp/Humidity Sensing Firmware
 * Custom nRF52840 board (kmm_pmask_control/nrf52840, Raytac MDBT50Q-P1MV2)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>

#include "drivers/bus_diag.h"
#include "drivers/driver_led.h"
#include "sensors/sensor_manager.h"
#include "comm/comm_manager.h"

LOG_MODULE_REGISTER(kmm_pmask_main, LOG_LEVEL_DBG);

int main(void)
{
    printk("\n=== KMM PMask Control Boot ===\n");

    /* Debug LEDs (P0.19/21/20/22, active low) */
    if (drv_led_init() < 0) {
        LOG_ERR("Failed to init LEDs");
    }

    /* Mask bus: PCA9517A repeater ON + presence pins, then scan the
     * TCA9546A channels for the four sensor clusters, then the SD card.
     */
    int mask_present = bus_diag_prepare_mask_bus();
    int found = bus_diag_scan_mux();
    int sd_ok = bus_diag_sd_check();

    printk("Bus diag: mask %s, %d/14 sensors, SD %s\n",
           mask_present == 1 ? "attached" : "NOT ATTACHED",
           found, sd_ok == 0 ? "OK" : "absent/fail");
    g_sensor_data.sd_ok = (sd_ok == 0);

    /* BLE: advertising only for now (data protocol redesign for the
     * 4-site topology pending — see comm_manager.c)
     */
    if (comm_manager_init() < 0) {
        LOG_ERR("Failed to init comm layer");
    }

    /* Sampling thread: PPG x4 + baro x4 @100 Hz, SHT/TMP/batt @1 Hz */
    sensor_manager_start();

    while (1) {
        drv_led_toggle(LED_1);  /* heartbeat on DEBUG_LED0 */
        k_msleep(500);
    }
}
