"""
flasher.py — ESP32 Flasher + Compiler GUI
Cable Tester 32CH project / SBC Group

Requisitos:
    pip install esptool pyserial

Uso:
    python flasher.py
"""

import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext, messagebox
import subprocess
import threading
import sys
import os
import shutil
import time
import serial
import serial.tools.list_ports
from pathlib import Path

# ── Constantes ────────────────────────────────────────────────
APP_TITLE    = "ESP32 Flasher — Cable Tester 32CH"
CHIP_DEFAULT = "esp32c3"
BAUD_OPTIONS = ["921600", "460800", "230400", "115200"]
CHIPS        = ["esp32c3", "esp32", "esp32s2", "esp32s3", "auto"]

APP_OFFSET_DEFAULT = "0x10000"

FULL_FLASH_OFFSETS = {
    "bootloader": "0x0",
    "partitions": "0x8000",
    "merged":     "0x0",
}

# Nombres de los binarios que genera build.py
APP_BIN_NAME    = "bring_up.ino.bin"
MERGED_BIN_NAME = "bring_up.ino.merged.bin"

# La NVS vive en 0x9000 (ver partitions.csv del proyecto). Escribir merged.bin
# en 0x0 borra los 4 MB completos y con ellos el SSID/contraseña guardados.
# Flashear solo la app en 0x10000 deja la NVS intacta.
NVS_WARNING = (
    "Tienes activado «Conservar WiFi guardado», pero vas a escribir en 0x0 "
    "o con un merged.bin.\n\n"
    "Eso borra la flash completa, incluida la particion NVS: se pierden el "
    "SSID y la contrasena, y el equipo arrancara en el portal captivo.\n\n"
    "Continuar de todos modos?"
)

# La tabla de particiones vive en 0x8000 y empieza con el magic 0xAA 0x50.
# Un modulo VIRGEN no la tiene: flashear solo la app @ 0x10000 deja un chip
# con aplicacion pero sin bootloader ni tabla → no arranca nunca, aunque
# esptool reporte exito. Es el fallo mas confuso de todos, por eso se detecta.
PART_TABLE_OFFSET = "0x8000"
PART_TABLE_MAGIC  = b"\xaa\x50"

VIRGIN_WARNING = (
    "Este modulo NO tiene tabla de particiones: esta virgen o su flash fue borrada.\n\n"
    "Vas a escribir solo la aplicacion en 0x10000. Eso dejaria el chip con app "
    "pero SIN bootloader ni tabla de particiones: esptool diria «completado» y "
    "el equipo no arrancaria nunca (se conecta y desconecta del USB).\n\n"
    "Un modulo nuevo necesita un flasheo COMPLETO (merged.bin en 0x0).\n\n"
    "Quieres cambiar a flasheo completo ahora?\n"
    "  Si  = cargar merged.bin en 0x0 (recomendado)\n"
    "  No  = flashear solo la app de todos modos"
)

# ── Colores ───────────────────────────────────────────────────
CLR_BG      = "#1e1e2e"
CLR_SURFACE = "#2a2a3e"
CLR_ACCENT  = "#89b4fa"
CLR_GREEN   = "#a6e3a1"
CLR_RED     = "#f38ba8"
CLR_YELLOW  = "#f9e2af"
CLR_FG      = "#cdd6f4"
CLR_MUTED   = "#6c7086"


# ══════════════════════════════════════════════════════════════
class FlashEntry:
    def __init__(self, parent, row, on_remove):
        self.frame = tk.Frame(parent, bg=CLR_SURFACE)
        self.frame.grid(row=row, column=0, columnspan=5, sticky="ew", pady=1)
        self.frame.columnconfigure(2, weight=1)

        self.addr_var = tk.StringVar(value=APP_OFFSET_DEFAULT)
        self.path_var = tk.StringVar(value="")

        tk.Label(self.frame, text="Addr:", bg=CLR_SURFACE, fg=CLR_MUTED,
                 font=("Consolas", 9)).grid(row=0, column=0, padx=(4, 2))
        tk.Entry(self.frame, textvariable=self.addr_var, width=10,
                 bg=CLR_BG, fg=CLR_YELLOW, insertbackground=CLR_FG,
                 font=("Consolas", 9), relief="flat").grid(row=0, column=1, padx=2)
        tk.Entry(self.frame, textvariable=self.path_var,
                 bg=CLR_BG, fg=CLR_FG, insertbackground=CLR_FG,
                 font=("Consolas", 9), relief="flat").grid(row=0, column=2, padx=2, sticky="ew")
        tk.Button(self.frame, text="…", command=self._browse,
                  bg=CLR_SURFACE, fg=CLR_ACCENT, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9)).grid(row=0, column=3, padx=2)
        tk.Button(self.frame, text="✕", command=lambda: on_remove(self),
                  bg=CLR_SURFACE, fg=CLR_RED, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9)).grid(row=0, column=4, padx=(2, 4))

    def _browse(self):
        path = filedialog.askopenfilename(
            title="Seleccionar .bin",
            filetypes=[("Binary", "*.bin"), ("All", "*.*")]
        )
        if path:
            self.path_var.set(path)
            name = os.path.basename(path).lower()
            for kw, offset in FULL_FLASH_OFFSETS.items():
                if kw in name:
                    self.addr_var.set(offset)
                    break

    def get(self):
        addr = self.addr_var.get().strip()
        path = self.path_var.get().strip()
        return (addr, path) if addr and path else None

    def destroy(self):
        self.frame.destroy()


# ══════════════════════════════════════════════════════════════
class FlasherApp:
    def __init__(self, root):
        self.root = root
        self.root.title(APP_TITLE)
        self.root.configure(bg=CLR_BG)
        self.root.resizable(True, True)
        self.root.minsize(760, 620)

        self._flash_entries: list[FlashEntry] = []
        self._process  = None
        self._running  = False
        self._builder  = None
        self._ser      = None          # serial.Serial activo
        self._ser_lock = threading.Lock()
        self._ser_stop = threading.Event()
        self._reconnect_delay = 1.5    # segundos entre reintentos tras un reset

        self._check_esptool()
        self._build_ui()
        self._refresh_ports()

    # ── esptool ───────────────────────────────────────────────
    def _check_esptool(self):
        self._esptool_cmd = None
        for c in ["esptool", "esptool.py"]:
            if shutil.which(c):
                self._esptool_cmd = c
                return
        try:
            subprocess.run([sys.executable, "-m", "esptool", "version"],
                           capture_output=True, timeout=5)
            self._esptool_cmd = "__module__"
        except Exception:
            pass

    def _esptool_parts(self):
        if self._esptool_cmd == "__module__":
            return [sys.executable, "-m", "esptool"]
        return [self._esptool_cmd]

    # ── UI ────────────────────────────────────────────────────
    def _build_ui(self):
        # Header
        top = tk.Frame(self.root, bg=CLR_BG)
        top.pack(fill="x", padx=12, pady=(10, 0))
        tk.Label(top, text="⚡  ESP32 Flasher — Cable Tester 32CH",
                 bg=CLR_BG, fg=CLR_ACCENT, font=("Consolas", 13, "bold")).pack(side="left")
        status = "esptool OK" if self._esptool_cmd else "⚠ esptool no encontrado — pip install esptool"
        color  = CLR_GREEN if self._esptool_cmd else CLR_RED
        tk.Label(top, text=status, bg=CLR_BG, fg=color,
                 font=("Consolas", 9)).pack(side="right")

        ttk.Separator(self.root).pack(fill="x", padx=12, pady=6)

        # Notebook (tabs)
        self.nb = ttk.Notebook(self.root)
        self.nb.pack(fill="both", expand=True, padx=12, pady=0)

        self._tab_flash  = tk.Frame(self.nb, bg=CLR_BG)
        self._tab_build  = tk.Frame(self.nb, bg=CLR_BG)
        self._tab_batch  = tk.Frame(self.nb, bg=CLR_BG)
        self._tab_serial = tk.Frame(self.nb, bg=CLR_BG)
        self.nb.add(self._tab_flash,  text="  ⚡ Flash  ")
        self.nb.add(self._tab_build,  text="  🔨 Compilar  ")
        self.nb.add(self._tab_batch,  text="  🏭 Lote  ")
        self.nb.add(self._tab_serial, text="  📡 Serial Monitor  ")

        self._build_flash_tab()
        self._build_compile_tab()
        self._build_batch_tab()
        self._build_serial_tab()

    # ── Tab Flash ─────────────────────────────────────────────
    def _build_flash_tab(self):
        tab = self._tab_flash

        # Config
        cfg = tk.Frame(tab, bg=CLR_BG)
        cfg.pack(fill="x", padx=8, pady=(8, 0))
        cfg.columnconfigure(1, weight=0)
        cfg.columnconfigure(3, weight=1)

        def lbl(text, r, c):
            tk.Label(cfg, text=text, bg=CLR_BG, fg=CLR_MUTED,
                     font=("Consolas", 9), anchor="e").grid(row=r, column=c,
                     sticky="e", padx=(0, 4), pady=3)

        lbl("Puerto:", 0, 0)
        pf = tk.Frame(cfg, bg=CLR_BG)
        pf.grid(row=0, column=1, sticky="w")
        self.port_var = tk.StringVar()
        self.port_cb  = ttk.Combobox(pf, textvariable=self.port_var,
                                     width=26, font=("Consolas", 9))
        self.port_cb.pack(side="left")
        tk.Button(pf, text="↺", command=self._refresh_ports,
                  bg=CLR_BG, fg=CLR_ACCENT, relief="flat",
                  activebackground=CLR_SURFACE, cursor="hand2",
                  font=("Consolas", 10)).pack(side="left", padx=4)

        lbl("Chip:", 0, 2)
        self.chip_var = tk.StringVar(value=CHIP_DEFAULT)
        ttk.Combobox(cfg, textvariable=self.chip_var, values=CHIPS,
                     width=12, font=("Consolas", 9)).grid(row=0, column=3, sticky="w")

        lbl("Baud:", 1, 0)
        self.baud_var = tk.StringVar(value=BAUD_OPTIONS[0])
        ttk.Combobox(cfg, textvariable=self.baud_var, values=BAUD_OPTIONS,
                     width=12, font=("Consolas", 9)).grid(row=1, column=1, sticky="w")

        lbl("Reset:", 1, 2)
        self.noreset_var = tk.BooleanVar(value=False)
        tk.Checkbutton(cfg, text="Manual BOOT+RESET (no auto-reset)",
                       variable=self.noreset_var, bg=CLR_BG, fg=CLR_FG,
                       selectcolor=CLR_SURFACE, activebackground=CLR_BG,
                       font=("Consolas", 9)).grid(row=1, column=3, sticky="w")

        # Conservar NVS (WiFi). Por defecto ACTIVADO: perder la red guardada
        # obliga a repetir el portal captivo, y casi nunca es lo que se quiere.
        lbl("WiFi:", 2, 0)
        self.keep_nvs_var = tk.BooleanVar(value=True)
        tk.Checkbutton(cfg,
                       text="Conservar WiFi guardado  (solo app @ 0x10000 — NO usar en módulo nuevo)",
                       variable=self.keep_nvs_var, bg=CLR_BG, fg=CLR_GREEN,
                       selectcolor=CLR_SURFACE, activebackground=CLR_BG,
                       font=("Consolas", 9)).grid(row=2, column=1, columnspan=3, sticky="w")

        # El stub flasher corre en RAM y exige mas del chip. Si revienta con
        # "Guru Meditation / Illegal instruction", el cargador de ROM (--no-stub)
        # suele funcionar: mas lento, pero mucho menos exigente. Que uno falle y
        # el otro no apunta a alimentacion marginal o soldadura floja.
        lbl("Modo:", 3, 0)
        self.nostub_var = tk.BooleanVar(value=False)
        tk.Checkbutton(cfg,
                       text="Sin stub (--no-stub) — lento, para módulos que dan Guru Meditation",
                       variable=self.nostub_var, bg=CLR_BG, fg=CLR_YELLOW,
                       selectcolor=CLR_SURFACE, activebackground=CLR_BG,
                       font=("Consolas", 9)).grid(row=3, column=1, columnspan=3, sticky="w")

        ttk.Separator(tab).pack(fill="x", padx=8, pady=6)

        # Archivos
        fh = tk.Frame(tab, bg=CLR_BG)
        fh.pack(fill="x", padx=8)
        tk.Label(fh, text="Archivos a flashear", bg=CLR_BG, fg=CLR_FG,
                 font=("Consolas", 10, "bold")).pack(side="left")
        tk.Button(fh, text="+ .bin", command=self._add_entry,
                  bg=CLR_SURFACE, fg=CLR_ACCENT, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9), padx=6).pack(side="left", padx=8)
        tk.Button(fh, text="🆕 Módulo nuevo (full flash)", command=self._add_merged_full,
                  bg=CLR_SURFACE, fg=CLR_RED, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9, "bold"), padx=6).pack(side="left", padx=4)
        tk.Button(fh, text="⬆ Solo app (conserva WiFi)", command=self._add_app_only,
                  bg=CLR_SURFACE, fg=CLR_GREEN, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9), padx=6).pack(side="left", padx=4)
        tk.Button(fh, text="⬇ Full flash (3 slots)", command=self._add_full_flash,
                  bg=CLR_SURFACE, fg=CLR_YELLOW, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9), padx=6).pack(side="left", padx=4)

        self.files_frame = tk.Frame(tab, bg=CLR_SURFACE, padx=4, pady=4)
        self.files_frame.pack(fill="x", padx=8, pady=(4, 0))
        self.files_frame.columnconfigure(2, weight=1)
        self._add_entry()

        ttk.Separator(tab).pack(fill="x", padx=8, pady=6)

        # Botones flash
        bf = tk.Frame(tab, bg=CLR_BG)
        bf.pack(fill="x", padx=8, pady=4)
        kw = dict(font=("Consolas", 10, "bold"), relief="flat", cursor="hand2", padx=14, pady=5)

        self.btn_flash = tk.Button(bf, text="⚡  FLASH",
                                   command=self._start_flash,
                                   bg=CLR_ACCENT, fg=CLR_BG,
                                   activebackground="#74c7ec", **kw)
        self.btn_flash.pack(side="left", padx=(0, 6))
        tk.Button(bf, text="🗑 Erase", command=self._erase_flash,
                  bg=CLR_SURFACE, fg=CLR_RED, activebackground=CLR_BG, **kw).pack(side="left", padx=4)
        tk.Button(bf, text="ℹ Chip Info", command=self._chip_info,
                  bg=CLR_SURFACE, fg=CLR_FG, activebackground=CLR_BG, **kw).pack(side="left", padx=4)
        self.btn_stop_flash = tk.Button(bf, text="⏹", command=self._stop,
                                        bg=CLR_SURFACE, fg=CLR_RED,
                                        activebackground=CLR_BG, state="disabled", **kw)
        self.btn_stop_flash.pack(side="left", padx=4)
        tk.Button(bf, text="🧹", command=lambda: self._clear_log(self.flash_log),
                  bg=CLR_SURFACE, fg=CLR_MUTED, activebackground=CLR_BG, **kw).pack(side="right")

        # Progress flash
        self.flash_progress = ttk.Progressbar(tab, mode="indeterminate")
        self.flash_progress.pack(fill="x", padx=8, pady=(0, 2))

        # Log flash
        self.flash_log = self._make_log(tab)

    # ── Tab Compilar ──────────────────────────────────────────
    def _build_compile_tab(self):
        tab = self._tab_build

        # Info sketch
        info = tk.Frame(tab, bg=CLR_BG)
        info.pack(fill="x", padx=8, pady=(8, 0))

        sketch_path = Path(__file__).parent.parent / "firmware" / "bring_up"
        build_path  = Path(__file__).parent / "build"

        tk.Label(info, text="Sketch:", bg=CLR_BG, fg=CLR_MUTED,
                 font=("Consolas", 9)).grid(row=0, column=0, sticky="e", padx=(0, 4))
        tk.Label(info, text=str(sketch_path), bg=CLR_BG, fg=CLR_FG,
                 font=("Consolas", 9), anchor="w").grid(row=0, column=1, sticky="w")

        tk.Label(info, text="Output:", bg=CLR_BG, fg=CLR_MUTED,
                 font=("Consolas", 9)).grid(row=1, column=0, sticky="e", padx=(0, 4))
        tk.Label(info, text=str(build_path), bg=CLR_BG, fg=CLR_FG,
                 font=("Consolas", 9), anchor="w").grid(row=1, column=1, sticky="w")

        tk.Label(info, text="Board:", bg=CLR_BG, fg=CLR_MUTED,
                 font=("Consolas", 9)).grid(row=2, column=0, sticky="e", padx=(0, 4))
        tk.Label(info, text="esp32:esp32:esp32c3:CDCOnBoot=cdc",
                 bg=CLR_BG, fg=CLR_YELLOW, font=("Consolas", 9), anchor="w").grid(row=2, column=1, sticky="w")

        ttk.Separator(tab).pack(fill="x", padx=8, pady=8)

        # Nota arduino-cli
        note = tk.Frame(tab, bg=CLR_SURFACE, padx=8, pady=6)
        note.pack(fill="x", padx=8)
        tk.Label(note, text="ℹ  arduino-cli se descarga automáticamente si no está instalado.\n"
                             "   Primera compilación instala el core ESP32 (~100 MB, ~2-5 min).",
                 bg=CLR_SURFACE, fg=CLR_MUTED, font=("Consolas", 9), justify="left").pack(anchor="w")

        ttk.Separator(tab).pack(fill="x", padx=8, pady=6)

        # Botones compilar
        bf = tk.Frame(tab, bg=CLR_BG)
        bf.pack(fill="x", padx=8, pady=4)
        kw = dict(font=("Consolas", 10, "bold"), relief="flat", cursor="hand2", padx=14, pady=5)

        self.btn_build = tk.Button(bf, text="🔨  COMPILAR",
                                   command=self._start_build,
                                   bg=CLR_GREEN, fg=CLR_BG,
                                   activebackground="#89dceb", **kw)
        self.btn_build.pack(side="left", padx=(0, 6))

        self.btn_build_flash = tk.Button(bf, text="🔨⚡ Compilar + Flash",
                                         command=self._build_then_flash,
                                         bg=CLR_ACCENT, fg=CLR_BG,
                                         activebackground="#74c7ec", **kw)
        self.btn_build_flash.pack(side="left", padx=4)

        self.btn_stop_build = tk.Button(bf, text="⏹", command=self._stop_build,
                                        bg=CLR_SURFACE, fg=CLR_RED,
                                        activebackground=CLR_BG, state="disabled", **kw)
        self.btn_stop_build.pack(side="left", padx=4)

        tk.Button(bf, text="📂 Abrir carpeta build",
                  command=self._open_build_dir,
                  bg=CLR_SURFACE, fg=CLR_MUTED,
                  activebackground=CLR_BG, **kw).pack(side="left", padx=4)

        tk.Button(bf, text="🧹", command=lambda: self._clear_log(self.build_log),
                  bg=CLR_SURFACE, fg=CLR_MUTED, activebackground=CLR_BG, **kw).pack(side="right")

        # Progress build
        self.build_progress = ttk.Progressbar(tab, mode="indeterminate")
        self.build_progress.pack(fill="x", padx=8, pady=(0, 2))

        # Log build
        self.build_log = self._make_log(tab)

    # ── Compilar ──────────────────────────────────────────────
    def _start_build(self, then_flash=False):
        if self._running:
            return
        try:
            from build import Builder
        except ImportError:
            self._log_w("No se puede importar build.py.", "err")
            return

        self._running = True
        self.btn_build.config(state="disabled")
        self.btn_build_flash.config(state="disabled")
        self.btn_stop_build.config(state="normal")
        self.build_progress.start(12)
        self.nb.select(self._tab_build)

        def log_cb(msg, tag="info"):
            self.root.after(0, lambda m=msg, t=tag: self._log_w(m, t))

        def done_cb(ok, bin_files):
            self.root.after(0, lambda: self._build_done(ok, bin_files, then_flash))

        self._builder = Builder(log_callback=log_cb, done_callback=done_cb)
        self._builder.run(threaded=True)

    def _build_then_flash(self):
        self._start_build(then_flash=True)

    def _stop_build(self):
        if self._builder:
            self._builder.stop()
            self._log_w("Compilación detenida.", "warn")

    def _build_done(self, ok, bin_files, then_flash=False):
        self._running = False
        self.btn_build.config(state="normal")
        self.btn_build_flash.config(state="normal")
        self.btn_stop_build.config(state="disabled")
        self.build_progress.stop()
        self._builder = None

        if ok and bin_files:
            self._clear_entries()
            keep_nvs = self.keep_nvs_var.get()

            if keep_nvs and "app" in bin_files:
                # Solo la particion de app: la NVS (y con ella el WiFi) sobrevive
                self._add_entry(addr=APP_OFFSET_DEFAULT, path=bin_files["app"])
                self._log_w("\n✅  Solo app @ 0x10000 — el WiFi guardado se conserva.", "ok")
            elif keep_nvs and "merged" in bin_files:
                # Pidio conservar WiFi pero no hay app.bin: avisar, no borrar en silencio
                self._add_entry(addr="0x0", path=bin_files["merged"])
                self._log_w("\n⚠️  No se genero app.bin; queda merged.bin @ 0x0, "
                            "que BORRA el WiFi guardado.", "warn")
            elif "merged" in bin_files:
                # Un solo archivo a 0x0 — más simple y sin riesgo de offset incorrecto
                self._add_entry(addr="0x0", path=bin_files["merged"])
                self._log_w("\n✅  merged.bin listo — flash completo en un solo archivo (0x0). "
                            "Borra la NVS/WiFi.", "ok")
            else:
                order = [("bootloader", "0x0"), ("partitions", "0x8000"), ("app", "0x10000")]
                for key, addr in order:
                    if key in bin_files:
                        self._add_entry(addr=addr, path=bin_files[key])
                self._log_w(f"\n✅  {len(bin_files)} archivo(s) listos en tab Flash.", "ok")

            if then_flash:
                self.root.after(500, lambda: (self.nb.select(self._tab_flash), self._start_flash()))
            else:
                self.root.after(200, lambda: self.nb.select(self._tab_flash))

    def _open_build_dir(self):
        build_dir = Path(__file__).parent / "build"
        build_dir.mkdir(exist_ok=True)
        if sys.platform == "win32":
            os.startfile(str(build_dir))
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(build_dir)])
        else:
            subprocess.Popen(["xdg-open", str(build_dir)])

    # ══════════════════════════════════════════════════════════
    # Tab Lote — flasheo + provision en serie
    # ══════════════════════════════════════════════════════════
    def _build_batch_tab(self):
        tab = self._tab_batch

        # ── Receta ──
        rec = tk.LabelFrame(tab, text=" Receta ", bg=CLR_BG, fg=CLR_ACCENT,
                            font=("Consolas", 9, "bold"), bd=1, relief="solid")
        rec.pack(fill="x", padx=8, pady=(8, 4))

        self.batch_recipe = tk.StringVar(value="master_links")
        tk.Radiobutton(rec, text="1 master + N links  (kit enlazado)",
                       variable=self.batch_recipe, value="master_links",
                       bg=CLR_BG, fg=CLR_FG, selectcolor=CLR_SURFACE,
                       activebackground=CLR_BG, font=("Consolas", 9),
                       command=self._batch_refresh_plan).grid(row=0, column=0, sticky="w", padx=6, pady=2)
        tk.Radiobutton(rec, text="Solo masters  (unidades independientes)",
                       variable=self.batch_recipe, value="all_master",
                       bg=CLR_BG, fg=CLR_FG, selectcolor=CLR_SURFACE,
                       activebackground=CLR_BG, font=("Consolas", 9),
                       command=self._batch_refresh_plan).grid(row=1, column=0, sticky="w", padx=6, pady=2)

        cnt = tk.Frame(rec, bg=CLR_BG)
        cnt.grid(row=0, column=1, rowspan=2, sticky="w", padx=16)
        tk.Label(cnt, text="Unidades:", bg=CLR_BG, fg=CLR_MUTED,
                 font=("Consolas", 9)).pack(side="left")
        self.batch_count = tk.StringVar(value="3")
        sp = tk.Spinbox(cnt, from_=1, to=20, width=4, textvariable=self.batch_count,
                        bg=CLR_BG, fg=CLR_YELLOW, font=("Consolas", 10),
                        relief="flat", insertbackground=CLR_FG,
                        command=self._batch_refresh_plan)
        sp.pack(side="left", padx=4)
        self.batch_count.trace_add("write", lambda *a: self._batch_refresh_plan())

        self.batch_plan_lbl = tk.Label(rec, text="", bg=CLR_BG, fg=CLR_GREEN,
                                       font=("Consolas", 9), justify="left")
        self.batch_plan_lbl.grid(row=2, column=0, columnspan=2, sticky="w", padx=6, pady=(2, 6))

        # ── WiFi a precargar ──
        wf = tk.LabelFrame(tab, text=" WiFi de TU RED (la del router) — OPCIONAL ",
                           bg=CLR_BG, fg=CLR_ACCENT,
                           font=("Consolas", 9, "bold"), bd=1, relief="solid")
        wf.pack(fill="x", padx=8, pady=4)
        wf.columnconfigure(1, weight=1)

        tk.Label(wf, text="SSID:", bg=CLR_BG, fg=CLR_MUTED,
                 font=("Consolas", 9)).grid(row=0, column=0, sticky="e", padx=(6, 4), pady=3)
        self.batch_ssid = tk.StringVar()
        tk.Entry(wf, textvariable=self.batch_ssid, bg=CLR_BG, fg=CLR_FG,
                 insertbackground=CLR_FG, font=("Consolas", 10),
                 relief="flat").grid(row=0, column=1, sticky="ew", pady=3)

        tk.Label(wf, text="Clave:", bg=CLR_BG, fg=CLR_MUTED,
                 font=("Consolas", 9)).grid(row=1, column=0, sticky="e", padx=(6, 4), pady=3)
        self.batch_pass = tk.StringVar()
        self.batch_pass_entry = tk.Entry(wf, textvariable=self.batch_pass, show="*",
                                         bg=CLR_BG, fg=CLR_FG, insertbackground=CLR_FG,
                                         font=("Consolas", 10), relief="flat")
        self.batch_pass_entry.grid(row=1, column=1, sticky="ew", pady=3)

        tk.Button(wf, text="👁", command=self._batch_toggle_pass,
                  bg=CLR_SURFACE, fg=CLR_ACCENT, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9)).grid(row=1, column=2, padx=4)

        wbtn = tk.Frame(wf, bg=CLR_BG)
        wbtn.grid(row=2, column=0, columnspan=3, sticky="w", padx=6, pady=(2, 6))
        tk.Button(wbtn, text="📥 Leer de una unidad ya configurada",
                  command=self._batch_read_wifi,
                  bg=CLR_SURFACE, fg=CLR_GREEN, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9), padx=6).pack(side="left")
        tk.Button(wbtn, text="🔍 Probar comunicacion",
                  command=self._batch_test_comm,
                  bg=CLR_SURFACE, fg=CLR_YELLOW, relief="flat",
                  activebackground=CLR_BG, cursor="hand2",
                  font=("Consolas", 9), padx=6).pack(side="left", padx=6)

        self.batch_wifi_all = tk.BooleanVar(value=False)
        tk.Checkbutton(wf, text="Precargar el WiFi tambien en los LINKS "
                                "(si no, solo el master lleva red)",
                       variable=self.batch_wifi_all, bg=CLR_BG, fg=CLR_FG,
                       selectcolor=CLR_SURFACE, activebackground=CLR_BG,
                       font=("Consolas", 9)).grid(row=3, column=0, columnspan=3,
                                                   sticky="w", padx=6)

        tk.Label(wf,
                 text="DEJA EL SSID VACIO para no precargar ninguna red: las unidades quedan\n"
                      "solo enlazadas, y cada una abrira su portal 'CableTester32-XXXX'\n"
                      "(red ABIERTA, sin clave) en http://192.168.4.1/\n"
                      "Este campo NO es el AP del ESP32 — es la red de tu router.\n"
                      "La clave solo se puede leer por USB, nunca por la web.",
                 bg=CLR_BG, fg=CLR_MUTED, font=("Consolas", 8),
                 justify="left").grid(row=4, column=0, columnspan=3, sticky="w", padx=6, pady=(0, 4))

        # ── Conservar configuración ──
        keep = tk.LabelFrame(tab, text=" Modo de flasheo ", bg=CLR_BG, fg=CLR_ACCENT,
                             font=("Consolas", 9, "bold"), bd=1, relief="solid")
        keep.pack(fill="x", padx=8, pady=4)
        self.batch_full = tk.BooleanVar(value=True)
        tk.Radiobutton(keep, text="Unidades NUEVAS — flasheo completo (merged.bin @ 0x0)",
                       variable=self.batch_full, value=True,
                       bg=CLR_BG, fg=CLR_FG, selectcolor=CLR_SURFACE,
                       activebackground=CLR_BG,
                       font=("Consolas", 9)).pack(anchor="w", padx=6, pady=1)
        tk.Radiobutton(keep, text="Ya provisionadas — solo app (0x10000): CONSERVA WiFi y enlace",
                       variable=self.batch_full, value=False,
                       bg=CLR_BG, fg=CLR_GREEN, selectcolor=CLR_SURFACE,
                       activebackground=CLR_BG,
                       font=("Consolas", 9)).pack(anchor="w", padx=6, pady=(1, 5))

        # ── Botones ──
        bf = tk.Frame(tab, bg=CLR_BG)
        bf.pack(fill="x", padx=8, pady=4)
        kw = dict(font=("Consolas", 10, "bold"), relief="flat", cursor="hand2", padx=14, pady=5)

        self.btn_batch = tk.Button(bf, text="🏭  INICIAR LOTE", command=self._batch_start,
                                   bg=CLR_GREEN, fg=CLR_BG,
                                   activebackground="#89dceb", **kw)
        self.btn_batch.pack(side="left", padx=(0, 6))

        self.btn_batch_next = tk.Button(bf, text="▶ Unidad conectada, continuar",
                                        command=self._batch_continue,
                                        bg=CLR_ACCENT, fg=CLR_BG,
                                        activebackground="#74c7ec",
                                        state="disabled", **kw)
        self.btn_batch_next.pack(side="left", padx=4)

        self.btn_batch_stop = tk.Button(bf, text="⏹", command=self._batch_stop,
                                        bg=CLR_SURFACE, fg=CLR_RED,
                                        activebackground=CLR_BG, state="disabled", **kw)
        self.btn_batch_stop.pack(side="left", padx=4)

        tk.Button(bf, text="🧹", command=lambda: self._clear_log(self.batch_log),
                  bg=CLR_SURFACE, fg=CLR_MUTED, activebackground=CLR_BG, **kw).pack(side="right")

        self.batch_progress = ttk.Progressbar(tab, mode="determinate")
        self.batch_progress.pack(fill="x", padx=8, pady=(0, 2))

        self.batch_log = self._make_log(tab)

        self._batch_stop_evt = threading.Event()
        self._batch_go_evt   = threading.Event()
        self._batch_refresh_plan()

    def _batch_toggle_pass(self):
        self.batch_pass_entry.config(
            show="" if self.batch_pass_entry.cget("show") else "*")

    def _batch_plan(self):
        """Devuelve la lista de roles a provisionar, en orden."""
        try:
            n = max(1, min(20, int(self.batch_count.get())))
        except ValueError:
            n = 1
        if self.batch_recipe.get() == "all_master":
            return ["master"] * n
        return ["master"] + ["link"] * (n - 1)

    def _batch_refresh_plan(self):
        plan = self._batch_plan()
        resumen = "  ".join(f"#{i+1}={r.upper()}" for i, r in enumerate(plan))
        self.batch_plan_lbl.config(text=f"Plan:  {resumen}")

    # ── Helpers serial para provision ─────────────────────────
    def _serial_cmd(self, port, cmd, timeout=5.0, settle=0.8):
        """
        Abre el puerto, manda un comando y devuelve la respuesta como texto.

        Distingue dos fallos que antes se confundian:
          None  -> no se pudo ABRIR el puerto (ocupado, inexistente)
          ""    -> puerto abierto pero la unidad NO CONTESTA
                   (firmware viejo sin ese comando, o el chip esta en el
                    bootloader porque falto el power cycle tras flashear)

        dtr/rts en False: el ESP32-C3 usa USB nativo; activarlos deja IO9 bajo.
        """
        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = 115200
            ser.timeout = 0.2
            ser.dtr = False
            ser.rts = False
            ser.open()
        except Exception:
            return None
        try:
            # El CDC nativo tarda en quedar listo tras abrir el puerto; y el
            # firmware usa readStringUntil() con 1 s de timeout, asi que puede
            # estar a mitad de espera. Un '\n' de cortesia lo destraba.
            time.sleep(settle)
            try:
                ser.write(b"\n")
                time.sleep(0.25)
                ser.reset_input_buffer()
            except Exception:
                pass

            ser.write((cmd + "\n").encode())
            buf = b""
            t_end   = time.time() + timeout
            t_quiet = None
            while time.time() < t_end:
                chunk = ser.read(256)
                if chunk:
                    buf += chunk
                    t_quiet = time.time()
                elif t_quiet and (time.time() - t_quiet) > 0.6:
                    break   # silencio tras recibir algo = respuesta completa
            return buf.decode("utf-8", errors="replace")
        except Exception:
            return ""
        finally:
            try:
                ser.close()
            except Exception:
                pass

    def _diag_serial(self, port, resp, cmd):
        """Traduce un fallo de _serial_cmd a causas concretas."""
        if resp is None:
            self._log_b(f"No se pudo ABRIR {port}.", "err")
            self._log_b("  · ¿El Serial Monitor lo tiene abierto? Desconectalo ahi.", "muted")
            self._log_b("  · ¿Es el puerto correcto? Refresca en la pestaña Flash.", "muted")
            return
        if not resp.strip():
            self._log_b(f"{port} abre, pero la unidad no contesta a '{cmd}'.", "err")
            self._log_b("  · Firmware VIEJO: esa unidad se flasheo antes de que "
                        "existiera el comando. Reflashéala.", "muted")
            self._log_b("  · O esta en el BOOTLOADER: tras flashear con "
                        "'--after no-reset' hay que hacer power cycle.", "muted")
            self._log_b("  · Comprueba con el Serial Monitor: si no sale el banner "
                        "al reconectar el USB, no esta corriendo el firmware.", "muted")
            return
        self._log_b(f"Respuesta inesperada a '{cmd}':", "warn")
        self._log_b(resp.strip()[:400], "muted")

    def _wait_for_port(self, port, timeout=25.0):
        """Espera a que un puerto reaparezca tras el reset (re-enumeracion USB)."""
        t_end = time.time() + timeout
        while time.time() < t_end:
            if self._batch_stop_evt.is_set():
                return False
            if port in [p.device for p in serial.tools.list_ports.comports()]:
                time.sleep(0.8)
                return True
            time.sleep(0.3)
        return False

    def _wait_ready(self, port, timeout=45.0, log=None):
        """
        Espera a que la unidad acepte comandos, no solo a que enumere el puerto.

        Una unidad recien flasheada esta MUDA un buen rato: ~2 s de animacion de
        LEDs, hasta 12 s de intento de WiFi y hasta 25 s de ventana de
        descubrimiento del enlace — todo dentro de setup(), donde loop() aun no
        lee el serial. Esperar solo a que aparezca el puerto no basta.

        El firmware cancela el descubrimiento en cuanto llega algo por serial,
        asi que aqui se sondea con 'link_mac' hasta que conteste.
        """
        t_end = time.time() + timeout
        intento = 0
        while time.time() < t_end:
            if self._batch_stop_evt.is_set():
                return None
            intento += 1
            resp = self._serial_cmd(port, "link_mac", timeout=3.0, settle=0.4)
            if resp:
                for line in resp.splitlines():
                    if "MAC:" in line:
                        return line.split("MAC:", 1)[1].strip()
            if log and intento % 3 == 0:
                restante = int(t_end - time.time())
                log(f"  ...esperando a que responda ({restante}s restantes)", "muted")
        return None

    def _batch_test_comm(self):
        """
        Diagnostico: manda 'help' y muestra lo que llegue, en crudo.
        Sirve para separar «no arranca» de «firmware viejo» de «puerto ocupado».
        """
        port = self._get_port()
        if not port:
            messagebox.showwarning("Puerto", "Selecciona un puerto en la pestaña Flash.")
            return
        self.nb.select(self._tab_batch)
        self._log_b(f"── Probando comunicacion con {port} ──", "info")
        self.root.update_idletasks()

        resp = self._serial_cmd(port, "help", timeout=6.0)
        if resp is None or not resp.strip():
            self._diag_serial(port, resp, "help")
            return

        self._log_b("Respuesta recibida — la unidad ESTA corriendo el firmware:", "ok")
        self._log_b(resp.strip()[:1500], "muted")

        faltan = [c for c in ("wifi_set", "wifi_get", "role_set", "link_mac")
                  if c not in resp]
        if faltan:
            self._log_b(f"⚠  Le faltan comandos de provision: {', '.join(faltan)}", "err")
            self._log_b("   Firmware anterior a la provision por lote → reflashéala.", "err")
        else:
            self._log_b("✅  Tiene todos los comandos de provision. Lista para el lote.", "ok")

    def _batch_read_wifi(self):
        port = self._get_port()
        if not port:
            messagebox.showwarning("Puerto", "Selecciona un puerto en la pestaña Flash.")
            return
        self.nb.select(self._tab_batch)
        self._log_b(f"Leyendo WiFi de {port}...", "info")
        self.root.update_idletasks()

        resp = self._serial_cmd(port, "wifi_get")
        if resp is None or not resp.strip():
            self._diag_serial(port, resp, "wifi_get")
            return
        if "Comando desconocido" in resp:
            self._log_b("La unidad respondio, pero NO conoce 'wifi_get': lleva firmware "
                        "anterior a la provision por lote. Reflashéala.", "err")
            return

        ssid = pw = None
        for line in resp.splitlines():
            if "SSID:" in line:
                ssid = line.split("SSID:", 1)[1].strip()
            elif "PASS:" in line and "solo visible" not in line:
                pw = line.split("PASS:", 1)[1].strip()

        if ssid:
            self.batch_ssid.set(ssid)
            self._log_b(f"SSID leido: {ssid}", "ok")
        else:
            self._log_b("No se encontro SSID en la respuesta:", "warn")
            self._log_b(resp.strip(), "muted")
        if pw:
            self.batch_pass.set(pw)
            self._log_b("Clave leida correctamente.", "ok")

    # ── Ciclo del lote ────────────────────────────────────────
    def _batch_start(self):
        if self._running:
            messagebox.showinfo("Ocupado", "Hay otra operacion en curso.")
            return
        if not self._esptool_cmd:
            messagebox.showerror("Error", "esptool no instalado.\npip install esptool")
            return
        # El SSID es OPCIONAL: sin el, las unidades quedan enlazadas pero sin red,
        # y cada una abrira su propio portal captivo.
        if not self.batch_ssid.get().strip():
            if not messagebox.askyesno(
                    "Sin WiFi",
                    "No pusiste SSID.\n\n"
                    "Las unidades quedaran ENLAZADAS entre si, pero sin red: cada una "
                    "abrira su portal 'CableTester32-XXXX' (abierto, sin clave) en "
                    "http://192.168.4.1/ para configurarla despues.\n\n"
                    "¿Continuar sin precargar red?"):
                return

        app_bin = Path(__file__).parent / "build" / APP_BIN_NAME
        merged  = Path(__file__).parent / "build" / MERGED_BIN_NAME
        if not app_bin.is_file() and not merged.is_file():
            messagebox.showerror("Firmware", "No hay binarios en tools/build.\nCompila primero.")
            return

        plan = self._batch_plan()
        self._batch_stop_evt.clear()
        self._batch_go_evt.clear()
        self._running = True
        self.btn_batch.config(state="disabled")
        self.btn_batch_stop.config(state="normal")
        self.batch_progress.config(maximum=len(plan), value=0)
        self._clear_log(self.batch_log)

        threading.Thread(target=self._batch_worker, args=(plan,), daemon=True).start()

    def _batch_continue(self):
        # Cada unidad que conectas puede enumerar en un COM distinto, asi que
        # hay que re-detectar antes de seguir. _refresh_ports() elige sola la
        # que tenga firma de ESP32.
        self._refresh_ports()
        p = self._get_port()
        self._log_b(f"Puerto detectado: {p}" if p
                    else "⚠  Ningun puerto ESP32 detectado — conectala y reintenta.",
                    "info" if p else "err")
        self._batch_go_evt.set()

    def _batch_stop(self):
        self._batch_stop_evt.set()
        self._batch_go_evt.set()      # desbloquea la espera
        self._log_b("Deteniendo lote...", "warn")

    def _batch_worker(self, plan):
        ssid     = self.batch_ssid.get().strip()
        pw       = self.batch_pass.get()
        wifi_all = self.batch_wifi_all.get()
        full     = self.batch_full.get()
        build    = Path(__file__).parent / "build"
        image    = build / (MERGED_BIN_NAME if full else APP_BIN_NAME)
        offset   = "0x0" if full else APP_OFFSET_DEFAULT
        ok_count = fail_count = 0

        # MAC del master, capturada en la primera unidad. Es lo que se escribe
        # en cada hijo para dejar el enlace hecho sin emparejamiento por radio.
        master_mac = None

        def log(m, t="info"):
            self.root.after(0, lambda: self._log_b(m, t))

        log(f"Lote de {len(plan)} unidad(es). "
            + (f"Red: '{ssid}'" if ssid else "SIN precargar red (portal en cada unidad)"),
            "info")
        log(f"Imagen: {image.name} @ {offset}"
            + ("  (borra NVS)" if full else "  (conserva WiFi y enlace)"), "muted")
        log("El enlace se escribe por USB: sin link_pair ni radio.", "muted")

        for idx, role in enumerate(plan, start=1):
            if self._batch_stop_evt.is_set():
                break

            log("", "info")
            log(f"───── UNIDAD {idx}/{len(plan)}  →  rol {role.upper()} ─────", "info")
            log("Conecta la unidad por USB y pulsa «Unidad conectada, continuar».", "warn")

            self.root.after(0, lambda: self.btn_batch_next.config(state="normal"))
            self._batch_go_evt.clear()
            self._batch_go_evt.wait()
            self.root.after(0, lambda: self.btn_batch_next.config(state="disabled"))
            if self._batch_stop_evt.is_set():
                break

            port = self._get_port()
            if not port:
                log("Sin puerto seleccionado en la pestaña Flash. Se omite.", "err")
                fail_count += 1
                continue

            # 1. Flash
            log(f"[1/4] Flasheando {port}...", "info")
            cmd = self._esptool_parts() + [
                "--chip", self.chip_var.get(), "--port", port,
                "--baud", self.baud_var.get(),
                "--before", "default-reset", "--after", "hard-reset",
                "write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
                offset, str(image),
            ]
            try:
                r = subprocess.run(
                    cmd, capture_output=True, text=True, timeout=180,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
            except Exception as ex:
                log(f"Error lanzando esptool: {ex}", "err")
                fail_count += 1
                continue
            if r.returncode != 0:
                log("Fallo el flasheo:", "err")
                log((r.stdout or "")[-500:], "muted")
                fail_count += 1
                continue
            log("Flasheo OK.", "ok")

            # 2. Esperar arranque y leer la MAC
            log("[2/4] Esperando arranque...", "info")
            if not self._wait_for_port(port):
                log("El puerto no reaparecio tras el reset. Haz power cycle manual "
                    "y repite esta unidad.", "err")
                fail_count += 1
                continue

            log("  (una unidad recien flasheada tarda hasta ~30 s en responder)", "muted")
            mac = self._wait_ready(port, timeout=50.0, log=log)
            if not mac:
                log("La unidad nunca respondio.", "err")
                log("  · ¿Arranca? Reconecta el USB y mira el banner en el Serial Monitor.", "muted")
                log("  · Si el USB se conecta y desconecta en bucle, es un problema de "
                    "ALIMENTACION o de strapping (IO9), no del firmware.", "muted")
                fail_count += 1
                continue
            log(f"MAC: {mac}", "muted")

            # 3. Provisionar rol, enlace y (si toca) WiFi
            log("[3/4] Provisionando...", "info")
            quiere_wifi = bool(ssid) and ((role == "master") or wifi_all)
            if quiere_wifi:
                r = self._serial_cmd(port, f"wifi_set {ssid}|{pw}") or ""
                if "[PROV]" not in r:
                    log("No acepto las credenciales WiFi.", "err")
                    fail_count += 1
                    continue

            if role == "master":
                master_mac = mac
                if (self._serial_cmd(port, "role_set master") or "").find("[PROV]") < 0:
                    log("No acepto el rol master.", "err")
                    fail_count += 1
                    continue
                log(f"MASTER listo. Su MAC se escribira en cada link.", "ok")
            else:
                if not master_mac:
                    log("No hay MAC de master todavia — el master debe ir primero.", "err")
                    fail_count += 1
                    continue
                # link_master fija el peer Y el rol LINK de una vez
                r = self._serial_cmd(port, f"link_master {master_mac}") or ""
                if "[PROV]" not in r:
                    log(f"No acepto el enlace al master: {r.strip()[:200]}", "err")
                    fail_count += 1
                    continue
                log(f"Enlazado al master {master_mac}.", "ok")

            # 4. Verificar leyendo de vuelta
            log("[4/4] Verificando...", "info")
            chk_r = (self._serial_cmd(port, "role_get")  or "").upper()
            chk_l = (self._serial_cmd(port, "link_info") or "")
            chk_w = (self._serial_cmd(port, "wifi_get")  or "")

            role_ok = ("MASTER" in chk_r) if role == "master" else ("LINK" in chk_r)
            link_ok = True if role == "master" else (master_mac.upper() in chk_l.upper())
            wifi_ok = (ssid in chk_w) if quiere_wifi else True

            if role_ok and link_ok and wifi_ok:
                extra = f", red '{ssid}'" if quiere_wifi else ", sin WiFi"
                log(f"✅  UNIDAD {idx} LISTA  ({role.upper()}{extra})", "ok")
                ok_count += 1
            else:
                log(f"⚠  Verificacion fallida — rol={role_ok} enlace={link_ok} wifi={wifi_ok}",
                    "err")
                log(chk_r.strip()[:200], "muted")
                log(chk_l.strip()[:400], "muted")
                fail_count += 1

            self.root.after(0, lambda v=idx: self.batch_progress.config(value=v))
            log("Desconecta esta unidad.", "muted")

        log("", "info")
        log("═" * 46, "info")
        log(f"LOTE TERMINADO — OK={ok_count}  FALLOS={fail_count}",
            "ok" if fail_count == 0 else "warn")
        if master_mac and plan.count("link"):
            log(f"Master: {master_mac}", "info")
            log("El enlace YA esta escrito. Enciende todas las unidades juntas:", "info")
            log("cada LINK se anuncia y el master lo da de alta solo.", "info")
            log("Comprueba con 'link_info' en el master (debe listar los hijos).", "info")
        log("═" * 46, "info")

        self.root.after(0, self._batch_done)

    def _batch_done(self):
        self._running = False
        self.btn_batch.config(state="normal")
        self.btn_batch_next.config(state="disabled")
        self.btn_batch_stop.config(state="disabled")

    def _log_b(self, msg, tag="info"):
        self._write_log(self.batch_log, msg, tag)

    # ── Tab Serial Monitor ────────────────────────────────────
    def _build_serial_tab(self):
        tab = self._tab_serial

        # ── Barra de configuración ──
        bar = tk.Frame(tab, bg=CLR_BG)
        bar.pack(fill="x", padx=8, pady=(8, 0))

        tk.Label(bar, text="Puerto:", bg=CLR_BG, fg=CLR_MUTED,
                 font=("Consolas", 9)).pack(side="left")
        self.ser_port_var = tk.StringVar()
        self.ser_port_cb  = ttk.Combobox(bar, textvariable=self.ser_port_var,
                                          width=22, font=("Consolas", 9))
        self.ser_port_cb.pack(side="left", padx=(4, 8))

        tk.Label(bar, text="Baud:", bg=CLR_BG, fg=CLR_MUTED,
                 font=("Consolas", 9)).pack(side="left")
        self.ser_baud_var = tk.StringVar(value="115200")
        ttk.Combobox(bar, textvariable=self.ser_baud_var,
                     values=BAUD_OPTIONS + ["9600", "57600"],
                     width=10, font=("Consolas", 9)).pack(side="left", padx=(4, 8))

        # Auto-reconnect checkbox
        self.ser_autorecon_var = tk.BooleanVar(value=True)
        tk.Checkbutton(bar, text="Auto-reconectar tras reset",
                       variable=self.ser_autorecon_var,
                       bg=CLR_BG, fg=CLR_FG, selectcolor=CLR_SURFACE,
                       activebackground=CLR_BG,
                       font=("Consolas", 9)).pack(side="left", padx=8)

        # ── Botones connect / disconnect ──
        btn_bar = tk.Frame(tab, bg=CLR_BG)
        btn_bar.pack(fill="x", padx=8, pady=4)
        kw = dict(font=("Consolas", 10, "bold"), relief="flat", cursor="hand2", padx=12, pady=4)

        self.btn_ser_connect = tk.Button(btn_bar, text="▶  Conectar",
                                         command=self._ser_connect,
                                         bg=CLR_GREEN, fg=CLR_BG,
                                         activebackground="#89dceb", **kw)
        self.btn_ser_connect.pack(side="left", padx=(0, 6))

        self.btn_ser_disconnect = tk.Button(btn_bar, text="⏹  Desconectar",
                                             command=self._ser_disconnect,
                                             bg=CLR_SURFACE, fg=CLR_RED,
                                             activebackground=CLR_BG,
                                             state="disabled", **kw)
        self.btn_ser_disconnect.pack(side="left", padx=4)

        tk.Button(btn_bar, text="↺  Refresh puertos",
                  command=self._ser_refresh_ports,
                  bg=CLR_SURFACE, fg=CLR_ACCENT,
                  activebackground=CLR_BG, **kw).pack(side="left", padx=4)

        tk.Button(btn_bar, text="🧹 Limpiar",
                  command=lambda: self._clear_log(self.ser_log),
                  bg=CLR_SURFACE, fg=CLR_MUTED,
                  activebackground=CLR_BG, **kw).pack(side="right")

        # ── Indicador de estado ──
        self.ser_status_var = tk.StringVar(value="● Desconectado")
        self.ser_status_lbl = tk.Label(btn_bar, textvariable=self.ser_status_var,
                                       bg=CLR_BG, fg=CLR_RED,
                                       font=("Consolas", 9, "bold"))
        self.ser_status_lbl.pack(side="left", padx=12)

        ttk.Separator(tab).pack(fill="x", padx=8, pady=4)

        # ── Log del serial ──
        self.ser_log = scrolledtext.ScrolledText(
            tab, bg=CLR_BG, fg=CLR_FG,
            font=("Consolas", 10), relief="flat",
            insertbackground=CLR_FG, wrap="word", state="disabled"
        )
        self.ser_log.pack(fill="both", expand=True, padx=8, pady=(0, 4))
        self.ser_log.tag_config("rx",      foreground=CLR_FG)
        self.ser_log.tag_config("tx",      foreground=CLR_YELLOW)
        self.ser_log.tag_config("sys",     foreground=CLR_MUTED)
        self.ser_log.tag_config("ok",      foreground=CLR_GREEN)
        self.ser_log.tag_config("err",     foreground=CLR_RED)

        # ── Línea de entrada ──
        send_frame = tk.Frame(tab, bg=CLR_SURFACE)
        send_frame.pack(fill="x", padx=8, pady=(0, 8))
        send_frame.columnconfigure(0, weight=1)

        self.ser_input = tk.Entry(send_frame, bg=CLR_BG, fg=CLR_YELLOW,
                                   insertbackground=CLR_FG,
                                   font=("Consolas", 10), relief="flat")
        self.ser_input.grid(row=0, column=0, sticky="ew", padx=(6, 4), pady=4)
        self.ser_input.bind("<Return>", self._ser_send)
        self.ser_input.bind("<Up>",     self._ser_history_up)
        self.ser_input.bind("<Down>",   self._ser_history_down)

        tk.Button(send_frame, text="Enviar ↵",
                  command=self._ser_send,
                  bg=CLR_ACCENT, fg=CLR_BG,
                  activebackground="#74c7ec",
                  font=("Consolas", 9, "bold"),
                  relief="flat", cursor="hand2",
                  padx=10).grid(row=0, column=1, padx=(0, 6), pady=4)

        # Historial de comandos
        self._ser_history: list[str] = []
        self._ser_hist_idx = -1

        # Poblar puertos en el combo del serial tab
        self._ser_refresh_ports()

    # ── Serial: gestión de puertos ────────────────────────────
    def _ser_refresh_ports(self):
        ports = serial.tools.list_ports.comports()
        items, esp_item = [], None
        for p in sorted(ports, key=lambda x: x.device):
            desc  = p.description or ""
            vid   = p.vid or 0
            label = f"{p.device}  —  {desc}"
            is_esp = vid in (0x303A, 0x10C4, 0x1A86) or \
                     any(w in desc.lower() for w in ("espressif", "cp210", "ch340"))
            if is_esp:
                label = "★ " + label
                if esp_item is None:
                    esp_item = label
            items.append(label)

        self.ser_port_cb["values"] = items
        if esp_item:
            self.ser_port_var.set(esp_item)
        else:
            self.ser_port_var.set("")   # no auto-elegir puertos que no son ESP

    def _ser_get_port(self):
        val = self.ser_port_var.get().strip().lstrip("★ ")
        return val.split()[0] if val else None

    # ── Serial: conectar / desconectar ────────────────────────
    def _ser_connect(self):
        port = self._ser_get_port()
        if not port:
            self._ser_log("Sin puerto seleccionado.", "err")
            return
        baud = int(self.ser_baud_var.get())
        self._ser_stop.clear()
        self.btn_ser_connect.config(state="disabled")
        self.btn_ser_disconnect.config(state="normal")
        threading.Thread(target=self._ser_worker,
                         args=(port, baud), daemon=True).start()

    def _ser_disconnect(self):
        self._ser_stop.set()
        self.btn_ser_connect.config(state="normal")
        self.btn_ser_disconnect.config(state="disabled")
        self._ser_set_status(False)

    # ── Serial: worker con auto-reconnect ─────────────────────
    def _ser_worker(self, port, baud):
        """
        Hilo que mantiene la conexión serial abierta.
        Si el ESP32-C3 se resetea (USB nativo se desconecta), espera y reconecta.
        Esto resuelve el crash de miniterm al presionar SW2/RESET.
        """
        while not self._ser_stop.is_set():
            try:
                self.root.after(0, lambda: self._ser_log(
                    f"Conectando a {port} @ {baud}...", "sys"))

                ser = serial.Serial()
                ser.port         = port
                ser.baudrate     = baud
                ser.timeout      = 0.1
                ser.write_timeout = 2
                ser.dtr          = False   # evita reset al abrir (ESP32-C3 nativo USB)
                ser.rts          = False
                ser.open()
                try:
                    with self._ser_lock:
                        self._ser = ser
                    # Pequeña pausa para que el CDC nativo inicialice el buffer
                    time.sleep(0.1)
                    self.root.after(0, lambda: self._ser_set_status(True, port, baud))

                    buf = b""
                    while not self._ser_stop.is_set():
                        try:
                            chunk = ser.read(64)  # timeout=0.1s, devuelve 0-64 bytes
                        except serial.SerialException:
                            break  # Puerto perdido → intentar reconectar
                        except OSError:
                            break  # ClearCommError failure en USB CDC nativo Windows

                        if chunk:
                            buf += chunk
                            # Procesar líneas completas
                            while b"\n" in buf:
                                line, buf = buf.split(b"\n", 1)
                                text = line.decode("utf-8", errors="replace").rstrip("\r")
                                self.root.after(0, lambda t=text: self._ser_log(t, "rx"))
                finally:
                    ser.close()
                    with self._ser_lock:
                        self._ser = None

            except serial.SerialException:
                with self._ser_lock:
                    self._ser = None

            # Puerto perdido o no disponible
            if self._ser_stop.is_set():
                break

            if self.ser_autorecon_var.get():
                self.root.after(0, lambda: self._ser_set_status(False, reconnecting=True))
                # Esperar a que el puerto vuelva (el ESP32-C3 tarda ~1-2s en reaparecer)
                for _ in range(20):
                    if self._ser_stop.is_set():
                        break
                    time.sleep(0.15)
                    # Verificar si el puerto ya está disponible
                    available = [p.device for p in serial.tools.list_ports.comports()]
                    if port in available:
                        break
            else:
                # Sin auto-reconect: terminar
                self.root.after(0, self._ser_disconnect)
                break

        with self._ser_lock:
            self._ser = None
        self.root.after(0, lambda: self._ser_set_status(False))

    # ── Serial: enviar datos ──────────────────────────────────
    def _ser_send(self, event=None):
        text = self.ser_input.get().strip()
        if not text:
            return
        self.ser_input.delete(0, "end")

        # Historial
        if not self._ser_history or self._ser_history[-1] != text:
            self._ser_history.append(text)
        self._ser_hist_idx = -1

        with self._ser_lock:
            if self._ser and self._ser.is_open:
                try:
                    self._ser.write((text + "\n").encode())
                    self._ser_log(f"> {text}", "tx")
                except (serial.SerialException, OSError) as e:
                    self._ser_log(f"Error enviando: {e}", "err")
            else:
                self._ser_log("No conectado.", "err")

    def _ser_history_up(self, event=None):
        if not self._ser_history:
            return
        self._ser_hist_idx = max(0, (self._ser_hist_idx - 1)
                                 if self._ser_hist_idx >= 0
                                 else len(self._ser_history) - 1)
        self.ser_input.delete(0, "end")
        self.ser_input.insert(0, self._ser_history[self._ser_hist_idx])

    def _ser_history_down(self, event=None):
        if self._ser_hist_idx < 0:
            return
        self._ser_hist_idx += 1
        self.ser_input.delete(0, "end")
        if self._ser_hist_idx < len(self._ser_history):
            self.ser_input.insert(0, self._ser_history[self._ser_hist_idx])
        else:
            self._ser_hist_idx = -1

    # ── Serial: UI helpers ─────────────────────────────────────
    def _ser_set_status(self, connected, port="", baud=0, reconnecting=False):
        if reconnecting:
            self.ser_status_var.set("↺ Reconectando...")
            self.ser_status_lbl.config(fg=CLR_YELLOW)
        elif connected:
            self.ser_status_var.set(f"● {port} @ {baud}")
            self.ser_status_lbl.config(fg=CLR_GREEN)
        else:
            self.ser_status_var.set("● Desconectado")
            self.ser_status_lbl.config(fg=CLR_RED)

    def _ser_log(self, msg, tag="rx"):
        self.ser_log.config(state="normal")
        self.ser_log.insert("end", msg + "\n", tag)
        self.ser_log.see("end")
        self.ser_log.config(state="disabled")

    # ── Puertos ───────────────────────────────────────────────
    def _refresh_ports(self):
        ports = serial.tools.list_ports.comports()
        items, esp_item = [], None
        for p in sorted(ports, key=lambda x: x.device):
            desc = p.description or ""
            vid  = p.vid or 0
            label = f"{p.device}  —  {desc}"
            is_esp = vid in (0x303A, 0x10C4, 0x1A86) \
                     or any(w in desc.lower() for w in ("espressif", "cp210", "ch340"))
            if is_esp:
                label = "★ " + label
                if esp_item is None:
                    esp_item = label
            items.append(label)

        self.port_cb["values"] = items
        if esp_item:
            self.port_cb.set(esp_item)
        else:
            # NUNCA auto-elegir un puerto sin firma de ESP. Antes se hacia
            # current(0) y salia COM1 (serie legacy de la placa madre): el flasheo
            # fallaba con "No serial data received" sin explicar por que.
            self.port_cb.set("")
            if items:
                self._log_f("⚠  Ningun puerto con firma de ESP32. Conecta el modulo "
                            "o elige el puerto a mano (COM1 NO es un ESP32).", "warn")

    def _get_port(self):
        val = self.port_var.get().strip().lstrip("★ ")
        return val.split()[0] if val else None

    # ── Flash ─────────────────────────────────────────────────
    def _start_flash(self):
        if not self._esptool_cmd:
            messagebox.showerror("Error", "esptool no instalado.\npip install esptool")
            return
        port = self._get_port()
        if not port:
            messagebox.showwarning("Puerto", "Selecciona un puerto COM.")
            return
        entries = [e.get() for e in self._flash_entries if e.get()]
        if not entries:
            messagebox.showwarning("Archivos", "Agrega al menos un archivo .bin.")
            return
        for addr, path in entries:
            if not os.path.isfile(path):
                messagebox.showerror("Archivo", f"No encontrado:\n{path}")
                return

        # El check dice "conservar WiFi", pero escribir en 0x0 o con merged.bin
        # arrasa la NVS igual. Avisar antes de contradecir lo que el usuario pidio.
        if self.keep_nvs_var.get():
            def _wipes_nvs(addr, path):
                try:
                    starts_at_zero = int(addr, 0) == 0
                except ValueError:
                    starts_at_zero = False
                return starts_at_zero or "merged" in os.path.basename(path).lower()

            if any(_wipes_nvs(a, p) for a, p in entries):
                if not messagebox.askyesno("Conservar WiFi", NVS_WARNING):
                    return

        # Modulo virgen + flasheo de solo-app = chip que nunca arranca.
        # Solo se comprueba en el caso peligroso: no hay nada en 0x0 ni en 0x8000.
        def _writes_boot(addr, path):
            """True si esta entrada cubre bootloader (0x0) o tabla (0x8000)."""
            if "merged" in os.path.basename(path).lower():
                return True
            try:
                return int(addr, 0) in (0x0, 0x8000)
            except ValueError:
                return False

        writes_boot = any(_writes_boot(a, p) for a, p in entries)
        if not writes_boot:
            self._log_f("Comprobando tabla de particiones en 0x8000...", "muted")
            self.root.update_idletasks()
            has_table = self._has_partition_table(port)
            if has_table is False:
                if messagebox.askyesno("Modulo nuevo detectado", VIRGIN_WARNING):
                    self._add_merged_full()
                    self._log_f("→ Cambiado a flasheo completo (merged.bin @ 0x0).", "warn")
                    entries = [e.get() for e in self._flash_entries if e.get()]
                    if not entries or not os.path.isfile(entries[0][1]):
                        messagebox.showerror(
                            "merged.bin", "No se encontro merged.bin.\nCompila primero.")
                        return
                else:
                    self._log_f("⚠  Flasheando solo la app en un chip sin tabla de "
                                "particiones — probablemente no arranque.", "warn")
            elif has_table is None:
                self._log_f(f"⚠  No se pudo leer 0x8000 en {port}. Causas tipicas: el puerto "
                            f"no es el ESP32, esta ocupado por el monitor serial, o el modulo "
                            f"no responde. Continuando de todos modos.", "warn")
            else:
                self._log_f("Tabla de particiones OK.", "ok")

        # Sin stub el enlace no negocia baudios altos: forzar 115200 evita
        # fallos raros que parecerian del hardware y son del baudrate.
        baud = "115200" if self.nostub_var.get() else self.baud_var.get()
        if self.nostub_var.get() and self.baud_var.get() != "115200":
            self._log_f("ℹ  Modo sin stub → baud forzado a 115200 (sera lento).", "warn")

        cmd = self._esptool_parts() + [
            "--chip", self.chip_var.get(),
            "--port", port,
            "--baud", baud,
        ]
        if self.nostub_var.get():
            cmd += ["--no-stub"]
        if not self.noreset_var.get():
            # after=no-reset: evita que DTR quede activo manteniendo IO9 bajo
            # El usuario hace power cycle manual tras el flash
            cmd += ["--before", "default-reset", "--after", "no-reset"]
        else:
            cmd += ["--before", "no-reset", "--after", "no-reset"]

        cmd += ["write_flash", "--flash_mode", "dio", "--flash_freq", "80m"]
        for addr, path in entries:
            cmd += [addr, path]

        self._run_flash(cmd)

    def _erase_flash(self):
        port = self._get_port()
        if not port:
            messagebox.showwarning("Puerto", "Selecciona un puerto COM.")
            return
        if not messagebox.askyesno(
                "Confirmar",
                f"Borrar TODA la flash de {port}?\n\n"
                "Incluye la NVS: se pierden el SSID y la contrasena guardados, "
                "y el equipo arrancara en el portal captivo.\n\n"
                "El check «Conservar WiFi» NO aplica a esta operacion."):
            return
        cmd = self._esptool_parts() + ["--chip", self.chip_var.get(),
                                        "--port", port, "--baud", self.baud_var.get(),
                                        "erase_flash"]
        self._run_flash(cmd)

    def _chip_info(self):
        port = self._get_port()
        if not port:
            messagebox.showwarning("Puerto", "Selecciona un puerto COM.")
            return
        cmd = self._esptool_parts() + ["--chip", self.chip_var.get(),
                                        "--port", port, "--baud", self.baud_var.get(),
                                        "flash_id"]
        self._run_flash(cmd)

    def _run_flash(self, cmd):
        if self._running:
            return
        self._running = True
        self.btn_flash.config(state="disabled")
        self.btn_stop_flash.config(state="normal")
        self.flash_progress.start(12)

        cmd_str = " ".join(str(c) for c in cmd)
        self._log_f(f"\n$ {cmd_str}", "muted")

        def worker():
            try:
                self._process = subprocess.Popen(
                    cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
                )
                for line in self._process.stdout:
                    line = line.rstrip()
                    if not line:
                        continue
                    lo = line.lower()
                    tag = "info"
                    if any(w in lo for w in ["error", "failed", "invalid"]):
                        tag = "err"
                    elif "warning" in lo:
                        tag = "warn"
                    elif any(w in lo for w in ["writing", "wrote", "done", "leaving", "hash", "compressed"]):
                        tag = "ok"
                    self.root.after(0, lambda l=line, t=tag: self._log_f(l, t))
                self._process.wait()
                rc = self._process.returncode
                if rc == 0:
                    msg = "\n✅  Flash completado.\n⚠️  Desconecta y reconecta el USB para arrancar el firmware (power cycle)."
                    tag = "ok"
                else:
                    msg = f"\n❌  Código de salida {rc}."
                    tag = "err"
                self.root.after(0, lambda m=msg, t=tag: self._log_f(m, t))
            except Exception as ex:
                self.root.after(0, lambda e=ex: self._log_f(f"❌  {e}", "err"))
            finally:
                self._process = None
                self.root.after(0, self._flash_done)

        threading.Thread(target=worker, daemon=True).start()

    def _flash_done(self):
        self._running = False
        self.btn_flash.config(state="normal")
        self.btn_stop_flash.config(state="disabled")
        self.flash_progress.stop()

    def _stop(self):
        if self._process:
            self._process.terminate()
            self._log_f("⏹  Detenido.", "warn")

    # ── Entradas de archivos ──────────────────────────────────
    def _add_entry(self, addr=None, path=None):
        row = len(self._flash_entries)
        e = FlashEntry(self.files_frame, row, self._remove_entry)
        if addr:
            e.addr_var.set(addr)
        if path:
            e.path_var.set(path)
        self._flash_entries.append(e)

    # ── Deteccion de modulo virgen ────────────────────────────
    def _has_partition_table(self, port):
        """
        Lee 2 bytes en 0x8000 y busca el magic de la tabla de particiones.
        Devuelve True / False, o None si no se pudo determinar.
        """
        import tempfile
        tmp = os.path.join(tempfile.gettempdir(), "_ct32_parttbl.bin")
        cmd = self._esptool_parts() + [
            "--chip", self.chip_var.get(),
            "--port", port,
            "--baud", "115200",          # lento pero fiable para una lectura corta
            "read_flash", PART_TABLE_OFFSET, "2", tmp,
        ]
        try:
            r = subprocess.run(
                cmd, capture_output=True, text=True, timeout=30,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
            )
            if r.returncode != 0 or not os.path.isfile(tmp):
                return None
            with open(tmp, "rb") as f:
                return f.read(2) == PART_TABLE_MAGIC
        except Exception:
            return None
        finally:
            try:
                os.remove(tmp)
            except OSError:
                pass

    def _add_merged_full(self):
        """Flasheo completo: merged.bin @ 0x0. Para modulos nuevos."""
        self.keep_nvs_var.set(False)
        self._clear_entries()
        merged = Path(__file__).parent / "build" / MERGED_BIN_NAME
        self._add_entry("0x0", str(merged) if merged.is_file() else None)
        if not merged.is_file():
            self._log_f(f"⚠  No encontrado {merged} — compila primero.", "warn")

    def _add_app_only(self):
        """Solo la particion de app @ 0x10000 — conserva NVS (WiFi)."""
        self._clear_entries()
        app_bin = Path(__file__).parent / "build" / APP_BIN_NAME
        self._add_entry(APP_OFFSET_DEFAULT,
                        str(app_bin) if app_bin.is_file() else None)
        if not app_bin.is_file():
            self._log_f(f"⚠  No encontrado {app_bin} — compila primero o "
                        f"selecciona el .bin a mano.", "warn")

    def _add_full_flash(self):
        self._clear_entries()
        self._add_entry("0x0")
        self._add_entry("0x8000")
        self._add_entry("0x10000")

    def _remove_entry(self, entry):
        if len(self._flash_entries) <= 1:
            return
        self._flash_entries.remove(entry)
        entry.destroy()
        for i, e in enumerate(self._flash_entries):
            e.frame.grid(row=i, column=0, columnspan=5, sticky="ew", pady=1)

    def _clear_entries(self):
        for e in self._flash_entries:
            e.destroy()
        self._flash_entries.clear()

    # ── Log helpers ───────────────────────────────────────────
    def _make_log(self, parent):
        log = scrolledtext.ScrolledText(
            parent, height=14, bg=CLR_BG, fg=CLR_FG,
            font=("Consolas", 9), relief="flat",
            insertbackground=CLR_FG, wrap="word", state="disabled"
        )
        log.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        log.tag_config("ok",    foreground=CLR_GREEN)
        log.tag_config("err",   foreground=CLR_RED)
        log.tag_config("warn",  foreground=CLR_YELLOW)
        log.tag_config("info",  foreground=CLR_ACCENT)
        log.tag_config("muted", foreground=CLR_MUTED)
        return log

    def _write_log(self, widget, msg, tag="info"):
        widget.config(state="normal")
        widget.insert("end", msg + "\n", tag)
        widget.see("end")
        widget.config(state="disabled")

    def _log_f(self, msg, tag="info"):
        self._write_log(self.flash_log, msg, tag)

    def _log_w(self, msg, tag="info"):
        self._write_log(self.build_log, msg, tag)

    def _clear_log(self, widget):
        widget.config(state="normal")
        widget.delete("1.0", "end")
        widget.config(state="disabled")


# ══════════════════════════════════════════════════════════════
def main():
    root = tk.Tk()
    root.geometry("860x700")

    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except Exception:
        pass
    style.configure("TCombobox",
                    fieldbackground=CLR_BG, background=CLR_SURFACE,
                    foreground=CLR_FG, selectbackground=CLR_SURFACE,
                    selectforeground=CLR_FG)
    style.configure("Horizontal.TProgressbar",
                    troughcolor=CLR_SURFACE, background=CLR_ACCENT)
    style.configure("TSeparator", background=CLR_SURFACE)
    style.configure("TNotebook",  background=CLR_BG, borderwidth=0)
    style.configure("TNotebook.Tab",
                    background=CLR_SURFACE, foreground=CLR_MUTED,
                    padding=[10, 4], font=("Consolas", 9))
    style.map("TNotebook.Tab",
              background=[("selected", CLR_BG)],
              foreground=[("selected", CLR_FG)])

    app = FlasherApp(root)

    def on_closing():
        app._ser_stop.set()   # detener hilo serial si está corriendo
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_closing)
    root.mainloop()


if __name__ == "__main__":
    main()
