/*
 * SD logging service — stores the exact v2 stream frames (DATA /
 * STATUS / TSYNC, see comm_protocol.h) on a FAT microSD card, so the
 * card is simply a third transport with one shared parser.
 *
 * Architecture (caterpillar flash-logging lessons):
 *   sensor thread --memcpy--> 32 KB SPSC ring --writer thread--> FAT
 * SD writes NEVER run in the sensor tick: consumer cards stall
 * 100-500 ms on internal wear leveling; at ~5.2 kB/s the ring rides
 * out ~6 s of stall. Real overflow is counted and visible as seq
 * gaps in the file.
 *
 * Files: /SD:/LOG/BnnnSmmm.BIN (boot counter from directory scan,
 * file index within boot), rotated every 8 MB or 15 min, fsync every
 * 2 s (bounded power-loss window), INDEX.TXT appended per file (for
 * BLE fetch via MCUmgr FS). Low space -> delete oldest file.
 */

#include "sd_logger.h"

#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>
#include <ff.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(sd_logger, LOG_LEVEL_INF);

#define MNT            "/SD:"
#define LOG_DIR        MNT "/LOG"
#define INDEX_PATH     LOG_DIR "/INDEX.TXT"

#define RING_SIZE      32768
#define BATCH_SIZE     4096
#define FSYNC_MS       2000
#define ROTATE_BYTES   (8 * 1024 * 1024)
#define ROTATE_MS      (15 * 60 * 1000)
#define RETRY_MS       10000
#define MIN_FREE_KB    (64 * 1024)   /* delete oldest below 64 MB free */

/* --- SPSC ring: producer = sensor thread, consumer = writer ------- */

static uint8_t ring[RING_SIZE];
static atomic_t ring_head;  /* total bytes produced */
static atomic_t ring_tail;  /* total bytes consumed */
static uint32_t ring_dropped;

static struct k_sem data_sem;

/* --- FS state (writer thread only) -------------------------------- */

static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = MNT,
};

static struct fs_file_t logf;
static bool mounted;
static bool file_open;
static uint32_t boot_num;      /* discovered once per power cycle */
static bool boot_num_known;
static uint32_t file_num;
static uint32_t file_bytes;
static int64_t file_opened_at;
static int64_t last_fsync;
static atomic_t active_flag;

void sd_logger_write(const uint8_t *frame, uint16_t len)
{
    uint32_t head = (uint32_t)atomic_get(&ring_head);
    uint32_t tail = (uint32_t)atomic_get(&ring_tail);
    uint32_t free_b = RING_SIZE - (head - tail);

    if (!atomic_get(&active_flag)) {
        return;  /* no card — don't fill the ring pointlessly */
    }
    if (len > free_b) {
        ring_dropped += len;
        return;
    }

    uint32_t off = head % RING_SIZE;
    uint32_t first = MIN((uint32_t)len, RING_SIZE - off);

    memcpy(&ring[off], frame, first);
    if (first < len) {
        memcpy(&ring[0], frame + first, len - first);
    }
    atomic_set(&ring_head, (atomic_val_t)(head + len));
    k_sem_give(&data_sem);
}

bool sd_logger_active(void)
{
    return atomic_get(&active_flag) != 0;
}

/* --- helpers (writer thread) -------------------------------------- */

static void scan_boot_number(void)
{
    struct fs_dir_t dir;
    struct fs_dirent ent;
    uint32_t max_boot = 0;

    fs_dir_t_init(&dir);
    if (fs_opendir(&dir, LOG_DIR) != 0) {
        boot_num = 1;
        boot_num_known = true;
        return;
    }
    while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
        unsigned int b, s;

        if (sscanf(ent.name, "B%3uS%3u.BIN", &b, &s) == 2 && b > max_boot) {
            max_boot = b;
        }
    }
    fs_closedir(&dir);
    boot_num = (max_boot % 999) + 1;
    boot_num_known = true;
}

static int delete_oldest(void)
{
    struct fs_dir_t dir;
    struct fs_dirent ent;
    char victim[16] = "";
    uint32_t best = UINT32_MAX;

    fs_dir_t_init(&dir);
    if (fs_opendir(&dir, LOG_DIR) != 0) {
        return -EIO;
    }
    while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
        unsigned int b, s;

        if (sscanf(ent.name, "B%3uS%3u.BIN", &b, &s) == 2) {
            /* age key: distance behind the current boot (mod wrap) */
            uint32_t age = ((boot_num - b) % 999) * 1000 + s;

            if (b == boot_num && s >= file_num) {
                continue;  /* never the file we're about to write */
            }
            if (age < best) {
                best = age;
                strncpy(victim, ent.name, sizeof(victim) - 1);
            }
        }
    }
    fs_closedir(&dir);

    if (victim[0] == '\0') {
        return -ENOENT;
    }

    char path[40];

    snprintf(path, sizeof(path), LOG_DIR "/%s", victim);
    LOG_WRN("low space: deleting %s", path);
    return fs_unlink(path);
}

static void ensure_free_space(void)
{
    struct fs_statvfs st;

    if (fs_statvfs(MNT, &st) != 0) {
        return;
    }
    while ((uint64_t)st.f_bfree * st.f_frsize / 1024 < MIN_FREE_KB) {
        if (delete_oldest() != 0) {
            break;
        }
        if (fs_statvfs(MNT, &st) != 0) {
            break;
        }
    }
}

static int open_next_file(void)
{
    char path[40];

    ensure_free_space();
    file_num++;
    snprintf(path, sizeof(path), LOG_DIR "/B%03uS%03u.BIN",
             boot_num, file_num);

    fs_file_t_init(&logf);
    int ret = fs_open(&logf, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);

    if (ret != 0) {
        LOG_ERR("open %s failed: %d", path, ret);
        return ret;
    }

    /* Append to INDEX.TXT so BLE clients can discover files */
    struct fs_file_t idx;

    fs_file_t_init(&idx);
    if (fs_open(&idx, INDEX_PATH,
                FS_O_CREATE | FS_O_WRITE | FS_O_APPEND) == 0) {
        char line[20];
        int n = snprintf(line, sizeof(line), "B%03uS%03u.BIN\n",
                         boot_num, file_num);

        fs_write(&idx, line, n);
        fs_close(&idx);
    }

    file_bytes = 0;
    file_opened_at = k_uptime_get();
    last_fsync = file_opened_at;
    file_open = true;
    LOG_INF("logging to %s", path);
    return 0;
}

static void close_file(void)
{
    if (file_open) {
        fs_close(&logf);
        file_open = false;
    }
}

static void teardown(void)
{
    close_file();
    if (mounted) {
        fs_unmount(&mp);
        mounted = false;
    }
    atomic_set(&active_flag, 0);
}

static bool try_bring_up(void)
{
    if (disk_access_init("SD") != 0) {
        return false;
    }
    if (fs_mount(&mp) != 0) {
        LOG_WRN("FAT mount failed (card unformatted?)");
        return false;
    }
    mounted = true;
    fs_mkdir(LOG_DIR);  /* EEXIST is fine */
    if (!boot_num_known) {
        scan_boot_number();
        LOG_INF("boot #%u on this card", boot_num);
    }
    if (open_next_file() != 0) {
        teardown();
        return false;
    }
    /* Start clean: drop whatever accumulated while absent */
    atomic_set(&ring_tail, atomic_get(&ring_head));
    atomic_set(&active_flag, 1);
    return true;
}

static int flush_batch(void)
{
    static uint8_t batch[BATCH_SIZE];
    uint32_t head = (uint32_t)atomic_get(&ring_head);
    uint32_t tail = (uint32_t)atomic_get(&ring_tail);
    uint32_t avail = head - tail;

    if (avail == 0) {
        return 0;
    }
    uint32_t n = MIN(avail, (uint32_t)BATCH_SIZE);
    uint32_t off = tail % RING_SIZE;
    uint32_t first = MIN(n, RING_SIZE - off);

    memcpy(batch, &ring[off], first);
    if (first < n) {
        memcpy(batch + first, &ring[0], n - first);
    }

    ssize_t written = fs_write(&logf, batch, n);

    if (written < 0 || (uint32_t)written != n) {
        LOG_ERR("fs_write failed: %d", (int)written);
        return -EIO;
    }
    atomic_set(&ring_tail, (atomic_val_t)(tail + n));
    file_bytes += n;
    return (int)n;
}

static void writer_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    k_sem_init(&data_sem, 0, 1);

    while (1) {
        if (!atomic_get(&active_flag)) {
            if (!try_bring_up()) {
                k_msleep(RETRY_MS);
            }
            continue;
        }

        k_sem_take(&data_sem, K_MSEC(500));

        int ret = flush_batch();

        if (ret < 0) {
            LOG_WRN("card write error — assuming removal, will re-probe");
            teardown();
            continue;
        }

        int64_t now = k_uptime_get();

        if (file_open && now - last_fsync >= FSYNC_MS) {
            if (fs_sync(&logf) != 0) {
                teardown();
                continue;
            }
            last_fsync = now;
            if (ring_dropped != 0) {
                LOG_WRN("ring overflow: %u bytes dropped total",
                        ring_dropped);
            }
        }

        if (file_open && (file_bytes >= ROTATE_BYTES ||
                          now - file_opened_at >= ROTATE_MS)) {
            close_file();
            if (open_next_file() != 0) {
                teardown();
            }
        }
    }
}

K_THREAD_DEFINE(sd_writer_thread, 3072, writer_thread_fn, NULL, NULL, NULL,
                K_PRIO_PREEMPT(9), 0, K_TICKS_FOREVER);

void sd_logger_start(void)
{
    k_thread_start(sd_writer_thread);
}
