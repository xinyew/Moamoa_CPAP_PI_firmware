/*
 * RTT wired data stream — binary frames on SEGGER RTT up-buffer 1.
 *
 * Buffer 0 stays the printk/log console. NO_BLOCK_SKIP mode drops
 * frames when nothing is reading, so this costs a memcpy at most and
 * nothing at all in the field (no probe attached).
 */

#include "rtt_stream.h"

#include <SEGGER_RTT.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rtt_stream, LOG_LEVEL_INF);

#define RTT_DATA_CHANNEL  1
#define RTT_DATA_BUF_SIZE 8192

static uint8_t rtt_data_buf[RTT_DATA_BUF_SIZE];

int rtt_stream_init(void)
{
    int ret = SEGGER_RTT_ConfigUpBuffer(RTT_DATA_CHANNEL, "data",
                                        rtt_data_buf, sizeof(rtt_data_buf),
                                        SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    if (ret < 0) {
        LOG_ERR("RTT data channel config failed: %d", ret);
        return ret;
    }

    LOG_INF("RTT data stream on up-buffer %d (%u B)", RTT_DATA_CHANNEL,
            (unsigned)sizeof(rtt_data_buf));
    return 0;
}

void rtt_stream_write(const uint8_t *data, uint16_t len)
{
    SEGGER_RTT_Write(RTT_DATA_CHANNEL, data, len);
}
