/*
 * Throughput test sink — same wire contract as the caterpillar
 * project's 0xFFE7 characteristic so the tooling is shared:
 *
 *   service 0xFFE0, characteristic 0xFFE7
 *   - writes (with or without response) are counted and discarded
 *   - reads return the u32 LE byte counter
 *
 * Deliberately dumb: any BLE client can measure the real link
 * throughput with no protocol knowledge.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>

static uint32_t tput_rx_bytes;

static ssize_t on_tput_write(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(buf);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    tput_rx_bytes += len;
    return len;
}

static ssize_t on_tput_read(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    uint8_t le[4];

    sys_put_le32(tput_rx_bytes, le);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, le, sizeof(le));
}

BT_GATT_SERVICE_DEFINE(tput_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xFFE0)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0xFFE7),
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP |
            BT_GATT_CHRC_READ,
        BT_GATT_PERM_WRITE | BT_GATT_PERM_READ,
        on_tput_read, on_tput_write, NULL),
);
