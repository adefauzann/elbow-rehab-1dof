"""
main.py
Backend server FastAPI untuk Dashboard Web Monitoring Rehabilitasi Sendi Siku 1-DOF.
Mendukung koneksi dual-mode: USB (Serial COM) dan Nirkabel (WiFi TCP Client dari ESP32).
"""

import asyncio
import json
import socket
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse
from pydantic import BaseModel
import pandas as pd
import io
import os
from datetime import datetime

from core.serial_manager import SerialManager
from core.excel_exporter import ExcelExporter

app = FastAPI(title="Rehabilitasi Sendi Siku Dashboard Backend")

# Inisialisasi manajer
serial_mgr = SerialManager()
excel_exporter = ExcelExporter()

# Menyimpan koneksi WebSocket aktif (Frontend ke Backend)
active_websockets: list[WebSocket] = []

# Buffer untuk menampung baris CSV yang dikirim dari ESP32
csv_buffer: list[str] = []
csv_receiving = False

# TCP Server State (untuk Koneksi WiFi dari ESP32)
tcp_server = None
tcp_client_writer = None
wifi_connected = False


# ------------------------------------------------------------------
# Request Models
# ------------------------------------------------------------------

class ConnectRequest(BaseModel):
    port: str
    baud_rate: int = 115200


class ExportRequest(BaseModel):
    data: list[dict]


# Helper untuk mendapatkan IP lokal komputer
def get_local_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.254.254.254', 1))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip


# ------------------------------------------------------------------
# Setup Callbacks Serial & TCP
# ------------------------------------------------------------------

# Helper untuk menjalankan async broadcast dari thread callback
def run_coroutine_from_thread(coro):
    try:
        loop = asyncio.get_event_loop()
    except RuntimeError:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)

    if loop.is_running():
        asyncio.run_coroutine_threadsafe(coro, loop)
    else:
        loop.run_until_complete(coro)


def on_realtime_data(data: dict):
    """Callback saat data JSON real-time diterima (dari USB maupun WiFi)."""
    run_coroutine_from_thread(broadcast_json({"type": "realtime", "data": data}))


def on_raw_line(line: str):
    """Callback saat baris teks mentah diterima (untuk parsing CSV)."""
    global csv_receiving, csv_buffer
    if line == "CSV_START":
        csv_buffer.clear()
        csv_receiving = True
        run_coroutine_from_thread(broadcast_json({"type": "csv_status", "status": "receiving"}))
    elif line == "CSV_END" and csv_receiving:
        csv_receiving = False
        run_coroutine_from_thread(broadcast_json({"type": "csv_status", "status": "finished"}))
        try:
            raw_csv = "\n".join(csv_buffer)
            df = pd.read_csv(io.StringIO(raw_csv))
            data_dict = df.to_dict(orient="records")
            run_coroutine_from_thread(broadcast_json({
                "type": "csv_data",
                "data": data_dict
            }))
        except Exception as e:
            run_coroutine_from_thread(broadcast_json({
                "type": "csv_status",
                "status": "error",
                "message": f"Gagal parse CSV: {e}"
            }))
    elif csv_receiving:
        csv_buffer.append(line)


def on_serial_error(error_msg: str):
    """Callback saat terjadi error koneksi serial."""
    run_coroutine_from_thread(broadcast_json({"type": "error", "message": error_msg}))


# Set callbacks ke manager
serial_mgr.on_data_callback = on_realtime_data
serial_mgr.on_raw_callback = on_raw_line
serial_mgr.on_error_callback = on_serial_error





# ------------------------------------------------------------------
# WebSocket Broadcast
# ------------------------------------------------------------------

async def broadcast_json(message: dict):
    """Kirim pesan JSON ke semua websocket client (frontend)."""
    disconnected = []
    for ws in active_websockets:
        try:
            await ws.send_json(message)
        except Exception:
            disconnected.append(ws)

    for ws in disconnected:
        if ws in active_websockets:
            active_websockets.remove(ws)


# ------------------------------------------------------------------
# TCP Server (Koneksi WiFi ESP32)
# ------------------------------------------------------------------

async def handle_tcp_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """Handler untuk koneksi TCP masuk dari ESP32 C++."""
    global tcp_client_writer, wifi_connected
    tcp_client_writer = writer
    wifi_connected = True
    
    # Broadcast status ke frontend
    await broadcast_json({
        "type": "wifi_status",
        "connected": True,
        "client_ip": writer.get_extra_info('peername')[0]
    })

    try:
        while True:
            data = await reader.readline()
            if not data:
                break
            line = data.decode("utf-8", errors="replace").strip()
            if line:
                # Teruskan baris teks mentah (untuk deteksi CSV_START/CSV_END)
                on_raw_line(line)
                # Coba parse JSON real-time
                if line.startswith("{"):
                    try:
                        parsed_data = json.loads(line)
                        on_realtime_data(parsed_data)
                    except json.JSONDecodeError:
                        pass
    except Exception as e:
        print(f"WiFi Connection Error: {e}")
    finally:
        wifi_connected = False
        tcp_client_writer = None
        writer.close()
        await writer.wait_closed()
        await broadcast_json({
            "type": "wifi_status",
            "connected": False,
            "client_ip": None
        })


async def start_tcp_server():
    """Memulai server TCP nirkabel di background."""
    global tcp_server
    # Dengarkan di port 9000
    tcp_server = await asyncio.start_server(handle_tcp_client, '0.0.0.0', 9000)
    print(f"TCP Server (WiFi ESP32) running on 0.0.0.0:9000")


# Hubungkan startup event FastAPI untuk menjalankan TCP server
@app.on_event("startup")
async def startup_event():
    asyncio.create_task(start_tcp_server())


# ------------------------------------------------------------------
# WebSocket Endpoint
# ------------------------------------------------------------------

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    active_websockets.append(websocket)
    
    # Kirim status inisial ke client frontend
    await websocket.send_json({
        "type": "status",
        "connected": serial_mgr.is_connected,
        "port": serial_mgr.port,
        "baud_rate": serial_mgr.baud_rate,
        "wifi_connected": wifi_connected,
        "local_ip": get_local_ip(),
        "tcp_port": 9000
    })

    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        if websocket in active_websockets:
            active_websockets.remove(websocket)


# ------------------------------------------------------------------
# REST API Endpoints
# ------------------------------------------------------------------

@app.get("/api/ports")
def get_ports():
    return {
        "ports": serial_mgr.list_ports(),
        "local_ip": get_local_ip(),
        "tcp_port": 9000
    }


@app.post("/api/connect")
async def connect_serial(req: ConnectRequest):
    success = await serial_mgr.connect(req.port, req.baud_rate)
    if success:
        await broadcast_json({
            "type": "status",
            "connected": True,
            "port": req.port,
            "baud_rate": req.baud_rate
        })
        return {"status": "connected", "port": req.port}
    else:
        raise HTTPException(status_code=500, detail="Gagal terhubung ke port serial.")


@app.post("/api/disconnect")
async def disconnect_serial():
    await serial_mgr.disconnect()
    await broadcast_json({
        "type": "status",
        "connected": False,
        "port": None,
        "baud_rate": None
    })
    return {"status": "disconnected"}


@app.get("/api/status")
def get_status():
    return {
        "connected": serial_mgr.is_connected,
        "port": serial_mgr.port,
        "baud_rate": serial_mgr.baud_rate,
        "wifi_connected": wifi_connected,
        "local_ip": get_local_ip(),
        "tcp_port": 9000
    }


@app.post("/api/request-csv")
def request_csv():
    """Mengirim perintah GET_CSV ke ESP32 (bisa via USB/Serial atau via WiFi/TCP)."""
    global tcp_client_writer
    cmd = "CMD:GET_CSV\n"
    
    if wifi_connected and tcp_client_writer:
        # Kirim via WiFi TCP socket
        try:
            tcp_client_writer.write(cmd.encode("utf-8"))
            return {"status": "requested_via_wifi"}
        except Exception as e:
            raise HTTPException(status_code=500, detail=f"Gagal mengirim permintaan via WiFi: {e}")
            
    elif serial_mgr.is_connected:
        # Kirim via Serial USB
        serial_mgr.send_command(cmd)
        return {"status": "requested_via_usb"}
        
    else:
        raise HTTPException(status_code=400, detail="Tidak ada koneksi ESP32 aktif (USB maupun WiFi).")


@app.post("/api/export")
def export_excel(req: ExportRequest):
    if not req.data:
        raise HTTPException(status_code=400, detail="Data kosong.")

    try:
        df = pd.DataFrame(req.data)
        temp_dir = os.path.join(os.path.dirname(__file__), "scratch")
        os.makedirs(temp_dir, exist_ok=True)
        filename = f"riwayat_rehabilitasi_{datetime.now().strftime('%Y%m%d_%H%M%S')}.xlsx"
        filepath = os.path.join(temp_dir, filename)

        success = excel_exporter.export(df, filepath)
        if not success:
            raise HTTPException(status_code=500, detail="Gagal membuat file Excel.")

        return FileResponse(
            path=filepath,
            filename=filename,
            media_type="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
        )
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Gagal melakukan eksport: {e}")


# ------------------------------------------------------------------
# Mount Static Files (Frontend HTML/CSS/JS)
# ------------------------------------------------------------------
os.makedirs(os.path.join(os.path.dirname(__file__), "static"), exist_ok=True)
app.mount("/", StaticFiles(directory="static", html=True), name="static")


if __name__ == "__main__":
    import uvicorn
    # Jalankan uvicorn server
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
