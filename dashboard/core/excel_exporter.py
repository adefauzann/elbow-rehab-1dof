"""
core/excel_exporter.py
Helper class untuk export data sesi rehabilitasi ke file Excel (.xlsx).

Referensi dokumen: docs/05_FEATURE_SESSION_HISTORY.md (bagian Fitur Export ke Excel)

Struktur file Excel yang dihasilkan:
  Sheet 1 — "Riwayat Sesi Rehabilitasi"
    Baris 1-3 : Header dokumen (judul, subtitle, tanggal export)
    Baris 5   : Header kolom (bold, background biru gelap)
    Baris 6+  : Data dengan color coding kategori dan status
    Baris n+2 : Statistik ringkasan

  Sheet 2 — "Statistik"
    Berbagai metrik agregat sesi
"""

import pandas as pd
from datetime import datetime
from typing import Optional

try:
    import openpyxl
    from openpyxl.styles import (
        PatternFill, Font, Alignment, Border, Side, GradientFill
    )
    from openpyxl.utils import get_column_letter
    OPENPYXL_AVAILABLE = True
except ImportError:
    OPENPYXL_AVAILABLE = False


CATEGORY_COLORS = {
    "Sangat Baik": "66bb6a",
    "Baik"       : "26c6da",
    "Cukup"      : "ffd54f",
    "Kurang"     : "ffa726",
    "Buruk"      : "ef5350",
}

STATUS_COLORS = {
    "Selesai"  : "66bb6a",
    "Terhenti" : "ffd54f",
    "Error"    : "ef5350",
}

COLUMNS_ORDER = [
    "no", "sudut_target", "repetisi_target", "repetisi_selesai",
    "median_pretest", "median_posttest", "delta_perkembangan",
    "skor_fuzzy", "kategori_hasil", "status_sesi",
]

COLUMN_HEADERS = [
    "No.", "Sudut Target (°)", "Rep. Target", "Rep. Selesai",
    "Median Pre-Test (°)", "Median Post-Test (°)", "Delta ROM (°)",
    "Skor Fuzzy", "Kategori Hasil", "Status Sesi",
]

FLOAT_COLS = {"sudut_target", "median_pretest", "median_posttest",
              "delta_perkembangan", "skor_fuzzy"}


class ExcelExporter:
    """
    Export DataFrame sesi rehabilitasi ke file Excel terformat.
    Lihat docs/05_FEATURE_SESSION_HISTORY.md untuk spesifikasi lengkap.
    """

    def export(self, df: pd.DataFrame, filepath: str) -> bool:
        """
        Export DataFrame ke file Excel.
        Return True jika berhasil, False jika gagal.
        """
        if not OPENPYXL_AVAILABLE:
            print("ERROR: openpyxl tidak terinstall. Jalankan: pip install openpyxl")
            return False

        try:
            wb = openpyxl.Workbook()
            ws = wb.active
            ws.title = "Riwayat Sesi Rehabilitasi"

            export_time = datetime.now().strftime("%d/%m/%Y %H:%M:%S")

            data_start_row = self._write_header_section(ws, export_time)
            self._write_data_section(ws, df, data_start_row)
            self._write_statistics_sheet(wb, df)
            self._auto_column_width(ws)

            wb.save(filepath)
            return True

        except Exception as e:
            print(f"ERROR saat export Excel: {e}")
            return False

    # ------------------------------------------------------------------
    # Private: Sheet 1
    # ------------------------------------------------------------------

    def _write_header_section(self, ws, export_time: str) -> int:
        """
        Tulis header dokumen di baris 1-3.
        Return nomor baris pertama data (header kolom).
        """
        title_font  = Font(name="Segoe UI", bold=True, size=14, color="FFFFFF")
        sub_font    = Font(name="Segoe UI", size=10, color="B0BEC5")
        date_font   = Font(name="Segoe UI", size=9, italic=True, color="90A4AE")
        bg_header   = PatternFill("solid", fgColor="0D1117")

        # Merge sel untuk judul
        ws.merge_cells("A1:J1")
        ws.merge_cells("A2:J2")
        ws.merge_cells("A3:J3")

        ws["A1"] = "LAPORAN RIWAYAT SESI REHABILITASI SENDI SIKU"
        ws["A1"].font      = title_font
        ws["A1"].fill      = bg_header
        ws["A1"].alignment = Alignment(horizontal="center", vertical="center")
        ws.row_dimensions[1].height = 28

        ws["A2"] = "Sistem Fuzzy Mamdani  |  Motor Stepper Closed-Loop"
        ws["A2"].font      = sub_font
        ws["A2"].fill      = bg_header
        ws["A2"].alignment = Alignment(horizontal="center", vertical="center")

        ws["A3"] = f"Diekspor pada: {export_time}"
        ws["A3"].font      = date_font
        ws["A3"].fill      = bg_header
        ws["A3"].alignment = Alignment(horizontal="center", vertical="center")

        # Baris kosong
        ws.row_dimensions[4].height = 6

        # Header kolom di baris 5
        header_font = Font(name="Segoe UI", bold=True, size=10, color="FFFFFF")
        header_fill = PatternFill("solid", fgColor="1F6FEB")
        thin_border = Border(
            left=Side(style="thin", color="30363D"),
            right=Side(style="thin", color="30363D"),
            bottom=Side(style="thin", color="30363D"),
        )

        for col_idx, header_text in enumerate(COLUMN_HEADERS, start=1):
            cell = ws.cell(row=5, column=col_idx, value=header_text)
            cell.font      = header_font
            cell.fill      = header_fill
            cell.alignment = Alignment(horizontal="center", vertical="center")
            cell.border    = thin_border
        ws.row_dimensions[5].height = 22

        return 6   # Data mulai baris 6

    def _write_data_section(self, ws, df: pd.DataFrame, start_row: int) -> None:
        """Tulis baris data dengan formatting per kolom dan color coding."""
        fill_even = PatternFill("solid", fgColor="0D1117")
        fill_odd  = PatternFill("solid", fgColor="161B22")
        data_font = Font(name="Consolas", size=10, color="C9D1D9")
        badge_font = Font(name="Segoe UI", bold=True, size=10)
        center_align = Alignment(horizontal="center", vertical="center")
        right_align  = Alignment(horizontal="right",  vertical="center")
        thin_border  = Border(
            left=Side(style="thin", color="21262D"),
            right=Side(style="thin", color="21262D"),
            bottom=Side(style="thin", color="21262D"),
        )

        for row_offset, (_, row_data) in enumerate(df[COLUMNS_ORDER].iterrows()):
            row_num  = start_row + row_offset
            row_fill = fill_even if row_offset % 2 == 0 else fill_odd

            ws.row_dimensions[row_num].height = 20

            for col_idx, field in enumerate(COLUMNS_ORDER, start=1):
                value = row_data.get(field, "")

                # Format angka float
                if field in FLOAT_COLS:
                    try:
                        value = round(float(value), 2)
                    except (ValueError, TypeError):
                        pass

                cell = ws.cell(row=row_num, column=col_idx, value=value)
                cell.fill   = row_fill
                cell.border = thin_border
                cell.font   = data_font

                if field in FLOAT_COLS or field == "no":
                    cell.alignment = right_align
                    if field in FLOAT_COLS:
                        cell.number_format = "0.00"
                else:
                    cell.alignment = center_align

                # Color coding kategori dan status
                if field == "kategori_hasil":
                    color = CATEGORY_COLORS.get(str(value), "C9D1D9")
                    cell.font = Font(name="Segoe UI", bold=True, size=10, color=color)
                elif field == "status_sesi":
                    color = STATUS_COLORS.get(str(value), "C9D1D9")
                    cell.font = Font(name="Segoe UI", bold=True, size=10, color=color)

        # Statistik ringkasan di bawah tabel
        summary_row = start_row + len(df) + 1
        ws.merge_cells(f"A{summary_row}:J{summary_row}")
        summary_text = (
            f"Total Sesi: {len(df)}  |  "
            f"Rata-rata Skor Fuzzy: {df['skor_fuzzy'].mean():.1f}  |  "
            f"Rata-rata Delta ROM: {df['delta_perkembangan'].mean():.1f}°"
        )
        cell = ws.cell(row=summary_row, column=1, value=summary_text)
        cell.font      = Font(name="Segoe UI", italic=True, size=9, color="8B949E")
        cell.fill      = PatternFill("solid", fgColor="0D1117")
        cell.alignment = Alignment(horizontal="center", vertical="center")

    def _write_statistics_sheet(self, wb, df: pd.DataFrame) -> None:
        """Buat sheet statistik ringkasan (Sheet 2)."""
        ws = wb.create_sheet(title="Statistik")

        header_font = Font(name="Segoe UI", bold=True, size=11, color="FFFFFF")
        header_fill = PatternFill("solid", fgColor="1F6FEB")
        data_font   = Font(name="Segoe UI", size=10, color="C9D1D9")
        data_fill   = PatternFill("solid", fgColor="0D1117")
        center      = Alignment(horizontal="center", vertical="center")

        ws["A1"] = "STATISTIK RINGKASAN SESI REHABILITASI"
        ws["A1"].font = Font(name="Segoe UI", bold=True, size=13, color="FFFFFF")
        ws.merge_cells("A1:C1")
        ws["A1"].fill = PatternFill("solid", fgColor="0D1117")
        ws["A1"].alignment = center

        stats = [
            ("Total Sesi",               len(df)),
            ("Sesi Selesai",             (df["status_sesi"] == "Selesai").sum()),
            ("Sesi Terhenti",            (df["status_sesi"] == "Terhenti").sum()),
            ("Rata-rata Skor Fuzzy",     round(df["skor_fuzzy"].mean(), 2)),
            ("Skor Fuzzy Tertinggi",     df["skor_fuzzy"].max()),
            ("Skor Fuzzy Terendah",      df["skor_fuzzy"].min()),
            ("Rata-rata Delta ROM (°)",  round(df["delta_perkembangan"].mean(), 2)),
            ("Delta ROM Terbesar (°)",   df["delta_perkembangan"].max()),
        ]

        # Distribusi kategori
        if "kategori_hasil" in df.columns:
            for kat, count in df["kategori_hasil"].value_counts().items():
                stats.append((f"Kategori: {kat}", count))

        for i, (label, value) in enumerate(stats, start=3):
            ws.cell(row=i, column=1, value=label).font = data_font
            ws.cell(row=i, column=1).fill = data_fill
            ws.cell(row=i, column=2, value=value).font = data_font
            ws.cell(row=i, column=2).fill = data_fill
            ws.cell(row=i, column=2).alignment = center

        ws.column_dimensions["A"].width = 35
        ws.column_dimensions["B"].width = 20

    def _auto_column_width(self, ws) -> None:
        """Auto-adjust lebar kolom berdasarkan konten terpanjang."""
        for col_cells in ws.columns:
            max_len = 0
            col_letter = get_column_letter(col_cells[0].column)
            for cell in col_cells:
                if cell.value:
                    max_len = max(max_len, len(str(cell.value)))
            ws.column_dimensions[col_letter].width = min(max(max_len + 2, 8), 30)
