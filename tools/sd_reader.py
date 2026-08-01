#!/usr/bin/env python3
"""
KMM PMask SD Card Reader — doctor-facing viewer/exporter.

Insert the device's microSD card into this computer, then open this
program. It finds the card automatically, shows which wall-clock time
ranges contain data, and lets you plot any range or export it to CSV
or Excel. No configuration needed.

Data format: the card stores the firmware's binary stream frames
(see src/comm/comm_protocol.h — DATA 0x11, STATUS 0x12, TSYNC 0x13).
TSYNC records map device uptime to real wall-clock time; with two or
more, clock drift is corrected by a linear fit.

Developer extras:
    python sd_reader.py --selftest [file.bin]   # parser/export check
"""

import os
import string
import struct
import sys
import tempfile
from datetime import datetime

MAGIC = 0xC9A5
LEN = {0x11: 204, 0x12: 45, 0x13: 16}
TICK_MS = 10

# Column model
PPG_COLS = [f"{c}{s}" for s in range(1, 5) for c in ("r", "i", "g")]
BARO_COLS = [f"p{b}" for b in range(1, 5)]
DATA_COLS = PPG_COLS + BARO_COLS
STATUS_COLS = ["sht1t", "sht1h", "sht2t", "sht2h", "sht3t", "sht3h",
               "tmp1", "tmp2", "tmp3", "vbat"]


def u24(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


class Session:
    """One boot's worth of frames (possibly several files)."""

    def __init__(self, boot):
        self.boot = boot
        self.files = []          # (file_index, path)
        self.samples = []        # (uptime_ms, {col: val}) at 100 Hz
        self.status = []         # (uptime_ms, {col: val}) at 1 Hz
        self.tsyncs = []         # (uptime_ms, epoch_ms)
        self._fit = None

    # -- wall-clock mapping -------------------------------------------------
    def _make_fit(self):
        ts = self.tsyncs
        if not ts:
            self._fit = (0.0, None)   # unsynced: relative time only
        elif len(ts) == 1:
            self._fit = (1.0, ts[0][1] - ts[0][0])
        else:
            n = len(ts)
            sx = sum(t[0] for t in ts); sy = sum(t[1] for t in ts)
            sxx = sum(t[0] * t[0] for t in ts)
            sxy = sum(t[0] * t[1] for t in ts)
            d = n * sxx - sx * sx
            if d == 0:
                self._fit = (1.0, ts[0][1] - ts[0][0])
            else:
                a = (n * sxy - sx * sy) / d
                b = (sy - a * sx) / n
                self._fit = (a, b)

    @property
    def synced(self):
        return bool(self.tsyncs)

    def to_epoch_ms(self, uptime_ms):
        if self._fit is None:
            self._make_fit()
        a, b = self._fit
        if b is None:
            return None
        return a * uptime_ms + b

    def time_range(self):
        """(start, end) as datetime (synced) or seconds floats (not)."""
        if not self.samples:
            return None, None
        u0, u1 = self.samples[0][0], self.samples[-1][0]
        if self.synced:
            return (datetime.fromtimestamp(self.to_epoch_ms(u0) / 1000),
                    datetime.fromtimestamp(self.to_epoch_ms(u1) / 1000))
        return u0 / 1000.0, u1 / 1000.0

    def label(self):
        t0, t1 = self.time_range()
        n = len(self.samples)
        dur = (self.samples[-1][0] - self.samples[0][0]) / 1000 if n else 0
        if self.synced:
            return (f"Recording {self.boot}:  "
                    f"{t0:%Y-%m-%d %H:%M:%S} - {t1:%H:%M:%S}"
                    f"  ({dur/60:.1f} min)")
        return (f"Recording {self.boot}:  no clock sync "
                f"(relative 0 - {dur/60:.1f} min)")


def parse_file(path, session):
    data = open(path, "rb").read()
    i = 0
    n = len(data)
    while i + 4 <= n:
        if struct.unpack_from("<H", data, i)[0] != MAGIC or \
                data[i + 2] not in LEN:
            i += 1
            continue
        t = data[i + 2]
        ln = LEN[t]
        if i + ln > n:
            break
        if t == 0x11:
            devt = struct.unpack_from("<I", data, i + 4)[0]
            nsamp = data[i + 9] or 4
            for k in range(nsamp):
                row = {}
                for s in range(4):
                    base = i + 12 + s * (nsamp * 9) + k * 9
                    row[f"r{s+1}"] = u24(data, base)
                    row[f"i{s+1}"] = u24(data, base + 3)
                    row[f"g{s+1}"] = u24(data, base + 6)
                bb = i + 12 + 144 + k * 12
                for b in range(4):
                    row[f"p{b+1}"] = u24(data, bb + b * 3) / 100.0
                up = devt - (nsamp - 1 - k) * TICK_MS
                session.samples.append((up, row))
        elif t == 0x12:
            devt = struct.unpack_from("<I", data, i + 4)[0]
            row = {}
            for j in range(3):
                row[f"sht{j+1}t"] = struct.unpack_from("<h", data,
                                                       i + 8 + 4 * j)[0] / 100
                row[f"sht{j+1}h"] = struct.unpack_from("<H", data,
                                                       i + 10 + 4 * j)[0] / 100
            for j in range(3):
                row[f"tmp{j+1}"] = struct.unpack_from("<h", data,
                                                      i + 20 + 2 * j)[0] / 100
            row["vbat"] = struct.unpack_from("<H", data, i + 34)[0]
            session.status.append((devt, row))
        elif t == 0x13:
            up = struct.unpack_from("<I", data, i + 4)[0]
            ep = struct.unpack_from("<Q", data, i + 8)[0]
            session.tsyncs.append((up, ep))
        i += ln


def load_folder(folder):
    """Parse every BnnnSmmm.BIN under folder -> [Session] newest first."""
    sessions = {}
    for name in sorted(os.listdir(folder)):
        up = name.upper()
        if not (up.endswith(".BIN") and up.startswith("B") and len(up) == 12):
            continue
        try:
            boot = int(up[1:4])
            fidx = int(up[5:8])
        except ValueError:
            continue
        sessions.setdefault(boot, Session(boot)).files.append(
            (fidx, os.path.join(folder, name)))
    out = []
    for boot in sorted(sessions, reverse=True):
        s = sessions[boot]
        for _, path in sorted(s.files):
            parse_file(path, s)
        s.samples.sort(key=lambda x: x[0])
        s.status.sort(key=lambda x: x[0])
        if s.samples:
            out.append(s)
    return out


def find_card():
    """Return the LOG folder of an inserted device card, if any."""
    for letter in string.ascii_uppercase:
        p = f"{letter}:\\LOG"
        try:
            if os.path.isdir(p) and any(
                    f.upper().endswith(".BIN") for f in os.listdir(p)):
                return p
        except OSError:
            continue
    return None


# --------------------------------------------------------------------------
#  Export
# --------------------------------------------------------------------------

def _rows_in_range(session, t0_ms, t1_ms):
    """Merge 100 Hz samples with latest 1 Hz status; yield dict rows."""
    si = 0
    cur_status = {}
    for up, row in session.samples:
        if up < t0_ms or up > t1_ms:
            continue
        while si < len(session.status) and session.status[si][0] <= up:
            cur_status = session.status[si][1]
            si += 1
        ep = session.to_epoch_ms(up)
        out = {"uptime_ms": up}
        out["wallclock"] = (datetime.fromtimestamp(ep / 1000)
                            .strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                            if ep is not None else "")
        out.update(row)
        out.update(cur_status)
        yield out


EXPORT_COLS = ["wallclock", "uptime_ms"] + DATA_COLS + STATUS_COLS


def export_csv(session, t0_ms, t1_ms, path):
    import csv

    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=EXPORT_COLS, extrasaction="ignore")
        w.writeheader()
        n = 0
        for row in _rows_in_range(session, t0_ms, t1_ms):
            w.writerow(row)
            n += 1
    return n


def export_xlsx(session, t0_ms, t1_ms, path):
    from openpyxl import Workbook

    wb = Workbook(write_only=True)
    ws = wb.create_sheet("KMM data")
    ws.append(EXPORT_COLS)
    n = 0
    for row in _rows_in_range(session, t0_ms, t1_ms):
        if n >= 1_000_000:
            break  # Excel row limit
        ws.append([row.get(c, "") for c in EXPORT_COLS])
        n += 1
    wb.save(path)
    return n


# --------------------------------------------------------------------------
#  GUI
# --------------------------------------------------------------------------

def run_gui():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    import matplotlib
    matplotlib.use("TkAgg")
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    from matplotlib.figure import Figure
    from matplotlib.dates import DateFormatter

    state = {"sessions": [], "folder": None}

    root = tk.Tk()
    root.title("KMM PMask — SD Card Reader")
    root.geometry("1150x760")

    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    src_lbl = ttk.Label(top, text="Looking for the memory card...")
    src_lbl.pack(side="left")

    def pick_folder():
        d = filedialog.askdirectory(title="Choose the LOG folder of the card")
        if d:
            load(d)

    ttk.Button(top, text="Open folder…",
               command=pick_folder).pack(side="right")
    ttk.Button(top, text="Rescan card",
               command=lambda: autoload()).pack(side="right", padx=6)

    mid = ttk.Frame(root, padding=(8, 0))
    mid.pack(fill="x")
    ttk.Label(mid, text="Recordings on this card "
                        "(select one):").pack(anchor="w")
    sess_list = tk.Listbox(mid, height=6, font=("Consolas", 10))
    sess_list.pack(fill="x", pady=4)

    rng = ttk.Frame(root, padding=(8, 4))
    rng.pack(fill="x")
    ttk.Label(rng, text="From").pack(side="left")
    ent_from = ttk.Entry(rng, width=22)
    ent_from.pack(side="left", padx=4)
    ttk.Label(rng, text="To").pack(side="left")
    ent_to = ttk.Entry(rng, width=22)
    ent_to.pack(side="left", padx=4)

    def full_range():
        s = current_session()
        if s:
            set_range_entries(s)

    ttk.Button(rng, text="Whole recording",
               command=full_range).pack(side="left", padx=8)

    chan = ttk.LabelFrame(root, text="What to show", padding=6)
    chan.pack(fill="x", padx=8)
    chan_vars = {}
    groups = [("Pulse (PPG site 1-4)", ["ppg1", "ppg2", "ppg3", "ppg4"]),
              ("Contact pressure",     ["p1", "p2", "p3", "p4"]),
              ("Temperature/Humidity", ["temps", "humid"]),
              ("Battery",              ["vbat"])]
    for gi, (gname, keys) in enumerate(groups):
        f = ttk.Frame(chan)
        f.grid(row=0, column=gi, padx=10, sticky="nw")
        ttk.Label(f, text=gname).pack(anchor="w")
        for k in keys:
            v = tk.BooleanVar(value=(k in ("ppg1", "p1")))
            chan_vars[k] = v
            ttk.Checkbutton(f, text=k, variable=v).pack(anchor="w")

    btns = ttk.Frame(root, padding=8)
    btns.pack(fill="x")
    status_lbl = ttk.Label(btns, text="")
    status_lbl.pack(side="right")

    fig_holder = ttk.Frame(root)
    fig_holder.pack(fill="both", expand=True)

    def current_session():
        sel = sess_list.curselection()
        if not sel:
            return None
        return state["sessions"][sel[0]]

    def fmt_t(session, up_ms):
        ep = session.to_epoch_ms(up_ms)
        if ep is None:
            return f"{up_ms/1000:.1f}"
        return datetime.fromtimestamp(ep / 1000).strftime("%Y-%m-%d %H:%M:%S")

    def set_range_entries(session):
        u0, u1 = session.samples[0][0], session.samples[-1][0]
        ent_from.delete(0, "end")
        ent_from.insert(0, fmt_t(session, u0))
        ent_to.delete(0, "end")
        ent_to.insert(0, fmt_t(session, u1))

    def parse_range(session):
        """Entries -> (t0_ms, t1_ms) in uptime."""
        u0, u1 = session.samples[0][0], session.samples[-1][0]

        def one(entry, default):
            txt = entry.get().strip()
            if not txt:
                return default
            if session.synced:
                for f in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%H:%M:%S",
                          "%H:%M"):
                    try:
                        dt = datetime.strptime(txt, f)
                        if f.startswith("%H"):
                            base = datetime.fromtimestamp(
                                session.to_epoch_ms(u0) / 1000)
                            dt = dt.replace(year=base.year, month=base.month,
                                            day=base.day)
                        ep = dt.timestamp() * 1000
                        a, b = session._fit
                        return (ep - b) / a
                    except ValueError:
                        continue
                raise ValueError(f"Can't understand time: {txt!r}")
            return float(txt) * 1000
        return one(ent_from, u0), one(ent_to, u1)

    def on_select(_evt=None):
        s = current_session()
        if s:
            set_range_entries(s)
    sess_list.bind("<<ListboxSelect>>", on_select)

    canvas_ref = {"c": None}

    def do_plot():
        s = current_session()
        if not s:
            messagebox.showinfo("Pick a recording",
                                "Select a recording in the list first.")
            return
        try:
            t0, t1 = parse_range(s)
        except ValueError as e:
            messagebox.showerror("Time range", str(e))
            return

        sel_samples = [(u, r) for u, r in s.samples if t0 <= u <= t1]
        if not sel_samples:
            messagebox.showinfo("No data", "No data in that time range.")
            return
        step = max(1, len(sel_samples) // 20000)
        sel_samples = sel_samples[::step]

        synced = s.synced
        if synced:
            xs = [datetime.fromtimestamp(s.to_epoch_ms(u) / 1000)
                  for u, _ in sel_samples]
        else:
            xs = [u / 1000 for u, _ in sel_samples]

        panels = []
        for i in range(1, 5):
            if chan_vars[f"ppg{i}"].get():
                panels.append((f"PPG site {i}",
                               [(f"r{i}", "red"), (f"i{i}", "purple"),
                                (f"g{i}", "green")], sel_samples))
        psel = [i for i in range(1, 5) if chan_vars[f"p{i}"].get()]
        if psel:
            panels.append(("Pressure (mbar)",
                           [(f"p{i}", None) for i in psel], sel_samples))

        sel_status = [(u, r) for u, r in s.status if t0 <= u <= t1]
        if chan_vars["temps"].get() and sel_status:
            panels.append(("Temperature (°C)",
                           [("sht1t", None), ("sht2t", None),
                            ("sht3t", None), ("tmp1", None),
                            ("tmp2", None), ("tmp3", None)], sel_status))
        if chan_vars["humid"].get() and sel_status:
            panels.append(("Humidity (%RH)",
                           [("sht1h", None), ("sht2h", None),
                            ("sht3h", None)], sel_status))
        if chan_vars["vbat"].get() and sel_status:
            panels.append(("Battery (mV)", [("vbat", None)], sel_status))

        if not panels:
            messagebox.showinfo("Nothing selected",
                                "Tick at least one signal to show.")
            return

        fig = Figure(figsize=(11, 6), dpi=100)
        axes = fig.subplots(len(panels), 1, sharex=True)
        if len(panels) == 1:
            axes = [axes]
        for ax, (title, cols, rows) in zip(axes, panels):
            if rows is sel_samples:
                rx = xs
            elif synced:
                rx = [datetime.fromtimestamp(s.to_epoch_ms(u) / 1000)
                      for u, _ in rows]
            else:
                rx = [u / 1000 for u, _ in rows]
            for col, color in cols:
                ys = [r.get(col) for _, r in rows]
                ax.plot(rx, ys, label=col, color=color, linewidth=0.8)
            ax.set_ylabel(title, fontsize=8)
            ax.legend(fontsize=7, loc="upper right")
            ax.grid(True, alpha=0.3)
        if synced:
            axes[-1].xaxis.set_major_formatter(DateFormatter("%H:%M:%S"))
            axes[-1].set_xlabel("Time")
        else:
            axes[-1].set_xlabel("Seconds since device start")
        fig.tight_layout()

        if canvas_ref["c"]:
            canvas_ref["c"].get_tk_widget().destroy()
        c = FigureCanvasTkAgg(fig, master=fig_holder)
        c.draw()
        c.get_tk_widget().pack(fill="both", expand=True)
        canvas_ref["c"] = c
        status_lbl.config(text=f"Showing {len(sel_samples)} points")

    def do_export(kind):
        s = current_session()
        if not s:
            messagebox.showinfo("Pick a recording",
                                "Select a recording in the list first.")
            return
        try:
            t0, t1 = parse_range(s)
        except ValueError as e:
            messagebox.showerror("Time range", str(e))
            return
        ext = ".csv" if kind == "csv" else ".xlsx"
        path = filedialog.asksaveasfilename(
            defaultextension=ext,
            filetypes=[("CSV" if kind == "csv" else "Excel", "*" + ext)],
            initialfile=f"kmm_recording_{s.boot}{ext}")
        if not path:
            return
        try:
            n = (export_csv if kind == "csv" else export_xlsx)(s, t0, t1, path)
        except Exception as e:
            messagebox.showerror("Export failed", str(e))
            return
        messagebox.showinfo("Done", f"Exported {n} rows to\n{path}")

    ttk.Button(btns, text="Plot",
               command=do_plot).pack(side="left")
    ttk.Button(btns, text="Export CSV",
               command=lambda: do_export("csv")).pack(side="left", padx=6)
    ttk.Button(btns, text="Export Excel",
               command=lambda: do_export("xlsx")).pack(side="left")

    def load(folder):
        state["folder"] = folder
        state["sessions"] = load_folder(folder)
        sess_list.delete(0, "end")
        for s in state["sessions"]:
            sess_list.insert("end", "  " + s.label())
        if state["sessions"]:
            sess_list.selection_set(0)
            set_range_entries(state["sessions"][0])
            src_lbl.config(text=f"Reading from: {folder}   "
                                f"({len(state['sessions'])} recordings)")
        else:
            src_lbl.config(text=f"No recordings found in {folder}")

    def autoload():
        card = find_card()
        if card:
            load(card)
        else:
            src_lbl.config(
                text="No card found — insert the device's memory card, "
                     "then click 'Rescan card' (or use 'Open folder…').")

    root.after(100, autoload)
    root.mainloop()


# --------------------------------------------------------------------------
#  Selftest
# --------------------------------------------------------------------------

def selftest(binfile=None):
    tmp = tempfile.mkdtemp(prefix="kmm_selftest_")
    if binfile:
        import shutil
        shutil.copy(binfile, os.path.join(tmp, "B001S001.BIN"))
    else:
        # synthesize 5 s of frames + syncs
        buf = bytearray()
        epoch0 = 1_785_600_000_000
        seq = 0
        for sec in range(5):
            for fr in range(25):
                t_ms = (sec * 1000) + fr * 40 + 40
                f = bytearray(204)
                struct.pack_into("<HBB", f, 0, MAGIC, 0x11, seq & 0xFF)
                struct.pack_into("<I", f, 4, t_ms)
                f[8] = 0xF; f[9] = 4; f[10] = 0xF
                for s in range(4):
                    for k in range(4):
                        base = 12 + s * 36 + k * 9
                        for ci, val in enumerate((100 + s, 200 + s, 300 + s)):
                            off = base + ci * 3
                            f[off:off+3] = struct.pack("<I", val)[:3]
                for k in range(4):
                    for b in range(4):
                        off = 156 + k * 12 + b * 3
                        f[off:off+3] = struct.pack("<I", 98000_00 + b)[:3]
                buf += f; seq += 1
            st = bytearray(45)
            struct.pack_into("<HBB", st, 0, MAGIC, 0x12, seq & 0xFF)
            struct.pack_into("<I", st, 4, sec * 1000 + 999)
            struct.pack_into("<H", st, 34, 3700)
            buf += st; seq += 1
            ts = bytearray(16)
            struct.pack_into("<HBB", ts, 0, MAGIC, 0x13, seq & 0xFF)
            struct.pack_into("<I", ts, 4, sec * 1000 + 999)
            struct.pack_into("<Q", ts, 8, epoch0 + sec * 1000 + 999)
            buf += ts; seq += 1
        open(os.path.join(tmp, "B001S001.BIN"), "wb").write(buf)

    sessions = load_folder(tmp)
    assert sessions, "no sessions parsed"
    s = sessions[0]
    print(f"selftest: {len(sessions)} session(s); first: {s.label()}")
    assert s.samples, "no samples"
    u0, u1 = s.samples[0][0], s.samples[-1][0]

    csvp = os.path.join(tmp, "out.csv")
    n = export_csv(s, u0, u1, csvp)
    assert n == len([x for x in s.samples if u0 <= x[0] <= u1])
    print(f"selftest: CSV {n} rows OK")

    try:
        xlsp = os.path.join(tmp, "out.xlsx")
        n2 = export_xlsx(s, u0, min(u1, u0 + 10_000), xlsp)
        print(f"selftest: XLSX {n2} rows OK")
    except ImportError:
        print("selftest: openpyxl missing — XLSX skipped")

    if s.synced:
        ep = s.to_epoch_ms(u0)
        print(f"selftest: wall clock start = "
              f"{datetime.fromtimestamp(ep/1000)}")

    # The GUI's plot path must be importable in the frozen bundle
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib.figure import Figure

    fig = Figure()
    ax = fig.subplots(1, 1)
    ax.plot([1, 2], [3, 4])
    print(f"selftest: matplotlib {matplotlib.__version__} OK")

    print("SELFTEST PASS")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        i = sys.argv.index("--selftest")
        arg = sys.argv[i + 1] if len(sys.argv) > i + 1 else None
        try:
            code = selftest(arg)
        except Exception as e:
            import traceback
            traceback.print_exc()
            open("selftest_result.txt", "w").write(f"FAIL: {e}\n")
            sys.exit(1)
        open("selftest_result.txt", "w").write("PASS\n")
        sys.exit(code)
    run_gui()
