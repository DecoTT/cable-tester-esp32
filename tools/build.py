"""
build.py — Compilador automático para ESP32-C3 (arduino-cli)
Cable Tester 32CH / SBC Group

Descarga arduino-cli si no está instalado, instala el core ESP32,
y compila el sketch bring_up.ino.

Uso standalone:
    python build.py

O importado desde flasher.py:
    from build import Builder
    b = Builder(log_callback=print)
    b.run()
"""

import os
import sys
import shutil
import zipfile
import urllib.request
import subprocess
import threading
from pathlib import Path

# ── Configuración ─────────────────────────────────────────────
SKETCH_DIR   = Path(__file__).parent.parent / "firmware" / "bring_up"
BUILD_DIR    = Path(__file__).parent / "build"
CLI_DIR      = Path(__file__).parent / "_arduino-cli"
CLI_EXE      = CLI_DIR / ("arduino-cli.exe" if sys.platform == "win32" else "arduino-cli")

BOARD_URL    = "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
CORE_FQBN    = "esp32:esp32"

# FQBN del ESP32-C3 con USB CDC habilitado (necesario para Serial por USB nativo)
# CDCOnBoot=cdc permite usar Serial.print() a través del USB nativo del ESP32-C3
BOARD_FQBN   = "esp32:esp32:esp32c3"  # CDCOnBoot default → usa hardware USB-Serial/JTAG

# arduino-cli descarga (Windows 64-bit)
CLI_DOWNLOAD = {
    "win32":  "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip",
    "linux":  "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz",
    "darwin": "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_macOS_64bit.tar.gz",
}


# ══════════════════════════════════════════════════════════════
class Builder:
    def __init__(self, log_callback=None, done_callback=None):
        """
        log_callback(msg, tag): función que recibe mensajes de log.
          tag: "info" | "ok" | "err" | "warn" | "muted"
        done_callback(success, bin_files): llamado al terminar.
          bin_files: dict {"bootloader": path, "partitions": path, "app": path}
        """
        self._log  = log_callback or (lambda m, t="info": print(f"[{t}] {m}"))
        self._done = done_callback or (lambda ok, files: None)
        self._stop_flag = False

    def run(self, threaded=True):
        """Inicia la compilación (en hilo separado si threaded=True)."""
        if threaded:
            threading.Thread(target=self._pipeline, daemon=True).start()
        else:
            self._pipeline()

    def stop(self):
        self._stop_flag = True

    # ── Pipeline principal ─────────────────────────────────────
    def _pipeline(self):
        try:
            self._log("═" * 50, "muted")
            self._log("Iniciando pipeline de compilación", "info")
            self._log(f"Sketch: {SKETCH_DIR}", "muted")
            self._log(f"Output: {BUILD_DIR}", "muted")
            self._log("═" * 50, "muted")

            if self._stop_flag: return

            # 1. Obtener arduino-cli
            cli = self._get_cli()
            if not cli:
                self._done(False, {})
                return

            if self._stop_flag: return

            # 2. Configurar URL del core ESP32
            self._configure_core_url(cli)

            if self._stop_flag: return

            # 3. Instalar core ESP32 si no está
            if not self._is_core_installed(cli):
                ok = self._install_core(cli)
                if not ok:
                    self._done(False, {})
                    return
            else:
                self._log("Core esp32:esp32 ya instalado.", "ok")

            if self._stop_flag: return

            # 4. Compilar
            bin_files = self._compile(cli)
            if bin_files:
                self._log("\n✅  Compilación exitosa.", "ok")
                for k, v in bin_files.items():
                    self._log(f"   {k}: {v}", "ok")
                self._done(True, bin_files)
            else:
                self._done(False, {})

        except Exception as ex:
            self._log(f"Error inesperado: {ex}", "err")
            self._done(False, {})

    # ── 1. Obtener arduino-cli ────────────────────────────────
    def _get_cli(self):
        # Primero buscar en PATH
        in_path = shutil.which("arduino-cli")
        if in_path:
            self._log(f"arduino-cli encontrado en PATH: {in_path}", "ok")
            return in_path

        # Luego en CLI_DIR local
        if CLI_EXE.exists():
            self._log(f"arduino-cli local: {CLI_EXE}", "ok")
            return str(CLI_EXE)

        # Descargar
        self._log("arduino-cli no encontrado. Descargando...", "warn")
        return self._download_cli()

    def _download_cli(self):
        platform = sys.platform
        url = CLI_DOWNLOAD.get(platform, CLI_DOWNLOAD["win32"])
        self._log(f"Descargando: {url}", "muted")

        CLI_DIR.mkdir(parents=True, exist_ok=True)
        archive = CLI_DIR / ("arduino-cli.zip" if platform == "win32" else "arduino-cli.tar.gz")

        try:
            def report(count, block, total):
                if total > 0:
                    pct = count * block * 100 // total
                    self._log(f"  Descargando... {min(pct, 100)}%", "muted")

            urllib.request.urlretrieve(url, archive, reporthook=report)
            self._log("Descarga completa. Extrayendo...", "muted")

            if platform == "win32":
                with zipfile.ZipFile(archive, "r") as z:
                    z.extractall(CLI_DIR)
            else:
                import tarfile
                with tarfile.open(archive, "r:gz") as t:
                    t.extractall(CLI_DIR)

            archive.unlink(missing_ok=True)

            if CLI_EXE.exists():
                if platform != "win32":
                    CLI_EXE.chmod(0o755)
                self._log(f"arduino-cli instalado en: {CLI_EXE}", "ok")
                return str(CLI_EXE)
            else:
                self._log("No se encontró arduino-cli.exe tras la extracción.", "err")
                return None

        except Exception as ex:
            self._log(f"Error descargando arduino-cli: {ex}", "err")
            return None

    # ── 2. Configurar URL del core ─────────────────────────────
    def _configure_core_url(self, cli):
        result = self._run([cli, "config", "init", "--overwrite"], capture=True)
        self._run([cli, "config", "add", "board_manager.additional_urls", BOARD_URL],
                  capture=True)
        self._log(f"Board URL configurada.", "muted")

    # ── 3. Verificar / instalar core ──────────────────────────
    def _is_core_installed(self, cli):
        r = self._run([cli, "core", "list"], capture=True)
        return r is not None and "esp32:esp32" in (r.stdout or "")

    def _install_core(self, cli):
        self._log("Instalando core esp32:esp32 (puede tardar 2-5 min)...", "warn")
        self._log("Actualizando índice de plataformas...", "muted")
        self._run([cli, "core", "update-index"], stream=True)

        self._log("Instalando esp32:esp32...", "info")
        ok = self._run([cli, "core", "install", CORE_FQBN,
                        "--additional-urls", BOARD_URL], stream=True)
        if ok:
            self._log("Core esp32:esp32 instalado.", "ok")
        else:
            self._log("Error instalando core esp32:esp32.", "err")
        return ok

    # ── 4. Compilar ───────────────────────────────────────────
    def _compile(self, cli):
        BUILD_DIR.mkdir(parents=True, exist_ok=True)

        self._log(f"\nCompilando para {BOARD_FQBN}...", "info")
        self._log("(Primera compilación puede tardar ~1-2 min mientras se compilan las librerías)", "muted")

        cmd = [
            cli, "compile",
            "--fqbn", BOARD_FQBN,
            "--output-dir", str(BUILD_DIR),
            "--warnings", "default",
            str(SKETCH_DIR),
        ]

        ok = self._run(cmd, stream=True)
        if not ok:
            self._log("Compilación fallida.", "err")
            return None

        # Buscar los .bin generados
        # arduino-cli genera: sketch.ino.merged.bin (completo, 0x0)
        #                     sketch.ino.bootloader.bin (0x0)
        #                     sketch.ino.partitions.bin (0x8000)
        #                     sketch.ino.bin            (app, 0x10000)
        bin_files = {}
        for f in sorted(BUILD_DIR.iterdir()):
            name = f.name.lower()
            if f.suffix != ".bin":
                continue
            if "merged" in name:
                bin_files["merged"] = str(f)
            elif "bootloader" in name:
                bin_files["bootloader"] = str(f)
            elif "partitions" in name:
                bin_files["partitions"] = str(f)
            else:
                bin_files["app"] = str(f)

        if not bin_files:
            self._log("No se encontraron archivos .bin en el directorio de salida.", "err")
            return None

        # Prioridad: merged.bin (un solo archivo a 0x0) > 3 archivos separados
        if "merged" in bin_files:
            self._log("Usando merged.bin (bootloader+partitions+app en un solo archivo).", "info")

        return bin_files

    # ── Helper: ejecutar proceso ──────────────────────────────
    def _run(self, cmd, capture=False, stream=False):
        try:
            if stream:
                proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
                )
                for line in proc.stdout:
                    line = line.rstrip()
                    if not line:
                        continue
                    # Filtrar ruido de PowerShell (política de ejecución de scripts)
                    # arduino-cli llama internamente a PowerShell en Windows; estos mensajes
                    # son advertencias del sistema, no errores de compilación.
                    if any(skip in line for skip in (
                        "profile.ps1", "PSSecurityException", "about_Execution_Policies",
                        "go.microsoft.com", "CategoryInfo", "FullyQualifiedErrorId",
                        "deshabilitada en este", "ejecuci", "UnauthorizedAccess",
                        "En línea:", "En l", "Car cter",
                    )) or line.lstrip().startswith("+ ") or line.lstrip().startswith(". "):
                        continue
                    lo = line.lower()
                    tag = "muted"
                    if "error" in lo and "psecurity" not in lo:
                        tag = "err"
                    elif "warning" in lo:
                        tag = "warn"
                    elif any(w in lo for w in ["sketch", "used", "maximum", "compiling", "installing"]):
                        tag = "info"
                    self._log(line, tag)
                    if self._stop_flag:
                        proc.terminate()
                        return False
                proc.wait()
                return proc.returncode == 0

            elif capture:
                r = subprocess.run(
                    cmd, capture_output=True, text=True, timeout=30,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
                )
                return r
            else:
                subprocess.run(cmd, check=True,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
                return True

        except subprocess.CalledProcessError:
            return False
        except Exception as ex:
            self._log(f"Error ejecutando: {' '.join(str(c) for c in cmd[:3])}... → {ex}", "err")
            return False


# ── Standalone ────────────────────────────────────────────────
if __name__ == "__main__":
    import time

    def log(msg, tag="info"):
        prefix = {"ok": "✓", "err": "✗", "warn": "!", "muted": " ", "info": "→"}.get(tag, " ")
        print(f"{prefix} {msg}")

    done_event = threading.Event()
    result = {}

    def done(ok, files):
        result["ok"]    = ok
        result["files"] = files
        done_event.set()

    b = Builder(log_callback=log, done_callback=done)
    b.run(threaded=True)
    done_event.wait()

    if result.get("ok"):
        print(f"\nBinarios en: {BUILD_DIR}")
        for k, v in result["files"].items():
            print(f"  {k:12s} → {v}")
        sys.exit(0)
    else:
        print("\nCompilación fallida.")
        sys.exit(1)
