#!/usr/bin/env python3
"""GUI firmware updater over UART (MCUboot serial recovery + smpclient).


Plain tkinter (stdlib) + pyserial for port listing; the update flow lives in
updater.py, shared with the CLI (fw_update.py).
"""

from bootstrap import ensure_smpclient, ensure_tk, ensure_venv_and_relaunch

ensure_venv_and_relaunch()
ensure_smpclient()
ensure_tk()

import queue  # noqa: E402
import threading  # noqa: E402
import tkinter as tk  # noqa: E402
from pathlib import Path  # noqa: E402
from tkinter import filedialog, ttk  # noqa: E402

from updater import STAGES, UpdateError, run_update  # noqa: E402

BG = "#383838"          # matches the GZM logo background
FG = "#e0e6ed"
ACCENT = "#80d8ff"
OK = "#00c853"
ERR = "#ff5252"
STEP_OFF = "#4a4f57"
STEP_COLORS = {"pending": STEP_OFF, "active": ACCENT, "done": OK, "error": ERR}

DEFAULT_IMAGE = (Path(__file__).resolve().parents[2]
                 / "app" / "build" / "app" / "zephyr" / "zephyr.signed.encrypted.bin")
LOGO_PNG = Path(__file__).resolve().parents[1] / "assets" / "gzm_logo_gui.png"


# USB-serial bridges (CH340, FTDI, CP210x, PL2303) are the likely UART4
# adapter; debug probes (ST-Link, J-Link) expose VCPs that must never be
# picked for the update.
USB_SERIAL_VIDS = {0x1a86, 0x0403, 0x10c4, 0x067b}
PROBE_VIDS = {0x0483, 0x1366}


def list_ports():
    """Return [(device, description, vid)] for the available serial ports."""
    try:
        from serial.tools import list_ports as lp
        ports = [(p.device, f"{p.description or ''} {p.manufacturer or ''}", p.vid)
                 for p in lp.comports()]
        def rank(item):
            dev, _, vid = item
            return (1 if "/ttyS" in dev else 0, 0 if vid in USB_SERIAL_VIDS else 1, dev)
        return sorted(ports, key=rank)
    except Exception:
        return []


class UpdaterGui(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("GZM - Firmware update v1.0")
        self.configure(bg=BG, padx=14, pady=12)
        self.resizable(True, False)
        self._queue = queue.Queue()
        self._running = False

        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TProgressbar", troughcolor="#2a2a2a",
                        background=ACCENT, thickness=10)

        self._build_widgets()
        self._refresh_ports()
        self.after(100, self._poll_queue)

    # ---------------- UI ----------------

    def _label(self, parent, text):
        return tk.Label(parent, text=text, bg=BG, fg=FG, anchor="w")

    def _build_widgets(self):
        row = 0
        grid = {"sticky": "ew", "pady": 3, "padx": 4}
        self.columnconfigure(1, weight=1)

        self._logo_img = None
        if LOGO_PNG.is_file():
            try:
                self._logo_img = tk.PhotoImage(file=str(LOGO_PNG))
                self.iconphoto(True, self._logo_img)
                tk.Label(self, image=self._logo_img, bg=BG, borderwidth=0
                         ).grid(row=row, column=0, columnspan=3, pady=(0, 14))
                row += 1
            except tk.TclError:
                self._logo_img = None

        self._label(self, "Update port (UART4):").grid(row=row, column=0, sticky="w")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(self, textvariable=self.port_var, width=28)
        self.port_combo.grid(row=row, column=1, **grid)
        tk.Button(self, text="↻", command=self._refresh_ports, bg="#2a2a2a", fg=FG,
                  relief="flat", width=3).grid(row=row, column=2, **grid)
        row += 1

        self._label(self, "App console (optional):").grid(row=row, column=0, sticky="w")
        self.console_var = tk.StringVar()
        self.console_combo = ttk.Combobox(self, textvariable=self.console_var, width=28)
        self.console_combo.grid(row=row, column=1, **grid)
        self._label(self, "").grid(row=row, column=2)
        row += 1

        self._label(self, "Firmware (.bin):").grid(row=row, column=0, sticky="w")
        self.image_var = tk.StringVar(
            value=str(DEFAULT_IMAGE) if DEFAULT_IMAGE.is_file() else "")
        tk.Entry(self, textvariable=self.image_var, bg="#2a2a2a", fg=FG,
                 insertbackground=FG, relief="flat"
                 ).grid(row=row, column=1, **grid)
        tk.Button(self, text="…", command=self._browse, bg="#2a2a2a", fg=FG,
                  relief="flat", width=3).grid(row=row, column=2, **grid)
        row += 1

        # One circle per step of the update, like the Sorion flasher:
        # grey = pending, blue = running, green = done, red = failed.
        self.steps_canvas = tk.Canvas(self, height=54, bg=BG, highlightthickness=0)
        self.steps_canvas.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(10, 0))
        self.steps_canvas.bind("<Configure>", lambda _e: self._draw_steps())
        self._step_state = ["pending"] * len(STAGES)
        row += 1

        self.progress = ttk.Progressbar(self, maximum=100)
        self.progress.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(8, 2))
        row += 1

        self.transfer_var = tk.StringVar(value="")
        tk.Label(self, textvariable=self.transfer_var, bg=BG, fg=FG, anchor="w",
                 font=("TkFixedFont", 9)).grid(row=row, column=0, columnspan=3,
                                               sticky="ew")
        row += 1

        self.status_var = tk.StringVar(value="Idle")
        tk.Label(self, textvariable=self.status_var, bg=BG, fg=ACCENT,
                 anchor="w").grid(row=row, column=0, columnspan=3, sticky="ew")
        row += 1

        self.log_text = tk.Text(self, height=14, width=80, bg="#252525", fg=FG,
                                insertbackground=FG, relief="flat", wrap="word",
                                state="disabled", font=("TkFixedFont", 9))
        self.log_text.grid(row=row, column=0, columnspan=3, sticky="nsew", pady=6)
        self.rowconfigure(row, weight=1)
        row += 1

        self.update_btn = tk.Button(self, text="Update Firmware",
                                    command=self._start, bg=ACCENT, fg="#000",
                                    relief="flat", font=("TkDefaultFont", 10, "bold"),
                                    padx=16, pady=6)
        self.update_btn.grid(row=row, column=0, columnspan=3, pady=(4, 0))

    def _draw_steps(self):
        c = self.steps_canvas
        c.delete("all")
        width = max(c.winfo_width(), 200)
        n = len(STAGES)
        r, y = 11, 16
        step = (width - 2 * 40) / (n - 1)
        xs = [40 + i * step for i in range(n)]
        for i in range(n - 1):
            done = self._step_state[i] == "done"
            c.create_line(xs[i] + r + 3, y, xs[i + 1] - r - 3, y,
                          fill=OK if done else STEP_OFF, width=2)
        for i, ((_, label), state) in enumerate(zip(STAGES, self._step_state)):
            color = STEP_COLORS[state]
            c.create_oval(xs[i] - r, y - r, xs[i] + r, y + r, fill=color, outline="")
            mark = {"done": "✓", "error": "✗"}.get(state, str(i + 1))
            c.create_text(xs[i], y, text=mark, fill=BG if state != "pending" else FG,
                          font=("TkDefaultFont", 9, "bold"))
            c.create_text(xs[i], y + r + 12, text=label,
                          fill=color if state != "pending" else FG,
                          font=("TkDefaultFont", 9))

    def _set_stage(self, name):
        index = [sid for sid, _ in STAGES].index(name)
        for i in range(len(STAGES)):
            if i < index:
                self._step_state[i] = "done"
            elif i == index:
                self._step_state[i] = "active"
        self._draw_steps()

    def _finish_steps(self, ok):
        for i, state in enumerate(self._step_state):
            if ok:
                self._step_state[i] = "done"
            elif state == "active":
                self._step_state[i] = "error"
        self._draw_steps()

    def _show_transfer(self, sent, total, rate):
        pct = 100.0 * sent / total if total else 0.0
        speed = f"{rate / 1024:.1f} KB/s" if rate else "-- KB/s"
        self.transfer_var.set(f"{pct:5.1f} %   ·   {sent:,} / {total:,} bytes   ·   {speed}")

    def _refresh_ports(self):
        ports = list_ports()
        devices = [d for d, _, _ in ports]
        self.port_combo["values"] = devices
        self.console_combo["values"] = [""] + devices
        console = [d for d, desc, _ in ports if "GZM" in desc or "Codigo" in desc]
        update = [d for d, _, vid in ports
                  if d not in console and "/ttyS" not in d and vid not in PROBE_VIDS]
        if update and not self.port_var.get():
            self.port_var.set(update[0])
        if console and not self.console_var.get():
            self.console_var.set(console[0])

    def _browse(self):
        path = filedialog.askopenfilename(
            title="Select the firmware image (zephyr.signed.encrypted.bin)",
            filetypes=[("Firmware image", "*.bin"), ("All files", "*")])
        if path:
            self.image_var.set(path)

    # ---------------- worker ----------------

    def _start(self):
        if self._running:
            return
        port = self.port_var.get().strip()
        image = self.image_var.get().strip()
        if not port or not image:
            self._log("Select the update port and the image first.", ERR)
            return
        self._running = True
        self.update_btn.configure(state="disabled")
        self.progress["value"] = 0
        self.transfer_var.set("")
        self._step_state = ["pending"] * len(STAGES)
        self._draw_steps()
        console = self.console_var.get().strip() or None
        threading.Thread(target=self._worker, args=(port, image, console),
                         daemon=True).start()

    def _worker(self, port, image, console):
        q = self._queue
        try:
            run_update(port, image, console_port=console,
                       log=lambda line: q.put(("log", line, None)),
                       progress=lambda pct: q.put(("progress", pct, None)),
                       status=lambda text: q.put(("status", text, None)),
                       stage=lambda name: q.put(("stage", name, None)),
                       transfer=lambda sent, total, rate:
                           q.put(("transfer", (sent, total, rate), None)))
            q.put(("done", "Update complete. Confirm it on the device shell (boot confirm)", OK))
        except UpdateError as exc:
            q.put(("done", f"Update failed: {exc}", ERR))
        except Exception as exc:  # unexpected
            q.put(("done", f"Unexpected error: {exc}", ERR))

    def _poll_queue(self):
        try:
            while True:
                kind, payload, color = self._queue.get_nowait()
                if kind == "log":
                    self._log(payload)
                elif kind == "progress":
                    self.progress["value"] = payload
                elif kind == "status":
                    self.status_var.set(payload)
                elif kind == "stage":
                    self._set_stage(payload)
                elif kind == "transfer":
                    self._show_transfer(*payload)
                elif kind == "done":
                    self.status_var.set(payload)
                    self._log(payload, color)
                    self.progress["value"] = 100 if color == OK else 0
                    self._finish_steps(color == OK)
                    self._running = False
                    self.update_btn.configure(state="normal")
        except queue.Empty:
            pass
        self.after(100, self._poll_queue)

    def _log(self, line, color=None):
        self.log_text.configure(state="normal")
        if color:
            tag = f"c{color}"
            self.log_text.tag_configure(tag, foreground=color)
            self.log_text.insert("end", line + "\n", tag)
        else:
            self.log_text.insert("end", line + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")


if __name__ == "__main__":
    UpdaterGui().mainloop()
