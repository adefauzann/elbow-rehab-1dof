"""
core/serial_manager.py
Mengelola koneksi serial ke ESP32 secara asynchronous.

Tanggung jawab:
  - Scan port serial yang tersedia
  - Membuka/menutup port serial
  - Membaca data secara non-blocking di background task
  - Mengirim perintah ke ESP32
  - Memanggil callback (atau broadcast) ketika data diterima
"""

import asyncio
import json
import serial
import serial.tools.list_ports
from typing import Callable, List, Optional


class SerialManager:
    """
    Manajer koneksi serial untuk FastAPI.
    """

    def __init__(self):
        self.port: Optional[str] = None
        self.baud_rate: int = 115200
        self._serial: Optional[serial.Serial] = None
        self._read_task: Optional[asyncio.Task] = None
        self.on_data_callback: Optional[Callable[[dict], None]] = None
        self.on_raw_callback: Optional[Callable[[str], None]] = None
        self.on_error_callback: Optional[Callable[[str], None]] = None
        self.is_connected = False

    def list_ports(self) -> List[str]:
        """Scan dan kembalikan daftar port serial yang tersedia."""
        return [p.device for p in serial.tools.list_ports.comports()]

    async def connect(self, port: str, baud_rate: int = 115200) -> bool:
        """Buka port serial dan mulai membaca di background task."""
        if self.is_connected:
            await self.disconnect()

        self.port = port
        self.baud_rate = baud_rate

        try:
            # Membuka serial secara non-blocking (timeout kecil)
            self._serial = serial.Serial(
                port=self.port,
                baudrate=self.baud_rate,
                timeout=0.1
            )
            self.is_connected = True
            # Jalankan loop pembacaan di background task
            self._read_task = asyncio.create_task(self._read_loop())
            return True
        except Exception as e:
            if self.on_error_callback:
                self.on_error_callback(str(e))
            self._close_resources()
            return False

    async def disconnect(self):
        """Hentikan background task dan tutup port serial."""
        self.is_connected = False
        if self._read_task:
            self._read_task.cancel()
            try:
                await self._read_task
            except asyncio.CancelledError:
                pass
            self._read_task = None
        self._close_resources()

    def send_command(self, command: str):
        """Kirim perintah string ke ESP32."""
        if self._serial and self._serial.is_open:
            try:
                self._serial.write(command.encode("utf-8"))
            except Exception as e:
                if self.on_error_callback:
                    self.on_error_callback(f"Gagal mengirim perintah: {e}")

    # ------------------------------------------------------------------
    # Private
    # ------------------------------------------------------------------

    async def _read_loop(self):
        """Loop asinkron untuk membaca baris dari serial."""
        loop = asyncio.get_running_loop()
        while self.is_connected:
            if self._serial and self._serial.is_open:
                try:
                    # Jalankan operasi I/O serial block di threadpool agar tidak memblokir event loop
                    raw_bytes = await loop.run_in_executor(
                        None, self._serial.readline
                    )
                    if raw_bytes:
                        line = raw_bytes.decode("utf-8", errors="replace").strip()
                        if line:
                            if self.on_raw_callback:
                                self.on_raw_callback(line)
                            self._try_parse_json(line)
                except Exception as e:
                    if self.is_connected: # Abaikan error jika sengaja di-disconnect
                        if self.on_error_callback:
                            self.on_error_callback(f"Koneksi serial terputus: {e}")
                        self.is_connected = False
                        break
            await asyncio.sleep(0.01) # Mencegah penggunaan CPU berlebih

        self._close_resources()

    def _try_parse_json(self, raw: str):
        """Coba parse string JSON untuk data real-time."""
        if raw.startswith("{"):
            try:
                data = json.loads(raw)
                if self.on_data_callback:
                    self.on_data_callback(data)
            except json.JSONDecodeError:
                pass

    def _close_resources(self):
        """Tutup port serial dengan aman."""
        try:
            if self._serial and self._serial.is_open:
                self._serial.close()
        except Exception:
            pass
        self._serial = None
        self.is_connected = False
