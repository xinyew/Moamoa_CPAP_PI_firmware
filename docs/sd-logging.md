# SD Logging & Wall-Clock Time

## Concept

The microSD card is a third transport: the exact stream frames
(DATA/STATUS, see `ble-protocol.md`) are appended to files, plus
SD-only TSYNC records that map device uptime to real wall-clock time.
One parser handles BLE, RTT and card data.

Log-by-default: whenever a FAT-formatted card is present, logging runs.
No arming step. `'P'`-off / mask-absent standby stop DATA frames but
STATUS (1 Hz) keeps recording, so the card shows *why* there is no data.

## On-card layout

```
/LOG/INDEX.TXT        one line per file, append-only (used by BLE fetch)
/LOG/B007S001.BIN     boot 7, file 1
/LOG/B007S002.BIN     boot 7, file 2 (rotated)
```

- 8.3 names: `BnnnSmmm.BIN`, boot counter `nnn` = (max on card % 999)+1
  discovered by directory scan once per power cycle; `mmm` = file index.
- Rotation: 8 MB or 15 min, whichever first. fsync every 2 s (bounded
  power-loss window). When free space < 64 MB the oldest file (by
  boot/file age) is deleted — circular storage.
- The ACTIVE file cannot be fetched over BLE (open for write); reboot
  or wait for rotation. Data up to the last fsync survives any cut.

## Write path

`sensor thread → 32 KB lock-free SPSC ring → sd_writer thread (prio 9)
→ fs_write in ≥4 KB batches`. Consumer SD cards stall up to ~500 ms
internally; the ring absorbs ~6 s at the ~5.2 kB/s data rate. Overflow
is counted (log warning) and shows up as seq gaps in the file — never
silent. Card removal mid-write → teardown → re-probe every 10 s.

## Wall-clock system

- Any client writes `'T'` + u64 unix-epoch-ms to NUS RX (portal does it
  on every connect; `cpap_ctl.py --sync`).
- Firmware stores `offset = epoch − uptime` and from then on writes a
  16 B TSYNC record (uptime, epoch) every second.
- **Uptime is monotonic → one sync anywhere retroactively timestamps
  the entire boot**, including hours logged before the first connect.
- No crystal RTC: the RC-calibrated clock drifts ~0.1 %. Readers fit a
  line through all TSYNC pairs of a boot, correcting drift; a boot that
  never saw a sync stays relative-time (readers must show that state).
- Capacity: ~5.2 kB/s → 18.5 MB/h → ~70 days continuous on 32 GB.

## Reading the card

1. **Doctors**: `tools/dist/CPAP_PI_SD_Reader.exe` — insert card, run,
   pick a recording + time range, plot / export CSV / Excel. Zero
   config. Rebuild with `tools/make_exe.bat`
   (pyinstaller + matplotlib + openpyxl); verify any build with
   `CPAP_PI_SD_Reader.exe --selftest [file.bin]`.
2. **Developers**: `tools/sd_reader.py` is also an importable parser
   (`load_folder()`, `Session.to_epoch_ms()`, `export_csv()`).
3. **Over BLE**: `python scripts/cpap_ctl.py --fetch INDEX` then
   `--fetch B007S001.BIN` (~18 KiB/s; fine for a session, pull the
   card for bulk).

## Firmware gotchas encountered (do not re-learn these)

- `CONFIG_FS_FATFS_REENTRANT=y` is mandatory: the writer thread and
  MCUmgr FS downloads share the volume.
- `CONFIG_MCUMGR_GRP_FS_MAX_FILE_SIZE_4GB` — the default profile
  assumes ≤64 KB files.
- fs_mgmt reads its chunk into a **stack** buffer on the MCUmgr
  workqueue: chunk capped at 4 KB and
  `CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE=8192`, else MPU fault.
