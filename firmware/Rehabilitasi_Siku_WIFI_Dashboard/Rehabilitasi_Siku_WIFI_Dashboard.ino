#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClient.h>

/*
  ================================================================
  SISTEM REHABILITASI SENDI SIKU 1-DOF - FINAL V8 WIFI
  ESP32-C6 + HBS860H + AS5600 + LCD 20x4 + LittleFS + WiFi
  ================================================================

  FITUR UTAMA:
  1. Boot-up/sambutan nama alat.
  2. Menu LCD:
       - LATIHAN
       - UJI TARGET
       - PANTAU SUDUT
       - LIHAT DATA
       - KELOLA DATA
  3. LATIHAN:
       - Zero dengan tombol OK.
       - Pre-test 3 kali, nilai maksimum dikonfirmasi tombol OK.
       - Latihan target tetap, 10 repetisi.
       - Istirahat tetap sebelum post-test.
       - Post-test 3 kali, nilai maksimum dikonfirmasi tombol OK.
       - Fuzzy Mamdani, lalu simpan LittleFS.
  4. Nomor data biasa: Data 001, Data 002, dan seterusnya.
  5. Tidak menyimpan data setiap repetisi.
  6. PANTAU SUDUT membaca gerak dari 0 derajat sampai sudut maksimum nyaman;
     nilai maksimum tersebut langsung menjadi target LATIHAN.
  7. Lihat data LittleFS langsung pada LCD.
  8. Hapus satu data, format LittleFS, dan info penyimpanan.
  9. Tombol STOP aktif selama motor bergerak.
 10. Limit switch NC dan pemeriksaan arah gerak.
 11. Serial Monitor diagnostik lengkap: manual naik/turun, target, zero,
     motor ON/OFF, data LittleFS, serta monitor magnet AS5600 live.
 12. Dashboard WiFi TCP: data real-time, status alat, kontrol aman, dan
     pengiriman riwayat CSV ke backend FastAPI.
 13. Telemetri dashboard dilengkapi error_sensor dalam derajat dan
     error_motor dalam satuan step/pulsa motor.

  PEMETAAN TOMBOL PADA INTERFACE:
    MODE  = menu berikut / tambah nilai
    ZERO  = menu sebelumnya / kurangi nilai
    OK    = pilih / konfirmasi / simpan sudut
    BACK  = batal / kembali
    STOP  = hentikan motor / keselamatan

  CATATAN:
  - Tombol dipasang antara GPIO dan GND, memakai INPUT_PULLUP.
  - Emergency stop hardware tetap harus memutus suplai 48 V driver.
  - Uji tanpa pengguna dan tanpa beban sebelum digunakan pada subjek.
*/

// ================================================================
// PIN ESP32-C6
// ================================================================
#define STEP_PIN       18
#define DIR_PIN        19
#define ENA_PIN        20

#define BTN_BACK        1   // BACK / batal / kembali
#define BTN_STOP       10   // STOP keselamatan
#define BTN_MODE       11   // menu berikut / tambah nilai
#define BTN_OK         12   // OK / konfirmasi
#define BTN_ZERO       13   // menu sebelumnya / kurangi nilai

#define LIMIT_ATAS      2   // NC, normal LOW, aktif HIGH
#define LIMIT_BAWAH     3   // NC, normal LOW, aktif HIGH

#define SDA_PIN         4
#define SCL_PIN         5

// ================================================================
// AS5600
// ================================================================
#define AS5600_ADDR      0x36
#define REG_STATUS       0x0B
#define REG_RAW_ANGLE_H  0x0C
#define REG_AGC          0x1A
#define REG_MAGNITUDE_H  0x1B

// ================================================================
// WIFI DAN DASHBOARD
// ================================================================
// Ganti tiga nilai berikut sebelum upload ke ESP32-C6.
// DASHBOARD_HOST harus sama dengan IP lokal komputer yang menjalankan
// main_wifi_dashboard.py. IP tersebut tampil pada terminal backend dan
// endpoint /api/status.
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char *DASHBOARD_HOST = "YOUR_PC_IP";
const uint16_t DASHBOARD_PORT = 9000;

const unsigned long INTERVAL_RECONNECT_WIFI_MS = 10000UL;
const unsigned long INTERVAL_RECONNECT_DASHBOARD_MS = 3000UL;
const unsigned long INTERVAL_REALTIME_DASHBOARD_MS = 250UL;
const unsigned long INTERVAL_SENSOR_IDLE_DASHBOARD_MS = 300UL;
const size_t BATAS_BUFFER_PERINTAH_WIFI = 180;

// ================================================================
// DRIVER HBS860H DAN ARAH
// ================================================================
#define MOTOR_HIDUP_LEVEL LOW
#define MOTOR_MATI_LEVEL  HIGH

// Sesuai versi terakhir yang sudah diuji pada alat.
// Bila arah motor terbalik, ubah 1 menjadi 0.
#define BALIK_ARAH_MOTOR 1

// Sesuai hasil uji terakhir: pembacaan sensor dibalik.
// Bila sudut naik sudah positif tanpa pembalikan, ubah menjadi 0.
#define BALIK_ARAH_SENSOR 1

#if BALIK_ARAH_MOTOR
  #define DIR_NAIK  HIGH
  #define DIR_TURUN LOW
#else
  #define DIR_NAIK  LOW
  #define DIR_TURUN HIGH
#endif

const float TANDA_ARAH_SENSOR = BALIK_ARAH_SENSOR ? -1.0f : 1.0f;

// ================================================================
// PARAMETER FINAL PENGUJIAN
// Ubah hanya sebelum seluruh rangkaian pengambilan data dimulai.
// Jangan diubah di tengah pengambilan data.
// ================================================================
const float TARGET_LATIHAN_DEFAULT = 60.0f;      // derajat
const uint16_t REPETISI_LATIHAN = 10;           // naik-turun
const unsigned long WAKTU_TAHAN_TARGET_MS = 1000UL;
const unsigned long JEDA_ANTAR_REPETISI_MS = 1500UL;
const unsigned long WAKTU_ISTIRAHAT_POST_MS = 120000UL; // 2 menit

const float TARGET_MIN = 0.0f;
const float TARGET_MAX = 100.0f;
const float TOLERANSI_SUDUT = 1.0f;
const float SUDUT_SIAP_TEST = 5.0f;
const unsigned long TIMEOUT_GERAK_MS = 60000UL;
const unsigned long DEBOUNCE_MS = 45UL;
const unsigned long DELAY_HOLDING_LIMIT_BAWAH_MS = 1000UL;

// DIP HBS860H = 1600 pulse/revolution, pulley 20T:60T = 1:3.
const uint16_t PULSA_PER_PUTARAN_MOTOR = 1600;
const float RASIO_REDUKSI = 3.0f;
const float PULSA_PER_DERAJAT_OUTPUT =
  (PULSA_PER_PUTARAN_MOTOR * RASIO_REDUKSI) / 360.0f;

// Profil gerak halus V2.
// Kecepatan dibuat sedikit lebih rendah dan perubahan periode dibuat lebih
// bertahap agar motor tidak menghentak pada awal, tengah, dan akhir gerakan.
const uint32_t PERIODE_PULSA_MULAI_US = 12000UL;
const uint32_t PERIODE_PULSA_CEPAT_US = 4500UL;
const uint32_t PERIODE_PULSA_DEKAT_US = 10500UL;
const uint32_t PERUBAHAN_PERIODE_PER_PULSA_US = 25UL;

// Pembacaan AS5600 tidak dilakukan terlalu sering saat motor masih jauh dari
// target. Ini mengurangi jeda I2C di antara pulsa STEP.
const uint8_t UPDATE_SENSOR_JAUH = 18;
const uint8_t UPDATE_SENSOR_SEDANG = 8;
const uint8_t UPDATE_SENSOR_DEKAT = 3;

// Filter gerak tetap cukup responsif untuk menghentikan motor di dekat target.
const float ALPHA_FILTER_GERAK = 0.38f;
const float BATAS_LONCATAN_SENSOR_DEG = 5.0f;
const uint8_t BATAS_GAGAL_SENSOR_BERURUTAN = 10;
const unsigned long INTERVAL_LCD_GERAK_MS = 600UL;

// Penguatan komunikasi AS5600.
const uint8_t AS5600_RETRY_I2C = 3;
const uint8_t AS5600_SAMPEL_GERAK = 3;
const uint16_t AS5600_TIMEOUT_MS = 8;


// ================================================================
// PARAMETER DIAGNOSTIK SERIAL SAJA
// Tidak mengubah menu maupun parameter latihan pada LCD.
// ================================================================
uint32_t pulsaManualSerial = 200UL;
uint32_t periodePulsaManualSerialUs = 6500UL;
const uint32_t PULSA_MANUAL_MIN = 1UL;
const uint32_t PULSA_MANUAL_MAX = 20000UL;
const uint32_t PERIODE_MANUAL_MIN_US = 3000UL;
const uint32_t PERIODE_MANUAL_MAX_US = 20000UL;
const unsigned long INTERVAL_MONITOR_MAGNET_MS = 250UL;

// ================================================================
// LITTLEFS
// ================================================================
const char *FILE_DATA = "/data_latihan.csv";
const char *FILE_TEMP = "/session_temp.csv";
const char *FILE_LOG  = "/log_sistem.csv";
const char *FILE_HAPUS_TEMP = "/data_hapus_temp.csv";
const char *FILE_CONFIG = "/config_alat.txt";

const char *HEADER_DATA =
  "nomor,target,rep_target,rep_selesai,"
  "pre1,pre2,pre3,median_pre,"
  "post1,post2,post3,median_post,"
  "delta,skor_fuzzy,kategori,status";

bool littleFSSiap = false;

// ================================================================
// LCD
// ================================================================
LiquidCrystal_I2C *lcd = nullptr;
bool lcdSiap = false;
uint8_t alamatLCD = 0;

// ================================================================
// ENUM DAN STRUKTUR DATA
// ================================================================
enum MenuUtama : uint8_t {
  MENU_LATIHAN = 0,
  MENU_UJI_TARGET,
  MENU_PANTAU_SUDUT,
  MENU_LIHAT_DATA,
  MENU_KELOLA_DATA,
  JUMLAH_MENU_UTAMA
};

enum MenuKelola : uint8_t {
  KELOLA_HAPUS_SATU = 0,
  KELOLA_FORMAT,
  KELOLA_INFO,
  JUMLAH_MENU_KELOLA
};

enum HasilGerak : uint8_t {
  GERAK_BERHASIL = 0,
  GERAK_STOP,
  GERAK_LIMIT_ATAS,
  GERAK_LIMIT_BAWAH,
  GERAK_SENSOR_ERROR,
  GERAK_ARAH_SALAH,
  GERAK_TIMEOUT,
  GERAK_GAGAL
};

enum KategoriFuzzy : uint8_t {
  KAT_BELUM = 0,
  KAT_RENDAH,
  KAT_SEDANG,
  KAT_BAIK
};


enum KondisiMagnetAS5600 : uint8_t {
  MAGNET_I2C_ERROR = 0,
  MAGNET_TIDAK_TERDETEKSI,
  MAGNET_TERLALU_JAUH,
  MAGNET_TERLALU_DEKAT,
  MAGNET_JARAK_OK,
  MAGNET_STATUS_TIDAK_VALID
};

struct DiagnostikMagnetAS5600 {
  bool komunikasiOK;
  bool md;
  bool ml;
  bool mh;
  uint8_t statusRaw;
  uint8_t agc;
  uint16_t magnitude;
  KondisiMagnetAS5600 kondisi;
};

struct TombolDebounce {
  uint8_t pin;
  bool rawTerakhir;
  bool stabil;
  unsigned long waktuPerubahan;
  bool eventTekan;
};

struct DataLatihan {
  uint32_t nomorData;
  float target;
  uint16_t repTarget;
  uint16_t repSelesai;
  float pre[3];
  float medianPre;
  float post[3];
  float medianPost;
  float delta;
  float skorFuzzy;
  uint8_t kategori;
  String status;
};

struct DataTersimpan {
  bool valid;
  uint32_t nomorData;
  float target;
  uint16_t repTarget;
  uint16_t repSelesai;
  float pre[3];
  float medianPre;
  float post[3];
  float medianPost;
  float delta;
  float skorFuzzy;
  String kategori;
  String status;
};

// ================================================================
// VARIABEL GLOBAL
// ================================================================
TombolDebounce tombolBack;
TombolDebounce tombolStop;
TombolDebounce tombolMode;
TombolDebounce tombolOK;
TombolDebounce tombolZero;

MenuUtama menuUtamaAktif = MENU_LATIHAN;
MenuKelola menuKelolaAktif = KELOLA_HAPUS_SATU;

DataLatihan dataAktif;

// Target latihan aktif dapat diatur dari menu PANTAU SUDUT dan disimpan
// ke LittleFS. Nilai ini harus dikunci sebelum pengambilan data dimulai.
// Target aktif diambil dari maksimum nyaman pada menu PANTAU SUDUT.
// Nilai default dipakai hanya sebelum pengukuran nyaman pertama dilakukan.
float targetLatihanAktif = TARGET_LATIHAN_DEFAULT;

// Target yang sedang dijalankan motor. Variabel ini khusus untuk telemetri
// dashboard, sehingga saat motor kembali ke 0 derajat dashboard tetap
// menampilkan target gerak yang benar, bukan hanya target latihan maksimum.
float targetGerakDashboard = TARGET_LATIHAN_DEFAULT;

bool motorAktif = false;
bool sedangBergerak = false;
bool holdingLimitBawah = false;
bool izinKeluarLimitBawah = false;
bool stopSaatDelayHolding = false;

bool sensorOK = false;
bool zeroSiap = false;
uint16_t rawSudut = 0;
float sudutAbsolut = 0.0f;
float sudutZero = 0.0f;
float sudutRelatif = 0.0f;
float sudutFilterGerak = 0.0f;
bool filterGerakSiap = false;
unsigned long waktuSudutValidTerakhir = 0;
uint32_t jumlahErrorI2C = 0;
uint32_t jumlahOutlierSensor = 0;


bool monitorMagnetLive = false;
unsigned long waktuMonitorMagnetTerakhir = 0;

// Status komunikasi dashboard.
WiFiClient dashboardClient;
String bufferPerintahWiFi = "";
unsigned long waktuReconnectWiFiTerakhir = 0;
unsigned long waktuReconnectDashboardTerakhir = 0;
unsigned long waktuRealtimeDashboardTerakhir = 0;
unsigned long waktuSensorIdleDashboardTerakhir = 0;
bool stopDariDashboard = false;
bool operasiLCDSedangAktif = false;
String tahapDashboard = "IDLE";
float errorDashboardSebelumnya = 0.0f;
float deltaErrorDashboard = 0.0f;
bool errorDashboardSiap = false;

// Forward declaration karena layanan live juga dipanggil saat motor bergerak.
float rawKeDerajat(uint16_t raw);
bool updateSudutGerakCepat();
void layaniMonitorMagnetLive(bool paksa = false);

// Forward declaration komunikasi WiFi/dashboard.
void mulaiWiFiDashboard();
void layaniWiFiDashboard(bool paksaKirim = false);
void layaniInputWiFiDashboard();
void kirimDataRealtimeWiFi(bool paksa = false);
void prosesPerintahDashboard(String perintah);
void kirimCSVKeDashboard();

// Fungsi program utama yang dipanggil oleh perintah dashboard.
bool updateSudut();
bool limitAtasAktif();
bool limitBawahAktif();
void motorMati();
bool setZeroDariSerial();
bool aktifkanMotorDariSerial();
HasilGerak gerakKeTarget(float target, bool izinkanKeluarDariLimitBawah,
                         const String &judulLCD);
bool simpanTargetLatihanKeConfig(float target);
const char *namaHasilGerak(HasilGerak hasil);

// ================================================================
// KOMUNIKASI WIFI DAN DASHBOARD
// ================================================================
String statusMotorDashboard() {
  if (sedangBergerak) return "BERGERAK";
  if (holdingLimitBawah) return "HOLDING_LIMIT_BAWAH";
  if (motorAktif) return "HOLDING";
  return "OFF";
}

String kategoriDashboard() {
  switch (dataAktif.kategori) {
    case KAT_BELUM:  return "BELUM";
    case KAT_RENDAH: return "RENDAH";
    case KAT_SEDANG: return "SEDANG";
    case KAT_BAIK:   return "BAIK";
    default:         return "BELUM";
  }
}

String modeGerakDashboard(float errorSudut) {
  if (sedangBergerak) {
    return errorSudut >= 0.0f ? "FLEKSI" : "EKSTENSI";
  }
  if (holdingLimitBawah) return "HOLDING_LIMIT_BAWAH";
  if (!motorAktif) return "IDLE";
  return tahapDashboard;
}

void kirimBarisDashboard(const String &baris) {
  if (!dashboardClient.connected()) return;
  dashboardClient.print(baris);
  dashboardClient.print('\n');
}

void kirimAckDashboard(const String &perintah, bool berhasil,
                       const String &pesan) {
  if (!dashboardClient.connected()) return;

  String json;
  json.reserve(220);
  json += "{\"message_type\":\"ack\",\"command\":\"";
  json += perintah;
  json += "\",\"success\":";
  json += berhasil ? "true" : "false";
  json += ",\"message\":\"";
  json += pesan;
  json += "\"}";
  kirimBarisDashboard(json);
}

void kirimHelloDashboard() {
  if (!dashboardClient.connected()) return;

  String json;
  json.reserve(220);
  json += "{\"message_type\":\"hello\",\"device\":\"ESP32-C6\",";
  json += "\"firmware\":\"REHAB_SIKU_V8_WIFI\",\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",\"rssi\":";
  json += String(WiFi.RSSI());
  json += "}";
  kirimBarisDashboard(json);
}

void kirimDataRealtimeWiFi(bool paksa) {
  if (!dashboardClient.connected()) return;

  unsigned long sekarang = millis();
  if (!paksa && sekarang - waktuRealtimeDashboardTerakhir <
      INTERVAL_REALTIME_DASHBOARD_MS) {
    return;
  }
  waktuRealtimeDashboardTerakhir = sekarang;

  // Saat motor tidak bergerak, sensor tetap dibaca agar nilai sudut pada
  // dashboard terus diperbarui. Ketika motor bergerak, sudut diperbarui oleh
  // loop closed-loop gerak motor.
  if (!sedangBergerak &&
      sekarang - waktuSensorIdleDashboardTerakhir >=
      INTERVAL_SENSOR_IDLE_DASHBOARD_MS) {
    waktuSensorIdleDashboardTerakhir = sekarang;
    updateSudut();
  }

  float targetUntukDashboard = sedangBergerak
    ? targetGerakDashboard
    : targetLatihanAktif;

  // Error bertanda tetap dipakai untuk menentukan arah gerak dan grafik
  // error lama pada dashboard. Nilai positif berarti target berada di atas
  // sudut aktual, sedangkan nilai negatif berarti sudut aktual melewati target.
  float errorSudutDashboard = targetUntukDashboard - sudutRelatif;

  // Dua field baru yang diminta dashboard:
  // 1. error_sensor: selisih absolut target dan pembacaan AS5600 (derajat).
  // 2. error_motor : selisih posisi target dan posisi aktual yang dikonversi
  //    menjadi step/pulsa berdasarkan konfigurasi HBS860H dan rasio reduksi.
  //
  // Catatan: error_motor ini merupakan estimasi dari umpan balik sudut AS5600,
  // bukan nilai following-error internal encoder yang tersimpan di HBS860H.
  float errorSensorDashboard = fabsf(errorSudutDashboard);
  int32_t stepTargetDashboard = static_cast<int32_t>(
    lroundf(targetUntukDashboard * PULSA_PER_DERAJAT_OUTPUT)
  );
  int32_t stepAktualDashboard = static_cast<int32_t>(
    lroundf(sudutRelatif * PULSA_PER_DERAJAT_OUTPUT)
  );
  int32_t selisihStepDashboard = stepTargetDashboard - stepAktualDashboard;
  if (selisihStepDashboard < 0) selisihStepDashboard = -selisihStepDashboard;
  int32_t errorMotorDashboard = selisihStepDashboard;

  if (errorDashboardSiap) {
    deltaErrorDashboard = errorSudutDashboard - errorDashboardSebelumnya;
  } else {
    deltaErrorDashboard = 0.0f;
    errorDashboardSiap = true;
  }
  errorDashboardSebelumnya = errorSudutDashboard;

  String modeDashboard = modeGerakDashboard(errorSudutDashboard);
  float outputFuzzyDashboard = dataAktif.skorFuzzy;

  String json;
  json.reserve(1250);
  json += "{";

  // ==============================================================
  // FIELD UTAMA SESUAI FORMAT DASHBOARD ANTIGRAVITY
  // ==============================================================
  json += "\"timestamp\":" + String(sekarang) + ",";
  json += "\"sudut_target\":" + String(targetUntukDashboard, 2) + ",";
  json += "\"sudut_aktual\":" + String(sudutRelatif, 2) + ",";
  json += "\"error\":" + String(errorSudutDashboard, 2) + ",";
  json += "\"error_sensor\":" + String(errorSensorDashboard, 2) + ",";
  json += "\"error_motor\":" + String(errorMotorDashboard) + ",";
  json += "\"output_fuzzy\":" + String(outputFuzzyDashboard, 2) + ",";
  json += "\"motor_on\":";
  json += motorAktif ? "true," : "false,";
  json += "\"limit_atas\":";
  json += limitAtasAktif() ? "true," : "false,";
  json += "\"limit_bawah\":";
  json += limitBawahAktif() ? "true," : "false,";
  json += "\"mode\":\"" + modeDashboard + "\",";
  json += "\"sensor_ok\":";
  json += sensorOK ? "true," : "false,";
  json += "\"fuzzy_input_error\":" + String(errorSudutDashboard, 2) + ",";
  json += "\"fuzzy_input_delta_error\":" + String(deltaErrorDashboard, 2) + ",";
  json += "\"fuzzy_output\":" + String(outputFuzzyDashboard, 2) + ",";

  // Input fuzzy yang benar pada penelitian: median post-test dan delta sudut.
  json += "\"fuzzy_input_median_post\":" + String(dataAktif.medianPost, 2) + ",";
  json += "\"fuzzy_input_delta_sudut\":" + String(dataAktif.delta, 2) + ",";

  // ==============================================================
  // FIELD TAMBAHAN / ALIAS UNTUK KOMPATIBILITAS
  // ==============================================================
  json += "\"message_type\":\"realtime\",";
  json += "\"device\":\"ESP32-C6\",";
  json += "\"millis\":" + String(sekarang) + ",";
  json += "\"angle\":" + String(sudutRelatif, 2) + ",";
  json += "\"sudut\":" + String(sudutRelatif, 2) + ",";
  json += "\"actual_angle\":" + String(sudutRelatif, 2) + ",";
  json += "\"absolute_angle\":" + String(sudutAbsolut, 2) + ",";
  json += "\"raw_angle\":" + String(rawSudut) + ",";
  json += "\"target\":" + String(targetUntukDashboard, 2) + ",";
  json += "\"target_angle\":" + String(targetUntukDashboard, 2) + ",";
  json += "\"zero_ready\":";
  json += zeroSiap ? "true," : "false,";
  json += "\"moving\":";
  json += sedangBergerak ? "true," : "false,";
  json += "\"limit_top\":";
  json += limitAtasAktif() ? "true," : "false,";
  json += "\"limit_bottom\":";
  json += limitBawahAktif() ? "true," : "false,";
  json += "\"motor_status\":\"" + statusMotorDashboard() + "\",";
  json += "\"phase\":\"" + tahapDashboard + "\",";
  json += "\"session_number\":" + String(dataAktif.nomorData) + ",";
  json += "\"rep_current\":" + String(dataAktif.repSelesai) + ",";
  json += "\"rep_target\":" + String(dataAktif.repTarget) + ",";
  json += "\"median_pre\":" + String(dataAktif.medianPre, 2) + ",";
  json += "\"median_post\":" + String(dataAktif.medianPost, 2) + ",";
  json += "\"delta\":" + String(dataAktif.delta, 2) + ",";
  json += "\"fuzzy_score\":" + String(dataAktif.skorFuzzy, 2) + ",";
  json += "\"category\":\"" + kategoriDashboard() + "\",";
  json += "\"session_status\":\"" + dataAktif.status + "\",";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI());
  json += "}";

  kirimBarisDashboard(json);
}

String ambilKolomCSV(const String &baris, uint8_t indeks) {
  int awal = 0;
  uint8_t kolom = 0;

  for (int i = 0; i <= baris.length(); i++) {
    bool akhirKolom = (i == baris.length() || baris.charAt(i) == ',');
    if (!akhirKolom) continue;

    if (kolom == indeks) {
      return baris.substring(awal, i);
    }
    kolom++;
    awal = i + 1;
  }
  return "";
}

void kirimCSVKeDashboard() {
  if (!dashboardClient.connected()) return;

  if (!littleFSSiap || !LittleFS.exists(FILE_DATA)) {
    kirimAckDashboard("GET_CSV", false, "File data tidak tersedia");
    return;
  }

  File file = LittleFS.open(FILE_DATA, FILE_READ);
  if (!file) {
    kirimAckDashboard("GET_CSV", false, "Gagal membuka file data");
    return;
  }

  kirimBarisDashboard("CSV_START");

  // Header disesuaikan dengan nama field tabel pada dashboard Antigravity.
  kirimBarisDashboard(
    "no,sudut_target,repetisi_target,repetisi_selesai,"
    "median_pretest,median_posttest,delta_perkembangan,"
    "skor_fuzzy,kategori_hasil,status_sesi"
  );

  bool barisPertama = true;
  while (file.available() && dashboardClient.connected()) {
    String baris = file.readStringUntil('\n');
    baris.trim();
    if (baris.length() == 0) continue;

    // Lewati header asli LittleFS.
    if (barisPertama) {
      barisPertama = false;
      if (baris.startsWith("nomor,")) continue;
    }

    // Format asli:
    // 0 nomor, 1 target, 2 rep_target, 3 rep_selesai,
    // 4-6 pre, 7 median_pre, 8-10 post, 11 median_post,
    // 12 delta, 13 skor_fuzzy, 14 kategori, 15 status.
    String barisDashboard;
    barisDashboard.reserve(180);
    barisDashboard += ambilKolomCSV(baris, 0) + ",";
    barisDashboard += ambilKolomCSV(baris, 1) + ",";
    barisDashboard += ambilKolomCSV(baris, 2) + ",";
    barisDashboard += ambilKolomCSV(baris, 3) + ",";
    barisDashboard += ambilKolomCSV(baris, 7) + ",";
    barisDashboard += ambilKolomCSV(baris, 11) + ",";
    barisDashboard += ambilKolomCSV(baris, 12) + ",";
    barisDashboard += ambilKolomCSV(baris, 13) + ",";
    barisDashboard += ambilKolomCSV(baris, 14) + ",";
    barisDashboard += ambilKolomCSV(baris, 15);

    kirimBarisDashboard(barisDashboard);
    delay(1);
  }

  file.close();
  kirimBarisDashboard("CSV_END");
}

void prosesPerintahDashboard(String perintah) {
  perintah.trim();
  if (perintah.length() == 0) return;

  String cmd = perintah;
  if (cmd.startsWith("CMD:")) cmd = cmd.substring(4);
  cmd.trim();

  String upper = cmd;
  upper.toUpperCase();

  if (upper == "PING") {
    kirimBarisDashboard(
      "{\"message_type\":\"pong\",\"device\":\"ESP32-C6\"}"
    );
    return;
  }

  if (upper == "GET_STATUS") {
    kirimDataRealtimeWiFi(true);
    return;
  }

  if (upper == "GET_CSV") {
    kirimCSVKeDashboard();
    return;
  }

  // STOP selalu diizinkan, termasuk saat latihan dan motor bergerak.
  if (upper == "STOP" || upper == "MOTOR_OFF") {
    bool sistemSedangAktif = sedangBergerak || operasiLCDSedangAktif;
    stopDariDashboard = sistemSedangAktif;
    if (operasiLCDSedangAktif) tombolStop.eventTekan = true;
    motorMati();
    kirimAckDashboard(upper, true, "Motor dihentikan");
    return;
  }

  // Perintah gerak jarak jauh hanya diizinkan ketika tidak sedang berada
  // di dalam alur menu LCD. Ini mencegah konflik dengan latihan pasien.
  if (operasiLCDSedangAktif || sedangBergerak) {
    kirimAckDashboard(upper, false, "Sistem sedang digunakan dari menu LCD");
    return;
  }

  if (upper == "ZERO") {
    setZeroDariSerial();
    kirimAckDashboard("ZERO", zeroSiap, zeroSiap ? "Zero berhasil" : "Zero gagal");
    kirimDataRealtimeWiFi(true);
    return;
  }

  if (upper == "MOTOR_ON") {
    aktifkanMotorDariSerial();
    kirimAckDashboard("MOTOR_ON", motorAktif, motorAktif ? "Motor aktif" : "Motor gagal aktif");
    kirimDataRealtimeWiFi(true);
    return;
  }

  if (upper.startsWith("SET_TARGET:")) {
    float targetBaru = cmd.substring(cmd.indexOf(':') + 1).toFloat();
    if (targetBaru < 5.0f || targetBaru > TARGET_MAX) {
      kirimAckDashboard("SET_TARGET", false, "Target di luar batas 5-100 derajat");
      return;
    }
    bool tersimpan = simpanTargetLatihanKeConfig(targetBaru);
    kirimAckDashboard("SET_TARGET", true,
                      tersimpan ? "Target tersimpan" : "Target aktif di RAM");
    kirimDataRealtimeWiFi(true);
    return;
  }

  if (upper.startsWith("TARGET:")) {
    float target = cmd.substring(cmd.indexOf(':') + 1).toFloat();
    if (!zeroSiap) {
      kirimAckDashboard("TARGET", false, "Zero belum disetel");
      return;
    }
    if (target < TARGET_MIN || target > TARGET_MAX) {
      kirimAckDashboard("TARGET", false, "Target di luar batas 0-100 derajat");
      return;
    }

    tahapDashboard = "UJI_TARGET_WIFI";
    HasilGerak hasil = gerakKeTarget(target, true, "UJI WIFI");
    tahapDashboard = "IDLE";
    kirimAckDashboard("TARGET", hasil == GERAK_BERHASIL,
                      String(namaHasilGerak(hasil)));
    kirimDataRealtimeWiFi(true);
    return;
  }

  kirimAckDashboard(upper, false, "Perintah tidak dikenali");
}

void layaniInputWiFiDashboard() {
  while (dashboardClient.connected() && dashboardClient.available()) {
    char c = static_cast<char>(dashboardClient.read());

    if (c == '\r') continue;
    if (c == '\n') {
      String perintah = bufferPerintahWiFi;
      bufferPerintahWiFi = "";
      prosesPerintahDashboard(perintah);
      continue;
    }

    if (bufferPerintahWiFi.length() < BATAS_BUFFER_PERINTAH_WIFI) {
      bufferPerintahWiFi += c;
    } else {
      bufferPerintahWiFi = "";
      kirimAckDashboard("UNKNOWN", false, "Perintah terlalu panjang");
    }
  }
}

void mulaiWiFiDashboard() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("WiFi menghubungkan ke SSID: ");
  Serial.println(WIFI_SSID);
}

void pastikanKoneksiWiFiDashboard() {
  unsigned long sekarang = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (dashboardClient.connected()) dashboardClient.stop();

    if (sekarang - waktuReconnectWiFiTerakhir >=
        INTERVAL_RECONNECT_WIFI_MS) {
      waktuReconnectWiFiTerakhir = sekarang;
      Serial.println("WiFi belum terhubung, mencoba ulang...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }

  if (dashboardClient.connected()) return;
  if (sekarang - waktuReconnectDashboardTerakhir <
      INTERVAL_RECONNECT_DASHBOARD_MS) {
    return;
  }

  waktuReconnectDashboardTerakhir = sekarang;
  Serial.print("Menghubungkan dashboard ");
  Serial.print(DASHBOARD_HOST);
  Serial.print(':');
  Serial.println(DASHBOARD_PORT);

  dashboardClient.stop();
  if (dashboardClient.connect(DASHBOARD_HOST, DASHBOARD_PORT)) {
    dashboardClient.setNoDelay(true);
    dashboardClient.setTimeout(10);
    bufferPerintahWiFi = "";
    Serial.println("Dashboard WiFi TERHUBUNG.");
    Serial.print("IP ESP32: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI WiFi: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    kirimHelloDashboard();
    kirimDataRealtimeWiFi(true);
  } else {
    Serial.println("Dashboard belum dapat dihubungi.");
  }
}

void layaniWiFiDashboard(bool paksaKirim) {
  pastikanKoneksiWiFiDashboard();
  if (!dashboardClient.connected()) return;

  layaniInputWiFiDashboard();
  kirimDataRealtimeWiFi(paksaKirim);
}

// ================================================================
// UTILITAS LCD
// ================================================================
String rapikanLCD(String teks) {
  if (teks.length() > 20) teks = teks.substring(0, 20);
  while (teks.length() < 20) teks += ' ';
  return teks;
}

void lcdBaris(uint8_t baris, const String &teks) {
  if (!lcdSiap || lcd == nullptr) return;
  lcd->setCursor(0, baris);
  lcd->print(rapikanLCD(teks));
}

void lcdBersih() {
  if (lcdSiap && lcd != nullptr) lcd->clear();
}

bool cekAlamatI2C(uint8_t alamat) {
  Wire.beginTransmission(alamat);
  return Wire.endTransmission() == 0;
}

void setupLCD() {
  if (cekAlamatI2C(0x27)) alamatLCD = 0x27;
  else if (cekAlamatI2C(0x3F)) alamatLCD = 0x3F;
  else {
    lcdSiap = false;
    Serial.println("LCD I2C tidak ditemukan pada 0x27 atau 0x3F.");
    return;
  }

  lcd = new LiquidCrystal_I2C(alamatLCD, 20, 4);
  lcd->init();
  lcd->backlight();
  lcd->clear();
  lcdSiap = true;

  Serial.print("LCD ditemukan pada alamat 0x");
  Serial.println(alamatLCD, HEX);
}

void tampilBoot() {
  lcdBersih();
  lcdBaris(0, "REHAB SIKU 1-DOF");
  lcdBaris(1, "FUZZY MAMDANI");
  lcdBaris(2, "ESP32-C6 SYSTEM");
  lcdBaris(3, "Booting...");

  Serial.println();
  Serial.println("========================================");
  Serial.println(" REHAB SIKU 1-DOF - FUZZY MAMDANI");
  Serial.println(" FINAL V4 LCD + LITTLEFS");
  Serial.println("========================================");
  delay(1800);
}

// ================================================================
// TOMBOL
// ================================================================
void initTombol(TombolDebounce &tombol, uint8_t pin) {
  tombol.pin = pin;
  pinMode(pin, INPUT_PULLUP);
  tombol.rawTerakhir = digitalRead(pin);
  tombol.stabil = tombol.rawTerakhir;
  tombol.waktuPerubahan = millis();
  tombol.eventTekan = false;
}

void updateSatuTombol(TombolDebounce &tombol) {
  bool pembacaan = digitalRead(tombol.pin);

  if (pembacaan != tombol.rawTerakhir) {
    tombol.rawTerakhir = pembacaan;
    tombol.waktuPerubahan = millis();
  }

  if (millis() - tombol.waktuPerubahan >= DEBOUNCE_MS) {
    if (pembacaan != tombol.stabil) {
      tombol.stabil = pembacaan;
      if (tombol.stabil == LOW) tombol.eventTekan = true;
    }
  }
}

void updateSemuaTombol() {
  updateSatuTombol(tombolBack);
  updateSatuTombol(tombolStop);
  updateSatuTombol(tombolMode);
  updateSatuTombol(tombolOK);
  updateSatuTombol(tombolZero);
  layaniWiFiDashboard(false);
}

bool ambilEvent(TombolDebounce &tombol) {
  if (!tombol.eventTekan) return false;
  tombol.eventTekan = false;
  return true;
}

void bersihkanEventTombol() {
  tombolBack.eventTekan = false;
  tombolStop.eventTekan = false;
  tombolMode.eventTekan = false;
  tombolOK.eventTekan = false;
  tombolZero.eventTekan = false;
}

bool tombolSedangDitekan(uint8_t pin) {
  return digitalRead(pin) == LOW;
}

bool tungguTahanTombolOK(unsigned long durasiMs, const String &judul) {
  unsigned long mulai = 0;

  while (true) {
    if (digitalRead(BTN_STOP) == LOW) return false;
    if (digitalRead(BTN_BACK) == LOW) return false;

    if (digitalRead(BTN_OK) == LOW) {
      if (mulai == 0) mulai = millis();
      unsigned long berlalu = millis() - mulai;
      unsigned long sisa = berlalu >= durasiMs ? 0 : durasiMs - berlalu;

      lcdBaris(0, judul);
      lcdBaris(1, "Tahan tombol OK");
      lcdBaris(2, "Sisa: " + String((sisa + 999UL) / 1000UL) + " detik");
      lcdBaris(3, "BACK = batal");

      if (berlalu >= durasiMs) {
        while (digitalRead(BTN_OK) == LOW) delay(10);
        return true;
      }
    } else {
      mulai = 0;
    }

    delay(20);
  }
}

// ================================================================
// LIMIT SWITCH
// ================================================================
bool limitAtasAktif() {
  return digitalRead(LIMIT_ATAS) == HIGH;
}

bool limitBawahAktif() {
  return digitalRead(LIMIT_BAWAH) == HIGH;
}

// ================================================================
// AS5600
// ================================================================
bool bacaBytesAS5600(uint8_t regAwal, uint8_t *buffer, uint8_t jumlahByte,
                       uint8_t jumlahRetry = AS5600_RETRY_I2C) {
  if (buffer == nullptr || jumlahByte == 0) return false;

  for (uint8_t percobaan = 0; percobaan < jumlahRetry; percobaan++) {
    while (Wire.available()) Wire.read();

    Wire.beginTransmission(AS5600_ADDR);
    Wire.write(regAwal);
    uint8_t statusKirim = Wire.endTransmission(false);

    if (statusKirim == 0) {
      uint8_t diterima = Wire.requestFrom(
        static_cast<uint8_t>(AS5600_ADDR),
        jumlahByte,
        static_cast<uint8_t>(true)
      );

      if (diterima == jumlahByte && Wire.available() >= jumlahByte) {
        for (uint8_t i = 0; i < jumlahByte; i++) {
          buffer[i] = Wire.read();
        }
        return true;
      }
    }

    jumlahErrorI2C++;
    while (Wire.available()) Wire.read();
    delayMicroseconds(250 + (percobaan * 250));
  }

  return false;
}

bool bacaRegister8(uint8_t reg, uint8_t &hasil) {
  uint8_t buffer[1] = {0};
  if (!bacaBytesAS5600(reg, buffer, 1)) return false;
  hasil = buffer[0];
  return true;
}

bool bacaRegister16_12bit(uint8_t regHigh, uint16_t &hasil) {
  uint8_t buffer[2] = {0, 0};
  if (!bacaBytesAS5600(regHigh, buffer, 2)) return false;
  hasil = ((buffer[0] & 0x0F) << 8) | buffer[1];
  return true;
}

bool bacaRawAS5600Sekali(uint16_t &hasilRaw) {
  uint8_t buffer[2] = {0, 0};
  if (!bacaBytesAS5600(REG_RAW_ANGLE_H, buffer, 2, 1)) return false;
  hasilRaw = ((buffer[0] & 0x0F) << 8) | buffer[1];
  return true;
}

int16_t selisihRawTerpendek(uint16_t nilai, uint16_t acuan) {
  int16_t selisih = static_cast<int16_t>(nilai) - static_cast<int16_t>(acuan);
  if (selisih > 2048) selisih -= 4096;
  if (selisih < -2048) selisih += 4096;
  return selisih;
}

bool bacaRawAS5600Stabil(uint16_t &hasilRaw, uint8_t jumlahSampel = 3) {
  if (jumlahSampel < 1) jumlahSampel = 1;
  if (jumlahSampel > 5) jumlahSampel = 5;

  uint16_t sampel[5] = {0, 0, 0, 0, 0};
  uint8_t valid = 0;

  for (uint8_t i = 0; i < jumlahSampel; i++) {
    uint16_t raw = 0;
    if (bacaRawAS5600Sekali(raw)) {
      sampel[valid++] = raw;
    }
    if (i + 1 < jumlahSampel) delayMicroseconds(250);
  }

  if (valid == 0) return false;
  if (valid == 1) {
    hasilRaw = sampel[0];
    return true;
  }

  // Semua sampel diubah menjadi selisih terpendek terhadap sampel pertama,
  // sehingga pembacaan di sekitar 359°/0° tidak menghasilkan loncatan palsu.
  int16_t nilai[5] = {0, 0, 0, 0, 0};
  nilai[0] = 0;
  for (uint8_t i = 1; i < valid; i++) {
    nilai[i] = selisihRawTerpendek(sampel[i], sampel[0]);
  }

  for (uint8_t i = 0; i < valid; i++) {
    for (uint8_t j = i + 1; j < valid; j++) {
      if (nilai[j] < nilai[i]) {
        int16_t sementara = nilai[i];
        nilai[i] = nilai[j];
        nilai[j] = sementara;
      }
    }
  }

  int16_t medianSelisih = nilai[valid / 2];
  int32_t rawMedian = static_cast<int32_t>(sampel[0]) + medianSelisih;
  while (rawMedian < 0) rawMedian += 4096;
  while (rawMedian >= 4096) rawMedian -= 4096;
  hasilRaw = static_cast<uint16_t>(rawMedian);
  return true;
}

bool bacaRawAS5600(uint16_t &hasilRaw) {
  return bacaRawAS5600Stabil(hasilRaw, AS5600_SAMPEL_GERAK);
}

const char *namaKondisiMagnet(KondisiMagnetAS5600 kondisi) {
  switch (kondisi) {
    case MAGNET_I2C_ERROR:            return "I2C ERROR";
    case MAGNET_TIDAK_TERDETEKSI:     return "TIDAK TERDETEKSI";
    case MAGNET_TERLALU_JAUH:         return "TERLALU JAUH / LEMAH";
    case MAGNET_TERLALU_DEKAT:        return "TERLALU DEKAT / KUAT";
    case MAGNET_JARAK_OK:             return "JARAK OK";
    default:                           return "STATUS TIDAK VALID";
  }
}

bool bacaDiagnostikMagnet(DiagnostikMagnetAS5600 &info) {
  info = {};
  uint8_t status = 0;
  uint8_t agc = 0;
  uint16_t magnitude = 0;

  if (!bacaRegister8(REG_STATUS, status)) {
    info.kondisi = MAGNET_I2C_ERROR;
    return false;
  }

  info.komunikasiOK = true;
  info.statusRaw = status;
  info.md = (status & 0x20) != 0; // Magnet detected
  info.ml = (status & 0x10) != 0; // Magnet too weak
  info.mh = (status & 0x08) != 0; // Magnet too strong

  // AGC dan magnitude bersifat diagnostik tambahan. Kegagalan membaca
  // keduanya tidak membatalkan status utama dari register STATUS.
  if (bacaRegister8(REG_AGC, agc)) info.agc = agc;
  if (bacaRegister16_12bit(REG_MAGNITUDE_H, magnitude)) {
    info.magnitude = magnitude;
  }

  if (!info.md) {
    info.kondisi = MAGNET_TIDAK_TERDETEKSI;
  } else if (info.ml && info.mh) {
    info.kondisi = MAGNET_STATUS_TIDAK_VALID;
  } else if (info.ml) {
    info.kondisi = MAGNET_TERLALU_JAUH;
  } else if (info.mh) {
    info.kondisi = MAGNET_TERLALU_DEKAT;
  } else {
    info.kondisi = MAGNET_JARAK_OK;
  }

  return true;
}

bool magnetTerdeteksi() {
  DiagnostikMagnetAS5600 info;
  return bacaDiagnostikMagnet(info) && info.md;
}

void cetakDiagnostikMagnetSerial(bool satuBaris = false) {
  DiagnostikMagnetAS5600 info;
  bool statusTerbaca = bacaDiagnostikMagnet(info);

  uint16_t rawSekarang = 0;
  bool sudutTerbaca = bacaRawAS5600(rawSekarang);
  float absolutSekarang = sudutTerbaca ? rawKeDerajat(rawSekarang) : 0.0f;
  float relatifSekarang = sudutRelatif;

  if (!satuBaris) {
    Serial.println();
    Serial.println("========== MAGNET AS5600 ==========");
  }

  if (!statusTerbaca) {
    Serial.println("MAGNET | I2C ERROR | Periksa SDA/SCL/VCC/GND");
    if (!satuBaris) Serial.println("===================================");
    return;
  }

  if (satuBaris) {
    Serial.print("MAGNET | MD="); Serial.print(info.md ? 1 : 0);
    Serial.print(" ML="); Serial.print(info.ml ? 1 : 0);
    Serial.print(" MH="); Serial.print(info.mh ? 1 : 0);
    Serial.print(" | "); Serial.print(namaKondisiMagnet(info.kondisi));
    Serial.print(" | AGC="); Serial.print(info.agc);
    Serial.print(" MAG="); Serial.print(info.magnitude);
    if (sudutTerbaca) {
      Serial.print(" RAW="); Serial.print(rawSekarang);
      Serial.print(" ABS="); Serial.print(absolutSekarang, 2);
      Serial.print(" REL="); Serial.print(relatifSekarang, 2);
    }
    Serial.println();
  } else {
    Serial.print("Komunikasi I2C : "); Serial.println("OK");
    Serial.print("MD terdeteksi  : "); Serial.println(info.md ? "YA" : "TIDAK");
    Serial.print("ML terlalu lemah: "); Serial.println(info.ml ? "YA" : "TIDAK");
    Serial.print("MH terlalu kuat : "); Serial.println(info.mh ? "YA" : "TIDAK");
    Serial.print("Status jarak    : "); Serial.println(namaKondisiMagnet(info.kondisi));
    Serial.print("AGC              : "); Serial.println(info.agc);
    Serial.print("Magnitude        : "); Serial.println(info.magnitude);
    if (sudutTerbaca) {
      Serial.print("Raw angle        : "); Serial.println(rawSekarang);
      Serial.print("Sudut absolut    : "); Serial.println(absolutSekarang, 2);
      Serial.print("Sudut relatif    : "); Serial.println(relatifSekarang, 2);
    }
    Serial.println("Catatan: status jarak berasal dari kuat/lemah medan magnet, bukan mm.");
    Serial.println("===================================");
  }
}

void layaniMonitorMagnetLive(bool paksa) {
  if (!monitorMagnetLive && !paksa) return;
  unsigned long sekarang = millis();
  if (!paksa && sekarang - waktuMonitorMagnetTerakhir < INTERVAL_MONITOR_MAGNET_MS) {
    return;
  }
  waktuMonitorMagnetTerakhir = sekarang;
  if (zeroSiap) updateSudutGerakCepat();
  cetakDiagnostikMagnetSerial(true);
}

float rawKeDerajat(uint16_t raw) {
  return raw * 360.0f / 4096.0f;
}

float normalisasi180(float sudut) {
  while (sudut > 180.0f) sudut -= 360.0f;
  while (sudut < -180.0f) sudut += 360.0f;
  return sudut;
}

bool bacaSudutRataRata(float &hasilDerajat, uint8_t jumlahSampel = 7) {
  float totalSin = 0.0f;
  float totalCos = 0.0f;
  uint8_t valid = 0;

  for (uint8_t i = 0; i < jumlahSampel; i++) {
    uint16_t raw = 0;
    if (bacaRawAS5600(raw)) {
      float derajat = rawKeDerajat(raw);
      float radian = derajat * DEG_TO_RAD;
      totalSin += sinf(radian);
      totalCos += cosf(radian);
      rawSudut = raw;
      valid++;
    }
    delay(2);
  }

  uint8_t minimalValid = (jumlahSampel + 1U) / 2U;
  if (valid < minimalValid) return false;

  float radianRata = atan2f(totalSin / valid, totalCos / valid);
  hasilDerajat = radianRata * RAD_TO_DEG;
  if (hasilDerajat < 0.0f) hasilDerajat += 360.0f;
  return true;
}

bool updateSudut() {
  float hasil = 0.0f;
  if (!bacaSudutRataRata(hasil, 7)) {
    sensorOK = false;
    return false;
  }

  sensorOK = true;
  sudutAbsolut = hasil;
  sudutRelatif = normalisasi180(
    TANDA_ARAH_SENSOR * (sudutAbsolut - sudutZero)
  );

  if (fabsf(sudutRelatif) < 0.15f) sudutRelatif = 0.0f;
  sudutFilterGerak = sudutRelatif;
  filterGerakSiap = true;
  waktuSudutValidTerakhir = millis();
  return true;
}

bool updateSudutGerakCepat() {
  uint16_t raw = 0;
  if (!bacaRawAS5600Stabil(raw, AS5600_SAMPEL_GERAK)) {
    sensorOK = false;
    return false;
  }

  float absolutBaru = rawKeDerajat(raw);
  float relatifMentah = normalisasi180(
    TANDA_ARAH_SENSOR * (absolutBaru - sudutZero)
  );
  if (fabsf(relatifMentah) < 0.15f) relatifMentah = 0.0f;

  unsigned long sekarang = millis();

  if (!filterGerakSiap) {
    sudutFilterGerak = relatifMentah;
    filterGerakSiap = true;
  } else {
    float beda = normalisasi180(relatifMentah - sudutFilterGerak);

    // Tolak satu pembacaan yang meloncat tidak masuk akal. Batas dibuat
    // sedikit lebih longgar ketika jeda pembacaan lebih panjang.
    unsigned long jeda = sekarang - waktuSudutValidTerakhir;
    float batasDinamis = BATAS_LONCATAN_SENSOR_DEG +
      min(4.0f, static_cast<float>(jeda) * 0.03f);

    if (fabsf(beda) > batasDinamis) {
      jumlahOutlierSensor++;
      sensorOK = true;  // nilai terakhir masih valid
      return true;
    }

    sudutFilterGerak += ALPHA_FILTER_GERAK * beda;
  }

  rawSudut = raw;
  sudutAbsolut = absolutBaru;
  sudutRelatif = sudutFilterGerak;
  waktuSudutValidTerakhir = sekarang;
  sensorOK = true;
  return true;
}

bool setZeroDenganOK(const String &asalMenu) {
  motorAktif = false;
  sedangBergerak = false;
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(ENA_PIN, MOTOR_MATI_LEVEL);
  zeroSiap = false;
  bersihkanEventTombol();

  while (true) {
    float sudutSekarang = 0.0f;
    bool terbaca = bacaSudutRataRata(sudutSekarang, 5);

    lcdBaris(0, "KALIBRASI NOL");
    lcdBaris(1, terbaca ? "Raw: " + String(sudutSekarang, 1) : "AS5600 ERROR");
    lcdBaris(2, "Posisikan pada 0");
    lcdBaris(3, "OK=Simpan BACK=Batal");

    updateSemuaTombol();

    if (ambilEvent(tombolBack)) {
      Serial.println("Kalibrasi dibatalkan dari menu " + asalMenu);
      return false;
    }

    if (ambilEvent(tombolStop)) {
      return false;
    }

    if (ambilEvent(tombolOK)) {
      if (!terbaca || !magnetTerdeteksi()) {
        lcdBaris(0, "ZERO GAGAL");
        lcdBaris(1, "Magnet/sensor error");
        lcdBaris(2, "Periksa AS5600");
        lcdBaris(3, "BACK = kembali");
        delay(1200);
        continue;
      }

      float hasilZero = 0.0f;
      if (!bacaSudutRataRata(hasilZero, 30)) {
        lcdBaris(0, "ZERO GAGAL");
        lcdBaris(1, "Sensor tak terbaca");
        lcdBaris(2, "Coba kembali");
        lcdBaris(3, "");
        delay(1200);
        continue;
      }

      sudutZero = hasilZero;
      sudutRelatif = 0.0f;
      sudutFilterGerak = 0.0f;
      filterGerakSiap = true;
      sensorOK = true;
      zeroSiap = true;

      lcdBaris(0, "ZERO BERHASIL");
      lcdBaris(1, "Sudut relatif 0.0");
      lcdBaris(2, "Rentang 0-100 deg");
      lcdBaris(3, "OK = lanjut");
      Serial.print("Zero AS5600 disimpan: ");
      Serial.println(sudutZero, 2);

      bersihkanEventTombol();
      while (true) {
        updateSemuaTombol();
        if (ambilEvent(tombolOK)) return true;
        if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return false;
        delay(20);
      }
    }

    delay(40);
  }
}

// ================================================================
// MOTOR DAN GERAK KE TARGET
// ================================================================
void setMotor(bool hidup) {
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(ENA_PIN, hidup ? MOTOR_HIDUP_LEVEL : MOTOR_MATI_LEVEL);
  motorAktif = hidup;
  if (!hidup) sedangBergerak = false;
}

void motorMati() {
  setMotor(false);
  holdingLimitBawah = false;
  izinKeluarLimitBawah = limitBawahAktif();
}

void aktifkanHoldingLimitBawah() {
  stopSaatDelayHolding = false;
  digitalWrite(STEP_PIN, LOW);
  sedangBergerak = false;

  // Free selama 1 detik, lalu holding sesuai kebutuhan mekanisme.
  digitalWrite(ENA_PIN, MOTOR_MATI_LEVEL);
  motorAktif = false;
  holdingLimitBawah = false;

  lcdBaris(0, "LIMIT BAWAH AKTIF");
  lcdBaris(1, "Pulsa dihentikan");
  lcdBaris(2, "Holding dlm 1 dtk");
  lcdBaris(3, "STOP = tetap free");

  unsigned long awal = millis();
  while (millis() - awal < DELAY_HOLDING_LIMIT_BAWAH_MS) {
    if (digitalRead(BTN_STOP) == LOW) {
      motorMati();
      stopSaatDelayHolding = true;
      izinKeluarLimitBawah = true;
      return;
    }
    delay(5);
  }

  digitalWrite(STEP_PIN, LOW);
  digitalWrite(ENA_PIN, MOTOR_HIDUP_LEVEL);
  motorAktif = true;
  holdingLimitBawah = true;
  izinKeluarLimitBawah = false;
}

bool stopDiminta() {
  // Tetap layani koneksi agar tombol STOP pada dashboard responsif walaupun
  // fungsi gerak motor sedang berjalan secara blocking.
  layaniWiFiDashboard(false);

  if (stopDariDashboard) {
    stopDariDashboard = false;
    motorMati();
    lcdBaris(0, "STOP DASHBOARD");
    lcdBaris(1, "Gerakan berhenti");
    lcdBaris(2, "Motor dinonaktifkan");
    lcdBaris(3, "Periksa posisi");
    Serial.println("STOP diterima dari dashboard WiFi.");
    return true;
  }

  if (digitalRead(BTN_STOP) == LOW) {
    delay(10);
    if (digitalRead(BTN_STOP) == LOW) {
      motorMati();
      lcdBaris(0, "STOP DITEKAN");
      lcdBaris(1, "Gerakan berhenti");
      lcdBaris(2, "Motor dinonaktifkan");
      lcdBaris(3, "Periksa posisi");
      Serial.println("STOP fisik ditekan. Motor OFF.");
      return true;
    }
  }

  if (Serial.available()) {
    char c = Serial.peek();
    if (c == 'x' || c == 'X' || c == '0') {
      Serial.read();
      motorMati();
      Serial.println("STOP dari Serial Monitor.");
      return true;
    }
  }

  return false;
}

void kirimSatuPulsaPeriode(uint32_t periodeUs) {
  const uint32_t highUs = 5UL;
  if (periodeUs < highUs + 5UL) periodeUs = highUs + 5UL;

  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(highUs);
  digitalWrite(STEP_PIN, LOW);
  delayMicroseconds(periodeUs - highUs);
}

uint32_t periodeTargetSmooth(float errorAbs) {
  // Jauh dari target: kecepatan konstan yang aman.
  if (errorAbs >= 25.0f) return PERIODE_PULSA_CEPAT_US;

  // 25° sampai 10°: mulai perlambatan ringan.
  if (errorAbs >= 10.0f) {
    float r = (25.0f - errorAbs) / 15.0f;
    return PERIODE_PULSA_CEPAT_US + static_cast<uint32_t>(r * 1400.0f);
  }

  // 10° sampai 4°: perlambatan lebih kuat mendekati target.
  if (errorAbs >= 4.0f) {
    float r = (10.0f - errorAbs) / 6.0f;
    return 5900UL + static_cast<uint32_t>(r * 2600.0f);
  }

  // Sangat dekat target: bergerak lambat untuk mencegah overshoot/getaran.
  return PERIODE_PULSA_DEKAT_US;
}

uint32_t rampPeriode(uint32_t sekarang, uint32_t target) {
  if (sekarang > target) {
    uint32_t beda = sekarang - target;
    uint32_t langkah = beda < PERUBAHAN_PERIODE_PER_PULSA_US
      ? beda : PERUBAHAN_PERIODE_PER_PULSA_US;
    return sekarang - langkah;
  }

  if (sekarang < target) {
    uint32_t beda = target - sekarang;
    uint32_t langkah = beda < PERUBAHAN_PERIODE_PER_PULSA_US
      ? beda : PERUBAHAN_PERIODE_PER_PULSA_US;
    return sekarang + langkah;
  }

  return sekarang;
}

const char *namaHasilGerak(HasilGerak hasil) {
  switch (hasil) {
    case GERAK_BERHASIL:     return "BERHASIL";
    case GERAK_STOP:         return "STOP";
    case GERAK_LIMIT_ATAS:   return "LIMIT_ATAS";
    case GERAK_LIMIT_BAWAH:  return "LIMIT_BAWAH";
    case GERAK_SENSOR_ERROR: return "SENSOR_ERROR";
    case GERAK_ARAH_SALAH:   return "ARAH_SALAH";
    case GERAK_TIMEOUT:      return "TIMEOUT";
    default:                 return "GAGAL";
  }
}

HasilGerak gerakKeTarget(float target, bool izinkanKeluarDariLimitBawah,
                         const String &judulLCD = "GERAK MOTOR") {
  if (!zeroSiap) return GERAK_GAGAL;
  if (target < TARGET_MIN || target > TARGET_MAX) return GERAK_GAGAL;
  targetGerakDashboard = target;
  if (!updateSudut()) return GERAK_SENSOR_ERROR;

  float errorAwal = target - sudutRelatif;
  bool arahNaik = errorAwal > 0.0f;
  if (fabsf(errorAwal) <= TOLERANSI_SUDUT) return GERAK_BERHASIL;

  if (limitAtasAktif()) {
    motorMati();
    return GERAK_LIMIT_ATAS;
  }

  if (holdingLimitBawah) {
    if (arahNaik && izinkanKeluarDariLimitBawah) {
      holdingLimitBawah = false;
      izinKeluarLimitBawah = true;
      setMotor(true);
      delay(100);
    } else {
      return GERAK_LIMIT_BAWAH;
    }
  }

  if (limitBawahAktif()) {
    if (arahNaik && izinkanKeluarDariLimitBawah) {
      izinKeluarLimitBawah = true;
    } else {
      aktifkanHoldingLimitBawah();
      return stopSaatDelayHolding ? GERAK_STOP : GERAK_LIMIT_BAWAH;
    }
  }

  if (!motorAktif) {
    setMotor(true);
    delay(100);
  }

  sedangBergerak = true;
  digitalWrite(DIR_PIN, arahNaik ? DIR_NAIK : DIR_TURUN);
  delayMicroseconds(100);

  unsigned long waktuMulai = millis();
  unsigned long waktuTampil = 0;
  uint32_t periodeUs = PERIODE_PULSA_MULAI_US;
  uint8_t pulsaSejakSensor = 0;
  uint16_t pulsaCekArah = 0;
  uint8_t gagalSensor = 0;
  bool arahDiperiksa = false;
  float sudutAwalCek = sudutRelatif;

  while (sedangBergerak) {
    if (millis() - waktuMulai > TIMEOUT_GERAK_MS) {
      motorMati();
      return GERAK_TIMEOUT;
    }

    if (stopDiminta()) return GERAK_STOP;

    // Monitor magnet live ditunda selama motor bergerak agar pembacaan
    // register dan pencetakan Serial tidak memotong ritme pulsa STEP.

    if (limitAtasAktif()) {
      motorMati();
      return GERAK_LIMIT_ATAS;
    }

    if (limitBawahAktif() && !arahNaik) {
      aktifkanHoldingLimitBawah();
      return stopSaatDelayHolding ? GERAK_STOP : GERAK_LIMIT_BAWAH;
    }

    float error = target - sudutRelatif;
    float errorAbs = fabsf(error);

    bool tercapai = arahNaik
      ? sudutRelatif >= target - TOLERANSI_SUDUT
      : sudutRelatif <= target + TOLERANSI_SUDUT;

    if (tercapai) {
      digitalWrite(STEP_PIN, LOW);
      sedangBergerak = false;
      motorAktif = true;
      updateSudut();
      return GERAK_BERHASIL;
    }

    uint32_t periodeTujuan = periodeTargetSmooth(errorAbs);
    periodeUs = rampPeriode(periodeUs, periodeTujuan);
    kirimSatuPulsaPeriode(periodeUs);
    pulsaSejakSensor++;
    if (!arahDiperiksa) pulsaCekArah++;

    uint8_t intervalSensor;
    if (errorAbs <= 4.0f) intervalSensor = UPDATE_SENSOR_DEKAT;
    else if (errorAbs <= 12.0f) intervalSensor = UPDATE_SENSOR_SEDANG;
    else intervalSensor = UPDATE_SENSOR_JAUH;

    if (pulsaSejakSensor >= intervalSensor) {
      pulsaSejakSensor = 0;
      if (!updateSudutGerakCepat()) {
        gagalSensor++;
        if (gagalSensor >= BATAS_GAGAL_SENSOR_BERURUTAN) {
          motorMati();
          return GERAK_SENSOR_ERROR;
        }
      } else {
        gagalSensor = 0;
      }
    }

    if (!arahDiperiksa && pulsaCekArah >= 30) {
      if (!updateSudutGerakCepat()) {
        motorMati();
        return GERAK_SENSOR_ERROR;
      }

      float perubahan = sudutRelatif - sudutAwalCek;
      bool cukup = fabsf(perubahan) >= 0.20f;
      bool benar = arahNaik ? perubahan > 0.0f : perubahan < 0.0f;

      if (cukup && !benar) {
        motorMati();
        return GERAK_ARAH_SALAH;
      }

      // Setelah 90 pulsa, pemeriksaan awal dianggap selesai agar backlash
      // tidak membuat motor berhenti palsu.
      if (cukup || pulsaCekArah >= 90) arahDiperiksa = true;
    }

    if (millis() - waktuTampil >= INTERVAL_LCD_GERAK_MS) {
      waktuTampil = millis();
      lcdBaris(0, judulLCD);
      lcdBaris(1, arahNaik ? "Arah: NAIK" : "Arah: TURUN");
      lcdBaris(2, "T:" + String(target, 1) + " A:" + String(sudutRelatif, 1));
      lcdBaris(3, "STOP = hentikan");
    }
  }

  return GERAK_GAGAL;
}

bool tungguAman(unsigned long durasiMs, const String &judul,
                 bool tampilHitungMundur = true) {
  unsigned long mulai = millis();

  while (millis() - mulai < durasiMs) {
    if (stopDiminta()) return false;
    if (limitAtasAktif()) {
      motorMati();
      return false;
    }

    unsigned long sisa = durasiMs - (millis() - mulai);
    if (tampilHitungMundur) {
      lcdBaris(0, judul);
      lcdBaris(1, "Motor: " + String(motorAktif ? "HOLD" : "BEBAS"));
      lcdBaris(2, "Sisa: " + String((sisa + 999UL) / 1000UL) + " detik");
      lcdBaris(3, "STOP = batal");
    }
    delay(20);
  }

  return true;
}

// ================================================================
// LITTLEFS: SETUP, BACA, SIMPAN, HAPUS, FORMAT
// ================================================================
void buatFileSistem() {
  if (!littleFSSiap) return;

  if (!LittleFS.exists(FILE_DATA)) {
    File file = LittleFS.open(FILE_DATA, FILE_WRITE);
    if (file) {
      file.println(HEADER_DATA);
      file.close();
    }
  }

  if (!LittleFS.exists(FILE_LOG)) {
    File file = LittleFS.open(FILE_LOG, FILE_WRITE);
    if (file) {
      file.println("nomor,tahap,keterangan");
      file.close();
    }
  }
}

bool simpanTargetLatihanKeConfig(float target) {
  target = constrain(target, 5.0f, TARGET_MAX);
  targetLatihanAktif = target;
  targetGerakDashboard = target;

  if (!littleFSSiap) return false;

  File file = LittleFS.open(FILE_CONFIG, FILE_WRITE);
  if (!file) return false;

  file.print("target_latihan=");
  file.println(targetLatihanAktif, 1);
  file.close();
  return true;
}

void muatTargetLatihanDariConfig() {
  targetLatihanAktif = TARGET_LATIHAN_DEFAULT;
  if (!littleFSSiap || !LittleFS.exists(FILE_CONFIG)) {
    simpanTargetLatihanKeConfig(targetLatihanAktif);
    return;
  }

  File file = LittleFS.open(FILE_CONFIG, FILE_READ);
  if (!file) return;

  String baris = file.readStringUntil('\n');
  file.close();
  baris.trim();

  int posisiSamaDengan = baris.indexOf('=');
  if (posisiSamaDengan >= 0) {
    float nilai = baris.substring(posisiSamaDengan + 1).toFloat();
    if (nilai >= 5.0f && nilai <= TARGET_MAX) {
      targetLatihanAktif = nilai;
      return;
    }
  }

  targetLatihanAktif = TARGET_LATIHAN_DEFAULT;
  simpanTargetLatihanKeConfig(targetLatihanAktif);
}

void setupLittleFS() {
  littleFSSiap = LittleFS.begin(true);
  if (!littleFSSiap) {
    Serial.println("LittleFS gagal dimulai.");
    return;
  }
  buatFileSistem();
  muatTargetLatihanDariConfig();
}

String fieldCSV(const String &baris, uint8_t indeks) {
  int awal = 0;
  uint8_t nomorField = 0;

  for (int i = 0; i <= static_cast<int>(baris.length()); i++) {
    bool akhirField = i == static_cast<int>(baris.length()) || baris.charAt(i) == ',';
    if (akhirField) {
      if (nomorField == indeks) return baris.substring(awal, i);
      nomorField++;
      awal = i + 1;
    }
  }
  return "";
}

bool parseDataBaris(const String &baris, DataTersimpan &hasil) {
  hasil.valid = false;
  if (baris.length() == 0 || baris.startsWith("nomor,")) return false;

  hasil.nomorData = fieldCSV(baris, 0).toInt();
  hasil.target = fieldCSV(baris, 1).toFloat();
  hasil.repTarget = fieldCSV(baris, 2).toInt();
  hasil.repSelesai = fieldCSV(baris, 3).toInt();
  hasil.pre[0] = fieldCSV(baris, 4).toFloat();
  hasil.pre[1] = fieldCSV(baris, 5).toFloat();
  hasil.pre[2] = fieldCSV(baris, 6).toFloat();
  hasil.medianPre = fieldCSV(baris, 7).toFloat();
  hasil.post[0] = fieldCSV(baris, 8).toFloat();
  hasil.post[1] = fieldCSV(baris, 9).toFloat();
  hasil.post[2] = fieldCSV(baris, 10).toFloat();
  hasil.medianPost = fieldCSV(baris, 11).toFloat();
  hasil.delta = fieldCSV(baris, 12).toFloat();
  hasil.skorFuzzy = fieldCSV(baris, 13).toFloat();
  hasil.kategori = fieldCSV(baris, 14);
  hasil.status = fieldCSV(baris, 15);
  hasil.status.trim();
  hasil.valid = hasil.nomorData > 0;
  return hasil.valid;
}

uint32_t nomorDataBerikutnya() {
  if (!littleFSSiap || !LittleFS.exists(FILE_DATA)) return 1;

  File file = LittleFS.open(FILE_DATA, FILE_READ);
  if (!file) return 1;

  uint32_t terbesar = 0;
  bool pertama = true;
  while (file.available()) {
    String baris = file.readStringUntil('\n');
    baris.trim();
    if (pertama) {
      pertama = false;
      continue;
    }
    if (baris.length() == 0) continue;
    uint32_t id = fieldCSV(baris, 0).toInt();
    if (id > terbesar) terbesar = id;
  }
  file.close();
  return terbesar + 1;
}

uint16_t muatDaftarID(uint32_t *daftar, uint16_t kapasitas) {
  if (!littleFSSiap || !LittleFS.exists(FILE_DATA)) return 0;

  File file = LittleFS.open(FILE_DATA, FILE_READ);
  if (!file) return 0;

  uint16_t jumlah = 0;
  bool pertama = true;
  while (file.available() && jumlah < kapasitas) {
    String baris = file.readStringUntil('\n');
    baris.trim();
    if (pertama) {
      pertama = false;
      continue;
    }
    if (baris.length() == 0) continue;
    uint32_t id = fieldCSV(baris, 0).toInt();
    if (id > 0) daftar[jumlah++] = id;
  }
  file.close();
  return jumlah;
}

bool bacaDataID(uint32_t id, DataTersimpan &hasil) {
  hasil.valid = false;
  if (!littleFSSiap || !LittleFS.exists(FILE_DATA)) return false;

  File file = LittleFS.open(FILE_DATA, FILE_READ);
  if (!file) return false;

  bool pertama = true;
  while (file.available()) {
    String baris = file.readStringUntil('\n');
    baris.trim();
    if (pertama) {
      pertama = false;
      continue;
    }
    if (baris.length() == 0) continue;
    if (fieldCSV(baris, 0).toInt() == id) {
      file.close();
      return parseDataBaris(baris, hasil);
    }
  }

  file.close();
  return false;
}

void simpanLog(uint32_t nomor, const String &tahap, const String &ket) {
  if (!littleFSSiap) return;
  File file = LittleFS.open(FILE_LOG, FILE_APPEND);
  if (!file) return;
  file.print(nomor);
  file.print(',');
  file.print(tahap);
  file.print(',');
  file.println(ket);
  file.close();
}

void simpanCheckpoint(const String &tahap) {
  if (!littleFSSiap) return;
  File file = LittleFS.open(FILE_TEMP, FILE_WRITE);
  if (!file) return;
  file.println("nomor,tahap,rep_selesai,median_pre,median_post");
  file.print(dataAktif.nomorData);
  file.print(',');
  file.print(tahap);
  file.print(',');
  file.print(dataAktif.repSelesai);
  file.print(',');
  file.print(dataAktif.medianPre, 2);
  file.print(',');
  file.println(dataAktif.medianPost, 2);
  file.close();
}

void hapusCheckpoint() {
  if (littleFSSiap && LittleFS.exists(FILE_TEMP)) LittleFS.remove(FILE_TEMP);
}

String namaKategoriFuzzy(uint8_t kategori) {
  switch (kategori) {
    case KAT_BELUM:  return "BELUM";
    case KAT_RENDAH: return "RENDAH";
    case KAT_SEDANG: return "SEDANG";
    case KAT_BAIK:   return "BAIK";
    default:         return "TIDAK_DIKETAHUI";
  }
}

bool simpanDataFinal() {
  if (!littleFSSiap) return false;

  File file = LittleFS.open(FILE_DATA, FILE_APPEND);
  if (!file) return false;

  file.print(dataAktif.nomorData); file.print(',');
  file.print(dataAktif.target, 2); file.print(',');
  file.print(dataAktif.repTarget); file.print(',');
  file.print(dataAktif.repSelesai); file.print(',');
  file.print(dataAktif.pre[0], 2); file.print(',');
  file.print(dataAktif.pre[1], 2); file.print(',');
  file.print(dataAktif.pre[2], 2); file.print(',');
  file.print(dataAktif.medianPre, 2); file.print(',');
  file.print(dataAktif.post[0], 2); file.print(',');
  file.print(dataAktif.post[1], 2); file.print(',');
  file.print(dataAktif.post[2], 2); file.print(',');
  file.print(dataAktif.medianPost, 2); file.print(',');
  file.print(dataAktif.delta, 2); file.print(',');
  file.print(dataAktif.skorFuzzy, 2); file.print(',');
  file.print(namaKategoriFuzzy(dataAktif.kategori)); file.print(',');
  file.println(dataAktif.status);
  file.close();

  hapusCheckpoint();
  kirimDataRealtimeWiFi(true);
  return true;
}

bool hapusDataID(uint32_t id) {
  if (!littleFSSiap || !LittleFS.exists(FILE_DATA)) return false;

  File sumber = LittleFS.open(FILE_DATA, FILE_READ);
  if (!sumber) return false;

  File tujuan = LittleFS.open(FILE_HAPUS_TEMP, FILE_WRITE);
  if (!tujuan) {
    sumber.close();
    return false;
  }

  bool dihapus = false;
  bool pertama = true;

  while (sumber.available()) {
    String baris = sumber.readStringUntil('\n');
    baris.trim();

    if (pertama) {
      tujuan.println(HEADER_DATA);
      pertama = false;
      continue;
    }

    if (baris.length() == 0) continue;
    if (fieldCSV(baris, 0).toInt() == id) {
      dihapus = true;
      continue;
    }
    tujuan.println(baris);
  }

  sumber.close();
  tujuan.close();

  if (!dihapus) {
    LittleFS.remove(FILE_HAPUS_TEMP);
    return false;
  }

  LittleFS.remove(FILE_DATA);
  if (!LittleFS.rename(FILE_HAPUS_TEMP, FILE_DATA)) {
    // Upaya pemulihan minimal: buat ulang file data agar filesystem tetap valid.
    File file = LittleFS.open(FILE_DATA, FILE_WRITE);
    if (file) {
      file.println(HEADER_DATA);
      file.close();
    }
    return false;
  }

  return true;
}

bool formatSeluruhLittleFS() {
  motorMati();
  if (!littleFSSiap) return false;

  bool hasil = LittleFS.format();
  littleFSSiap = LittleFS.begin(true);
  if (!hasil || !littleFSSiap) return false;

  buatFileSistem();
  targetLatihanAktif = TARGET_LATIHAN_DEFAULT;
  simpanTargetLatihanKeConfig(targetLatihanAktif);
  return true;
}

// ================================================================
// FUZZY MAMDANI
// ================================================================
float mfSegitiga(float x, float a, float b, float c) {
  if (x <= a || x >= c) return 0.0f;
  if (x == b) return 1.0f;
  if (x < b) return (x - a) / (b - a);
  return (c - x) / (c - b);
}

float mfTrapesium(float x, float a, float b, float c, float d) {
  if (x < a || x > d) return 0.0f;
  if (x >= b && x <= c) return 1.0f;

  if (x >= a && x < b) {
    if (a == b) return 1.0f;
    return (x - a) / (b - a);
  }

  if (x > c && x <= d) {
    if (c == d) return 1.0f;
    return (d - x) / (d - c);
  }

  return 0.0f;
}

float mfOutput(uint8_t kategori, float y) {
  switch (kategori) {
    case KAT_BELUM:  return mfTrapesium(y, 0, 0, 15, 30);
    case KAT_RENDAH: return mfSegitiga(y, 20, 35, 50);
    case KAT_SEDANG: return mfSegitiga(y, 40, 60, 75);
    case KAT_BAIK:   return mfTrapesium(y, 65, 80, 100, 100);
    default:         return 0.0f;
  }
}

float hitungFuzzyMamdani(float sudutPost, float deltaSudut) {
  float post = constrain(sudutPost, 0.0f, 100.0f);
  float delta = constrain(deltaSudut, -20.0f, 30.0f);

  float muPost[3] = {
    mfTrapesium(post, 0, 0, 20, 40),
    mfSegitiga(post, 30, 50, 70),
    mfTrapesium(post, 60, 80, 100, 100)
  };

  float muDelta[4] = {
    mfTrapesium(delta, -20, -20, -5, 0),
    mfSegitiga(delta, -1, 3, 7),
    mfSegitiga(delta, 5, 12, 20),
    mfTrapesium(delta, 15, 25, 30, 30)
  };

  const uint8_t aturan[3][4] = {
    {KAT_BELUM,  KAT_RENDAH, KAT_SEDANG, KAT_SEDANG},
    {KAT_BELUM,  KAT_RENDAH, KAT_SEDANG, KAT_BAIK},
    {KAT_RENDAH, KAT_SEDANG, KAT_BAIK,   KAT_BAIK}
  };

  float kekuatanKategori[4] = {0, 0, 0, 0};
  for (uint8_t i = 0; i < 3; i++) {
    for (uint8_t j = 0; j < 4; j++) {
      float kekuatan = min(muPost[i], muDelta[j]);
      uint8_t kategori = aturan[i][j];
      kekuatanKategori[kategori] = max(
        kekuatanKategori[kategori], kekuatan
      );
    }
  }

  float pembilang = 0.0f;
  float penyebut = 0.0f;
  for (float y = 0.0f; y <= 100.0f; y += 0.5f) {
    float agregasi = 0.0f;
    for (uint8_t kategori = 0; kategori < 4; kategori++) {
      float potong = min(
        kekuatanKategori[kategori], mfOutput(kategori, y)
      );
      agregasi = max(agregasi, potong);
    }
    pembilang += y * agregasi;
    penyebut += agregasi;
  }

  if (penyebut <= 0.0001f) return 0.0f;
  return pembilang / penyebut;
}

uint8_t kategoriDariSkor(float skor) {
  if (skor < 30.0f) return KAT_BELUM;
  if (skor < 50.0f) return KAT_RENDAH;
  if (skor < 75.0f) return KAT_SEDANG;
  return KAT_BAIK;
}

float median3(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

// ================================================================
// PENGUKURAN PRE-TEST / POST-TEST
// Tombol OK digunakan untuk mengonfirmasi nilai sudut maksimum.
// ================================================================
bool konfirmasiNilaiSudut(const String &nama, uint8_t percobaan,
                          float nilai) {
  bersihkanEventTombol();
  while (true) {
    lcdBaris(0, "KONFIRMASI " + nama);
    lcdBaris(1, "Percobaan " + String(percobaan) + "/3");
    lcdBaris(2, "Sudut maks: " + String(nilai, 1));
    lcdBaris(3, "OK=Simpan BACK=Ulang");

    updateSemuaTombol();
    if (ambilEvent(tombolOK)) return true;
    if (ambilEvent(tombolBack)) return false;
    if (ambilEvent(tombolStop)) return false;
    delay(20);
  }
}

bool ukurTigaPercobaan(const String &nama, float hasil[3]) {
  motorMati();
  if (!zeroSiap) return false;

  for (uint8_t p = 0; p < 3; p++) {
    bool selesaiPercobaan = false;

    while (!selesaiPercobaan) {
      // Tunggu lengan kembali mendekati posisi awal.
      while (true) {
        if (stopDiminta()) return false;
        if (!updateSudut()) {
          lcdBaris(0, nama + " " + String(p + 1) + "/3");
          lcdBaris(1, "AS5600 ERROR");
          lcdBaris(2, "Periksa sensor");
          lcdBaris(3, "BACK = batal");
          delay(100);
          continue;
        }

        lcdBaris(0, nama + " " + String(p + 1) + "/3");
        lcdBaris(1, "Kembali ke posisi 0");
        lcdBaris(2, "Sudut: " + String(sudutRelatif, 1));
        lcdBaris(3, "BACK=Batal");

        updateSemuaTombol();
        if (ambilEvent(tombolBack)) return false;
        if (sudutRelatif <= SUDUT_SIAP_TEST) break;
        delay(30);
      }

      float maksimum = 0.0f;
      bersihkanEventTombol();

      while (true) {
        if (stopDiminta()) return false;
        if (limitAtasAktif()) return false;
        if (!updateSudut()) continue;
        if (sudutRelatif > maksimum) maksimum = sudutRelatif;

        lcdBaris(0, nama + " " + String(p + 1) + "/3");
        lcdBaris(1, "Sudut: " + String(sudutRelatif, 1));
        lcdBaris(2, "Maks  : " + String(maksimum, 1));
        lcdBaris(3, "OK = konfirmasi");

        updateSemuaTombol();
        if (ambilEvent(tombolBack)) return false;
        if (ambilEvent(tombolOK)) {
          if (konfirmasiNilaiSudut(nama, p + 1, maksimum)) {
            hasil[p] = maksimum;
            selesaiPercobaan = true;
            lcdBaris(0, nama + " TERSIMPAN");
            lcdBaris(1, "Percobaan " + String(p + 1) + "/3");
            lcdBaris(2, "Nilai: " + String(maksimum, 1));
            lcdBaris(3, p < 2 ? "Kembali ke 0" : "Selesai");
            delay(900);
          }
          break;
        }
        delay(20);
      }
    }
  }

  return true;
}

bool tampilMedianDanTunggu(const String &nama, const float nilai[3],
                           float median) {
  bersihkanEventTombol();
  while (true) {
    lcdBaris(0, "HASIL " + nama);
    lcdBaris(1, "P1:" + String(nilai[0], 1) + " P2:" + String(nilai[1], 1));
    lcdBaris(2, "P3:" + String(nilai[2], 1));
    lcdBaris(3, "Med:" + String(median, 1) + " OK=Lanjut");

    updateSemuaTombol();
    if (ambilEvent(tombolOK)) return true;
    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return false;
    delay(20);
  }
}

// ================================================================
// LATIHAN LENGKAP
// ================================================================
void resetDataAktif() {
  dataAktif.nomorData = nomorDataBerikutnya();
  dataAktif.target = targetLatihanAktif;
  dataAktif.repTarget = REPETISI_LATIHAN;
  dataAktif.repSelesai = 0;
  for (uint8_t i = 0; i < 3; i++) {
    dataAktif.pre[i] = 0.0f;
    dataAktif.post[i] = 0.0f;
  }
  dataAktif.medianPre = 0.0f;
  dataAktif.medianPost = 0.0f;
  dataAktif.delta = 0.0f;
  dataAktif.skorFuzzy = 0.0f;
  dataAktif.kategori = KAT_BELUM;
  dataAktif.status = "BELUM_SELESAI";
}

bool konfirmasiMulaiLatihan() {
  bersihkanEventTombol();
  while (true) {
    lcdBaris(0, "LATIHAN DATA " + String(dataAktif.nomorData));
    lcdBaris(1, "Target: " + String(dataAktif.target, 1) + " deg");
    lcdBaris(2, "Repetisi: " + String(dataAktif.repTarget));
    lcdBaris(3, "OK=Lanjut BACK=Batal");

    updateSemuaTombol();
    if (ambilEvent(tombolOK)) return true;
    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return false;
    delay(20);
  }
}

bool hitungMundur3Detik() {
  for (int i = 3; i >= 1; i--) {
    lcdBaris(0, "MULAI LATIHAN");
    lcdBaris(1, "Pastikan posisi aman");
    lcdBaris(2, "Mulai dalam " + String(i));
    lcdBaris(3, "STOP selalu aktif");

    unsigned long awal = millis();
    while (millis() - awal < 1000UL) {
      if (stopDiminta()) return false;
      delay(10);
    }
  }
  return true;
}

bool jalankan10Repetisi() {
  for (uint16_t rep = 0; rep < dataAktif.repTarget; rep++) {
    lcdBaris(0, "LATIHAN DATA " + String(dataAktif.nomorData));
    lcdBaris(1, "Rep " + String(rep + 1) + "/" + String(dataAktif.repTarget) + " NAIK");
    lcdBaris(2, "Target: " + String(dataAktif.target, 1));
    lcdBaris(3, "STOP = hentikan");

    HasilGerak naik = gerakKeTarget(
      dataAktif.target, true,
      "REP " + String(rep + 1) + "/" + String(dataAktif.repTarget)
    );

    if (naik != GERAK_BERHASIL) {
      simpanLog(dataAktif.nomorData, "LATIHAN_NAIK", namaHasilGerak(naik));
      return false;
    }

    if (!tungguAman(WAKTU_TAHAN_TARGET_MS, "TAHAN TARGET")) {
      simpanLog(dataAktif.nomorData, "TAHAN", "STOP/FAULT");
      return false;
    }

    HasilGerak turun = gerakKeTarget(
      0.0f, true,
      "REP " + String(rep + 1) + "/" + String(dataAktif.repTarget)
    );

    bool kembaliOK = turun == GERAK_BERHASIL || turun == GERAK_LIMIT_BAWAH;
    if (!kembaliOK) {
      simpanLog(dataAktif.nomorData, "LATIHAN_TURUN", namaHasilGerak(turun));
      return false;
    }

    dataAktif.repSelesai = rep + 1;
    simpanCheckpoint("REP_" + String(dataAktif.repSelesai) + "_SELESAI");

    lcdBaris(0, "REPETISI SELESAI");
    lcdBaris(1, String(dataAktif.repSelesai) + "/" + String(dataAktif.repTarget));
    lcdBaris(2, "Target tetap " + String(dataAktif.target, 0));
    lcdBaris(3, "");
    delay(500);

    if (rep + 1 < dataAktif.repTarget) {
      if (!tungguAman(JEDA_ANTAR_REPETISI_MS, "JEDA ANTAR REP")) return false;
    }
  }

  return true;
}

bool tungguIstirahatPost() {
  motorMati();
  unsigned long mulai = millis();

  while (millis() - mulai < WAKTU_ISTIRAHAT_POST_MS) {
    if (stopDiminta()) return false;

    unsigned long sisa = WAKTU_ISTIRAHAT_POST_MS - (millis() - mulai);
    unsigned long menit = sisa / 60000UL;
    unsigned long detik = (sisa % 60000UL) / 1000UL;

    lcdBaris(0, "WAKTU ISTIRAHAT");
    lcdBaris(1, "Motor: BEBAS");
    lcdBaris(2, "Sisa " + String(menit) + ":" + (detik < 10 ? "0" : "") + String(detik));
    lcdBaris(3, "STOP = batal");
    delay(100);
  }

  bersihkanEventTombol();
  while (true) {
    lcdBaris(0, "ISTIRAHAT SELESAI");
    lcdBaris(1, "Siap post-test");
    lcdBaris(2, "Motor tetap bebas");
    lcdBaris(3, "OK=Lanjut BACK=Batal");

    updateSemuaTombol();
    if (ambilEvent(tombolOK)) return true;
    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return false;
    delay(20);
  }
}

bool tampilHasilDanSimpan() {
  dataAktif.delta = dataAktif.medianPost - dataAktif.medianPre;
  dataAktif.skorFuzzy = hitungFuzzyMamdani(
    dataAktif.medianPost, dataAktif.delta
  );
  dataAktif.kategori = kategoriDariSkor(dataAktif.skorFuzzy);
  dataAktif.status = "SELESAI";

  bersihkanEventTombol();
  uint8_t halaman = 0;

  while (true) {
    if (halaman == 0) {
      lcdBaris(0, "HASIL DATA " + String(dataAktif.nomorData));
      lcdBaris(1, "Pre : " + String(dataAktif.medianPre, 1));
      lcdBaris(2, "Post: " + String(dataAktif.medianPost, 1));
      lcdBaris(3, "Delta:" + String(dataAktif.delta, 1) + " OK>Lanjut");
    } else {
      lcdBaris(0, "FUZZY MAMDANI");
      lcdBaris(1, "Skor: " + String(dataAktif.skorFuzzy, 1));
      lcdBaris(2, "Kategori: " + namaKategoriFuzzy(dataAktif.kategori));
      lcdBaris(3, "OK=Simpan BACK=Batal");
    }

    updateSemuaTombol();

    if (ambilEvent(tombolBack)) {
      dataAktif.status = "BATAL_SIMPAN";
      simpanLog(dataAktif.nomorData, "HASIL", "BATAL_SIMPAN");
      return false;
    }

    if (ambilEvent(tombolStop)) return false;

    if (ambilEvent(tombolOK)) {
      if (halaman == 0) {
        halaman = 1;
      } else {
        bool berhasil = simpanDataFinal();
        lcdBaris(0, berhasil ? "DATA TERSIMPAN" : "GAGAL SIMPAN DATA");
        lcdBaris(1, "Data " + String(dataAktif.nomorData));
        lcdBaris(2, berhasil ? "LittleFS: BERHASIL" : "LittleFS: ERROR");
        lcdBaris(3, "OK = Menu Utama");

        bersihkanEventTombol();
        while (true) {
          updateSemuaTombol();
          if (ambilEvent(tombolOK) || ambilEvent(tombolBack)) return berhasil;
          delay(20);
        }
      }
    }

    delay(20);
  }
}

void jalankanMenuLatihan() {
  tahapDashboard = "PERSIAPAN_LATIHAN";
  motorMati();
  resetDataAktif();
  hapusCheckpoint();

  if (!konfirmasiMulaiLatihan()) return;
  if (!setZeroDenganOK("LATIHAN")) return;

  tahapDashboard = "PRE_TEST";
  simpanCheckpoint("MULAI_PRETEST");
  if (!ukurTigaPercobaan("PRE-TEST", dataAktif.pre)) {
    simpanLog(dataAktif.nomorData, "PRETEST", "DIBATALKAN");
    motorMati();
    return;
  }

  dataAktif.medianPre = median3(
    dataAktif.pre[0], dataAktif.pre[1], dataAktif.pre[2]
  );
  simpanCheckpoint("PRETEST_SELESAI");
  if (!tampilMedianDanTunggu("PRE-TEST", dataAktif.pre, dataAktif.medianPre)) return;

  if (!hitungMundur3Detik()) return;
  tahapDashboard = "LATIHAN";
  simpanCheckpoint("MULAI_LATIHAN");

  if (!jalankan10Repetisi()) {
    dataAktif.status = "LATIHAN_DIHENTIKAN";
    simpanLog(dataAktif.nomorData, "LATIHAN", "DIBATALKAN/FAULT");
    motorMati();
    return;
  }

  simpanCheckpoint("LATIHAN_SELESAI");
  motorMati();

  lcdBaris(0, "LATIHAN SELESAI");
  lcdBaris(1, "Repetisi: " + String(dataAktif.repSelesai) + "/" + String(dataAktif.repTarget));
  lcdBaris(2, "Motor dibuat bebas");
  lcdBaris(3, "Mulai istirahat");
  delay(1200);

  tahapDashboard = "ISTIRAHAT";
  if (!tungguIstirahatPost()) return;

  tahapDashboard = "POST_TEST";
  simpanCheckpoint("MULAI_POSTTEST");
  if (!ukurTigaPercobaan("POST-TEST", dataAktif.post)) {
    simpanLog(dataAktif.nomorData, "POSTTEST", "DIBATALKAN");
    return;
  }

  dataAktif.medianPost = median3(
    dataAktif.post[0], dataAktif.post[1], dataAktif.post[2]
  );
  simpanCheckpoint("POSTTEST_SELESAI");
  if (!tampilMedianDanTunggu("POST-TEST", dataAktif.post, dataAktif.medianPost)) return;

  tahapDashboard = "HASIL";
  tampilHasilDanSimpan();
  motorMati();
  tahapDashboard = "IDLE";
  kirimDataRealtimeWiFi(true);
}

// ================================================================
// UJI TARGET
// ================================================================
void jalankanMenuUjiTarget() {
  motorMati();
  if (!setZeroDenganOK("UJI TARGET")) return;

  float targetUji = 10.0f;
  bersihkanEventTombol();

  while (true) {
    lcdBaris(0, "UJI TARGET MOTOR");
    lcdBaris(1, "Target: " + String(targetUji, 0) + " deg");
    lcdBaris(2, "MODE:+5 ZERO:-5");
    lcdBaris(3, "OK=Uji BACK=Menu");

    updateSemuaTombol();

    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) {
      motorMati();
      return;
    }

    if (ambilEvent(tombolMode)) {
      targetUji += 5.0f;
      if (targetUji > TARGET_MAX) targetUji = TARGET_MAX;
    }

    if (ambilEvent(tombolZero)) {
      targetUji -= 5.0f;
      if (targetUji < 5.0f) targetUji = 5.0f;
    }

    if (ambilEvent(tombolOK)) {
      // Konfirmasi kedua menggunakan OK.
      bersihkanEventTombol();
      bool mulai = false;
      while (true) {
        lcdBaris(0, "KONFIRMASI TARGET");
        lcdBaris(1, "Motor ke " + String(targetUji, 0) + " deg");
        lcdBaris(2, "Pastikan posisi aman");
        lcdBaris(3, "OK=Mulai BACK=Batal");

        updateSemuaTombol();
        if (ambilEvent(tombolOK)) { mulai = true; break; }
        if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) break;
        delay(20);
      }

      if (!mulai) continue;

      HasilGerak hasil = gerakKeTarget(targetUji, true, "UJI TARGET");
      updateSudut();

      lcdBaris(0, hasil == GERAK_BERHASIL ? "TARGET TERCAPAI" : "UJI TARGET GAGAL");
      lcdBaris(1, "Target: " + String(targetUji, 1));
      lcdBaris(2, "Aktual: " + String(sudutRelatif, 1));
      lcdBaris(3, "Error: " + String(fabsf(targetUji - sudutRelatif), 1));
      delay(1300);

      if (hasil == GERAK_BERHASIL) {
        bersihkanEventTombol();
        while (true) {
          lcdBaris(0, "UJI TARGET SELESAI");
          lcdBaris(1, "OK = kembali 0");
          lcdBaris(2, "MODE = uji lagi");
          lcdBaris(3, "BACK = menu");

          updateSemuaTombol();
          if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) {
            motorMati();
            return;
          }
          if (ambilEvent(tombolMode)) {
            motorMati();
            break;
          }
          if (ambilEvent(tombolOK)) {
            HasilGerak kembali = gerakKeTarget(0.0f, true, "KEMBALI KE 0");
            if (kembali != GERAK_BERHASIL && kembali != GERAK_LIMIT_BAWAH) {
              motorMati();
            }
            break;
          }
          delay(20);
        }
      } else {
        motorMati();
      }
    }

    delay(20);
  }
}

// ================================================================
// PANTAU SUDUT DAN PENETAPAN TARGET OTOMATIS
// Motor selalu OFF/FREE. Setelah zero, pasien menggerakkan lengan sampai
// sudut maksimum yang masih nyaman. Nilai maksimum terfilter langsung
// menjadi target sudut untuk menu LATIHAN tanpa pengaturan tambah/kurang.
// ================================================================
String statusMagnetPendekLCD(KondisiMagnetAS5600 kondisi) {
  switch (kondisi) {
    case MAGNET_JARAK_OK:         return "OK";
    case MAGNET_TERLALU_JAUH:     return "JAUH";
    case MAGNET_TERLALU_DEKAT:    return "DEKAT";
    case MAGNET_TIDAK_TERDETEKSI: return "TIDAK ADA";
    case MAGNET_I2C_ERROR:        return "I2C ERROR";
    default:                      return "CEK";
  }
}

bool tetapkanTargetDariMaksNyaman(float maksNyaman) {
  // Sudut maksimum nyaman langsung menjadi target latihan.
  // Nilai dibatasi oleh rentang mekanik alat, lalu dibulatkan 0,1 derajat.
  float targetBaru = constrain(maksNyaman, 5.0f, TARGET_MAX);
  targetBaru = roundf(targetBaru * 10.0f) / 10.0f;

  bersihkanEventTombol();
  while (true) {
    lcdBaris(0, "PAKAI MAKS NYAMAN?");
    lcdBaris(1, "Maks : " + String(maksNyaman, 1) + " deg");
    lcdBaris(2, "Target: " + String(targetBaru, 1) + " deg");
    lcdBaris(3, "OK=Ya BACK=Batal");

    updateSemuaTombol();

    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) {
      return false;
    }

    if (ambilEvent(tombolOK)) {
      bool tersimpan = simpanTargetLatihanKeConfig(targetBaru);

      lcdBaris(0, "TARGET LATIHAN SIAP");
      lcdBaris(1, "Target: " + String(targetLatihanAktif, 1) + " deg");
      lcdBaris(2, tersimpan ? "LittleFS: TERSIMPAN" : "RAM SAJA / FS ERROR");
      lcdBaris(3, "OK = menu utama");

      Serial.print("Maksimum nyaman ditetapkan sebagai target latihan: ");
      Serial.print(targetLatihanAktif, 1);
      Serial.println(" derajat.");

      bersihkanEventTombol();
      while (true) {
        updateSemuaTombol();
        if (ambilEvent(tombolOK) || ambilEvent(tombolBack)) return true;
        if (ambilEvent(tombolStop)) return false;
        delay(20);
      }
    }

    delay(20);
  }
}

void jalankanMenuPantauSudut() {
  motorMati();
  if (!setZeroDenganOK("PANTAU SUDUT")) return;

  float maksReferensi = 0.0f;
  DiagnostikMagnetAS5600 magnet;
  magnet.kondisi = MAGNET_STATUS_TIDAK_VALID;
  unsigned long waktuSensor = 0;
  unsigned long waktuMagnet = 0;
  bool pembacaanValid = updateSudut();

  if (pembacaanValid && sudutRelatif > 0.0f) {
    maksReferensi = sudutRelatif;
  }

  bersihkanEventTombol();
  while (true) {
    unsigned long sekarang = millis();

    if (sekarang - waktuSensor >= 50UL) {
      waktuSensor = sekarang;
      pembacaanValid = updateSudutGerakCepat();
      if (pembacaanValid) {
        float sudutPositif = max(0.0f, sudutRelatif);
        if (sudutPositif > maksReferensi) maksReferensi = sudutPositif;
      }
    }

    if (sekarang - waktuMagnet >= 500UL) {
      waktuMagnet = sekarang;
      bacaDiagnostikMagnet(magnet);
    }

    lcdBaris(0, "PANTAU SUDUT NYAMAN");
    if (pembacaanValid) {
      lcdBaris(1, "Sudut: " + String(max(0.0f, sudutRelatif), 1) + " deg");
      lcdBaris(2, "Maks:" + String(maksReferensi, 1) + " Mag:" +
                    statusMagnetPendekLCD(magnet.kondisi));
    } else {
      lcdBaris(1, "AS5600 ERROR");
      lcdBaris(2, "Mag:" + statusMagnetPendekLCD(magnet.kondisi));
    }
    lcdBaris(3, "OK=Simpan M=Reset");

    updateSemuaTombol();

    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) {
      motorMati();
      return;
    }

    // MODE mengulang pencatatan maksimum nyaman tanpa mengubah zero.
    if (ambilEvent(tombolMode)) {
      maksReferensi = pembacaanValid ? max(0.0f, sudutRelatif) : 0.0f;
      lcdBaris(0, "MAKS NYAMAN DI-RESET");
      lcdBaris(1, "Gerakkan dari 0 deg");
      lcdBaris(2, "sampai batas nyaman");
      lcdBaris(3, "");
      delay(650);
      bersihkanEventTombol();
    }

    // ZERO mengulang kalibrasi titik nol.
    if (ambilEvent(tombolZero)) {
      if (!setZeroDenganOK("PANTAU SUDUT")) return;
      maksReferensi = 0.0f;
      filterGerakSiap = false;
      updateSudut();
      bersihkanEventTombol();
    }

    if (ambilEvent(tombolOK)) {
      if (!pembacaanValid || maksReferensi < 5.0f) {
        lcdBaris(0, "MAKS NYAMAN BELUM ADA");
        lcdBaris(1, "Gerakkan dari 0 deg");
        lcdBaris(2, "sampai batas nyaman");
        lcdBaris(3, "");
        delay(1000);
        bersihkanEventTombol();
        continue;
      }

      if (tetapkanTargetDariMaksNyaman(maksReferensi)) {
        motorMati();
        return;
      }
      bersihkanEventTombol();
    }

    delay(10);
  }
}

// ================================================================
// LIHAT DATA LITTLEFS LANGSUNG DI LCD
// ================================================================
void tampilRecordLCD(const DataTersimpan &data) {
  uint8_t halaman = 0;
  bersihkanEventTombol();

  while (true) {
    if (halaman == 0) {
      lcdBaris(0, "DATA " + String(data.nomorData));
      lcdBaris(1, "Target: " + String(data.target, 1));
      lcdBaris(2, "Rep: " + String(data.repSelesai) + "/" + String(data.repTarget));
      lcdBaris(3, "OK>Halaman BACK<");
    } else if (halaman == 1) {
      lcdBaris(0, "DATA " + String(data.nomorData) + " - PRE");
      lcdBaris(1, "P1:" + String(data.pre[0], 1) + " P2:" + String(data.pre[1], 1));
      lcdBaris(2, "P3:" + String(data.pre[2], 1));
      lcdBaris(3, "Median: " + String(data.medianPre, 1));
    } else if (halaman == 2) {
      lcdBaris(0, "DATA " + String(data.nomorData) + " - POST");
      lcdBaris(1, "P1:" + String(data.post[0], 1) + " P2:" + String(data.post[1], 1));
      lcdBaris(2, "P3:" + String(data.post[2], 1));
      lcdBaris(3, "Median: " + String(data.medianPost, 1));
    } else {
      lcdBaris(0, "DATA " + String(data.nomorData) + " - HASIL");
      lcdBaris(1, "Delta: " + String(data.delta, 1));
      lcdBaris(2, "Skor : " + String(data.skorFuzzy, 1));
      lcdBaris(3, data.kategori);
    }

    updateSemuaTombol();
    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return;
    if (ambilEvent(tombolOK) || ambilEvent(tombolMode)) {
      halaman = (halaman + 1) % 4;
    }
    if (ambilEvent(tombolZero)) {
      halaman = halaman == 0 ? 3 : halaman - 1;
    }
    delay(20);
  }
}

void jalankanMenuLihatData() {
  const uint16_t MAKS_ID = 100;
  uint32_t daftarID[MAKS_ID];
  uint16_t jumlah = muatDaftarID(daftarID, MAKS_ID);

  if (jumlah == 0) {
    lcdBaris(0, "DATA LITTLEFS");
    lcdBaris(1, "Belum ada data");
    lcdBaris(2, "latihan tersimpan");
    lcdBaris(3, "BACK = kembali");
    while (digitalRead(BTN_BACK) == HIGH && digitalRead(BTN_STOP) == HIGH) {
      delay(20);
    }
    return;
  }

  int indeks = 0;
  bersihkanEventTombol();

  while (true) {
    lcdBaris(0, "DATA LITTLEFS");
    lcdBaris(1, "Jumlah data: " + String(jumlah));
    lcdBaris(2, "Pilih Data " + String(daftarID[indeks]));
    lcdBaris(3, "M/Z pilih OK buka");

    updateSemuaTombol();

    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return;
    if (ambilEvent(tombolMode)) indeks = (indeks + 1) % jumlah;
    if (ambilEvent(tombolZero)) indeks = (indeks - 1 + jumlah) % jumlah;

    if (ambilEvent(tombolOK)) {
      DataTersimpan data;
      if (bacaDataID(daftarID[indeks], data)) tampilRecordLCD(data);
      else {
        lcdBaris(0, "GAGAL BACA DATA");
        lcdBaris(1, "Data " + String(daftarID[indeks]));
        lcdBaris(2, "LittleFS error");
        lcdBaris(3, "");
        delay(1200);
      }
      bersihkanEventTombol();
    }

    delay(20);
  }
}

// ================================================================
// KELOLA DATA
// ================================================================
void menuHapusSatuData() {
  const uint16_t MAKS_ID = 100;
  uint32_t daftarID[MAKS_ID];
  uint16_t jumlah = muatDaftarID(daftarID, MAKS_ID);

  if (jumlah == 0) {
    lcdBaris(0, "HAPUS SATU DATA");
    lcdBaris(1, "Belum ada data");
    lcdBaris(2, "");
    lcdBaris(3, "BACK = kembali");
    while (digitalRead(BTN_BACK) == HIGH && digitalRead(BTN_STOP) == HIGH) delay(20);
    return;
  }

  int indeks = 0;
  bersihkanEventTombol();

  while (true) {
    lcdBaris(0, "HAPUS SATU DATA");
    lcdBaris(1, "Pilih Data " + String(daftarID[indeks]));
    lcdBaris(2, "MODE/ZERO = pilih");
    lcdBaris(3, "OK=Lanjut BACK=<");

    updateSemuaTombol();
    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return;
    if (ambilEvent(tombolMode)) indeks = (indeks + 1) % jumlah;
    if (ambilEvent(tombolZero)) indeks = (indeks - 1 + jumlah) % jumlah;

    if (ambilEvent(tombolOK)) {
      uint32_t id = daftarID[indeks];
      DataTersimpan data;
      bacaDataID(id, data);

      bersihkanEventTombol();
      bool lanjut = false;
      while (true) {
        lcdBaris(0, "HAPUS DATA " + String(id) + "?");
        lcdBaris(1, "Pre:" + String(data.medianPre, 1) + " Post:" + String(data.medianPost, 1));
        lcdBaris(2, "Tidak dapat kembali");
        lcdBaris(3, "OK=Lanjut BACK=Batal");

        updateSemuaTombol();
        if (ambilEvent(tombolOK)) { lanjut = true; break; }
        if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) break;
        delay(20);
      }

      if (!lanjut) continue;

      if (!tungguTahanTombolOK(3000UL, "KONFIRMASI HAPUS")) continue;

      bool hasil = hapusDataID(id);
      lcdBaris(0, hasil ? "DATA DIHAPUS" : "HAPUS GAGAL");
      lcdBaris(1, "Data " + String(id));
      lcdBaris(2, hasil ? "LittleFS diperbarui" : "Periksa filesystem");
      lcdBaris(3, "OK = kembali");

      bersihkanEventTombol();
      while (true) {
        updateSemuaTombol();
        if (ambilEvent(tombolOK) || ambilEvent(tombolBack)) return;
        delay(20);
      }
    }

    delay(20);
  }
}

void menuFormatLittleFS() {
  motorMati();
  bersihkanEventTombol();

  bool lanjut = false;
  while (true) {
    lcdBaris(0, "FORMAT LITTLEFS");
    lcdBaris(1, "Semua data dihapus");
    lcdBaris(2, "secara permanen");
    lcdBaris(3, "OK=Lanjut BACK=Batal");

    updateSemuaTombol();
    if (ambilEvent(tombolOK)) { lanjut = true; break; }
    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return;
    delay(20);
  }

  if (!lanjut) return;
  if (!tungguTahanTombolOK(5000UL, "PERINGATAN FORMAT")) return;

  lcdBaris(0, "MEMFORMAT DATA");
  lcdBaris(1, "Mohon tunggu...");
  lcdBaris(2, "Jangan matikan alat");
  lcdBaris(3, "");

  bool hasil = formatSeluruhLittleFS();

  lcdBaris(0, hasil ? "FORMAT BERHASIL" : "FORMAT GAGAL");
  lcdBaris(1, hasil ? "Jumlah data: 0" : "LittleFS error");
  lcdBaris(2, hasil ? "Data berikut: 1" : "Periksa sistem");
  lcdBaris(3, "OK = kembali");

  bersihkanEventTombol();
  while (true) {
    updateSemuaTombol();
    if (ambilEvent(tombolOK) || ambilEvent(tombolBack)) return;
    delay(20);
  }
}

void menuInfoLittleFS() {
  uint32_t daftar[100];
  uint16_t jumlah = muatDaftarID(daftar, 100);

  bersihkanEventTombol();
  while (true) {
    lcdBaris(0, "INFO LITTLEFS");
    lcdBaris(1, "Data: " + String(jumlah));
    lcdBaris(2, "Pakai: " + String(LittleFS.usedBytes() / 1024UL) + " KB");
    lcdBaris(3, "Total:" + String(LittleFS.totalBytes() / 1024UL) + " KB BACK<");

    updateSemuaTombol();
    if (ambilEvent(tombolBack) || ambilEvent(tombolStop) || ambilEvent(tombolOK)) return;
    delay(20);
  }
}

String namaMenuKelola(MenuKelola menu) {
  switch (menu) {
    case KELOLA_HAPUS_SATU: return "HAPUS 1 DATA";
    case KELOLA_FORMAT:     return "FORMAT LITTLEFS";
    case KELOLA_INFO:       return "INFO PENYIMPANAN";
    default:                return "KELOLA DATA";
  }
}

void jalankanMenuKelolaData() {
  menuKelolaAktif = KELOLA_HAPUS_SATU;
  bersihkanEventTombol();

  while (true) {
    lcdBaris(0, "KELOLA DATA");
    lcdBaris(1, "> " + namaMenuKelola(menuKelolaAktif));
    lcdBaris(2, "MODE/ZERO = pilih");
    lcdBaris(3, "OK=Buka BACK=Menu");

    updateSemuaTombol();
    if (ambilEvent(tombolBack) || ambilEvent(tombolStop)) return;
    if (ambilEvent(tombolMode)) {
      menuKelolaAktif = static_cast<MenuKelola>(
        (menuKelolaAktif + 1) % JUMLAH_MENU_KELOLA
      );
    }
    if (ambilEvent(tombolZero)) {
      menuKelolaAktif = static_cast<MenuKelola>(
        (menuKelolaAktif + JUMLAH_MENU_KELOLA - 1) % JUMLAH_MENU_KELOLA
      );
    }
    if (ambilEvent(tombolOK)) {
      if (menuKelolaAktif == KELOLA_HAPUS_SATU) menuHapusSatuData();
      else if (menuKelolaAktif == KELOLA_FORMAT) menuFormatLittleFS();
      else menuInfoLittleFS();
      bersihkanEventTombol();
    }
    delay(20);
  }
}

// ================================================================
// MENU UTAMA
// ================================================================
String namaMenuUtama(MenuUtama menu) {
  switch (menu) {
    case MENU_LATIHAN:     return "LATIHAN";
    case MENU_UJI_TARGET:  return "UJI TARGET";
    case MENU_PANTAU_SUDUT:return "PANTAU SUDUT";
    case MENU_LIHAT_DATA:  return "LIHAT DATA";
    case MENU_KELOLA_DATA: return "KELOLA DATA";
    default:               return "MENU";
  }
}

void tampilMenuUtama() {
  uint32_t daftar[100];
  uint16_t jumlah = littleFSSiap ? muatDaftarID(daftar, 100) : 0;

  lcdBaris(0, "MENU UTAMA");
  lcdBaris(1, "> " + namaMenuUtama(menuUtamaAktif));
  lcdBaris(2, "Data tersimpan: " + String(jumlah));
  lcdBaris(3, "MODE/ZERO OK=Pilih");
}

void jalankanPilihanMenuUtama() {
  bersihkanEventTombol();
  operasiLCDSedangAktif = true;
  tahapDashboard = namaMenuUtama(menuUtamaAktif);

  switch (menuUtamaAktif) {
    case MENU_LATIHAN:     jalankanMenuLatihan(); break;
    case MENU_UJI_TARGET:  jalankanMenuUjiTarget(); break;
    case MENU_PANTAU_SUDUT:jalankanMenuPantauSudut(); break;
    case MENU_LIHAT_DATA:  jalankanMenuLihatData(); break;
    case MENU_KELOLA_DATA: jalankanMenuKelolaData(); break;
    default: break;
  }

  motorMati();
  operasiLCDSedangAktif = false;
  stopDariDashboard = false;
  tahapDashboard = "IDLE";
  bersihkanEventTombol();
  kirimDataRealtimeWiFi(true);
}

// ================================================================
// SERIAL DIAGNOSTIK
// Seluruh fitur di bagian ini hanya dikontrol melalui Serial Monitor.
// Kontrol manual tetap hanya melalui Serial; menu LCD memiliki PANTAU SUDUT.
// ================================================================
void cetakFileSerial(const char *path, const String &judul) {
  Serial.println();
  Serial.println("========== " + judul + " ==========");
  if (!littleFSSiap || !LittleFS.exists(path)) {
    Serial.println("File tidak tersedia.");
    Serial.println("====================================");
    return;
  }

  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    Serial.println("Gagal membuka file.");
    Serial.println("====================================");
    return;
  }

  while (file.available()) Serial.write(file.read());
  file.close();
  Serial.println("====================================");
}

void tampilSemuaDataSerial() {
  cetakFileSerial(FILE_DATA, "DATA LATIHAN CSV");
}

void tampilDataTerakhirSerial() {
  if (!littleFSSiap || !LittleFS.exists(FILE_DATA)) {
    Serial.println("Data tidak tersedia.");
    return;
  }

  File file = LittleFS.open(FILE_DATA, FILE_READ);
  if (!file) return;
  String terakhir = "";
  bool pertama = true;
  while (file.available()) {
    String baris = file.readStringUntil('\n');
    baris.trim();
    if (pertama) {
      pertama = false;
      continue;
    }
    if (baris.length() > 0) terakhir = baris;
  }
  file.close();

  Serial.println();
  Serial.println("========== DATA TERAKHIR ==========");
  if (terakhir.length() == 0) Serial.println("Belum ada data latihan.");
  else {
    Serial.println(HEADER_DATA);
    Serial.println(terakhir);
  }
  Serial.println("===================================");
}

void tampilStatusSerial() {
  updateSudut();
  DiagnostikMagnetAS5600 magnet;
  bacaDiagnostikMagnet(magnet);

  Serial.println();
  Serial.println("========== STATUS SISTEM ==========");
  Serial.print("AS5600       : "); Serial.println(sensorOK ? "OK" : "ERROR");
  Serial.print("Magnet       : "); Serial.println(namaKondisiMagnet(magnet.kondisi));
  Serial.print("MD/ML/MH     : ");
  Serial.print(magnet.md ? 1 : 0); Serial.print('/');
  Serial.print(magnet.ml ? 1 : 0); Serial.print('/');
  Serial.println(magnet.mh ? 1 : 0);
  Serial.print("AGC/Magnitude: "); Serial.print(magnet.agc);
  Serial.print(" / "); Serial.println(magnet.magnitude);
  Serial.print("Zero         : "); Serial.println(zeroSiap ? "SIAP" : "BELUM");
  Serial.print("Raw angle    : "); Serial.println(rawSudut);
  Serial.print("Sudut absolut: "); Serial.println(sudutAbsolut, 2);
  Serial.print("Sudut relatif: "); Serial.println(sudutRelatif, 2);
  Serial.print("Motor        : ");
  if (holdingLimitBawah) Serial.println("HOLDING LIMIT BAWAH");
  else Serial.println(motorAktif ? "ON / HOLDING" : "OFF / FREE");
  Serial.print("Limit atas   : "); Serial.println(limitAtasAktif() ? "AKTIF" : "NORMAL");
  Serial.print("Limit bawah  : "); Serial.println(limitBawahAktif() ? "AKTIF" : "NORMAL");
  Serial.print("Target aktif : "); Serial.println(targetLatihanAktif, 1);
  Serial.print("Repetisi     : "); Serial.println(REPETISI_LATIHAN);
  Serial.print("Pulse/rev    : "); Serial.println(PULSA_PER_PUTARAN_MOTOR);
  Serial.print("Pulse/deg    : "); Serial.println(PULSA_PER_DERAJAT_OUTPUT, 3);
  Serial.print("Manual pulse : "); Serial.println(pulsaManualSerial);
  Serial.print("Manual period: "); Serial.print(periodePulsaManualSerialUs);
  Serial.println(" us");
  Serial.print("Magnet live  : "); Serial.println(monitorMagnetLive ? "ON" : "OFF");
  Serial.print("Error I2C    : "); Serial.println(jumlahErrorI2C);
  Serial.print("Outlier sudut: "); Serial.println(jumlahOutlierSensor);
  Serial.print("Data berikut : "); Serial.println(nomorDataBerikutnya());
  Serial.println("===================================");
}

bool setZeroDariSerial() {
  motorMati();
  DiagnostikMagnetAS5600 info;
  if (!bacaDiagnostikMagnet(info)) {
    Serial.println("ZERO GAGAL: AS5600 tidak dapat diakses.");
    return false;
  }
  if (info.kondisi != MAGNET_JARAK_OK) {
    Serial.print("ZERO GAGAL: status magnet = ");
    Serial.println(namaKondisiMagnet(info.kondisi));
    Serial.println("Atur posisi/jarak magnet sampai status JARAK OK.");
    return false;
  }

  float hasilZero = 0.0f;
  if (!bacaSudutRataRata(hasilZero, 30)) {
    Serial.println("ZERO GAGAL: sudut tidak terbaca.");
    return false;
  }

  sudutZero = hasilZero;
  sudutRelatif = 0.0f;
  sudutFilterGerak = 0.0f;
  filterGerakSiap = true;
  sensorOK = true;
  zeroSiap = true;

  Serial.print("ZERO BERHASIL. Sudut absolut nol = ");
  Serial.print(sudutZero, 2);
  Serial.println(" derajat.");
  return true;
}

bool aktifkanMotorDariSerial() {
  if (limitAtasAktif()) {
    Serial.println("Motor tidak diaktifkan: limit atas aktif.");
    return false;
  }

  if (limitBawahAktif()) {
    aktifkanHoldingLimitBawah();
    Serial.println("Motor holding karena limit bawah aktif.");
    return true;
  }

  setMotor(true);
  delay(100);
  Serial.println("Motor ON / ENABLE / HOLDING.");
  return true;
}

void gerakManualDariSerial(bool arahNaik, uint32_t jumlahPulsa) {
  jumlahPulsa = constrain(jumlahPulsa, PULSA_MANUAL_MIN, PULSA_MANUAL_MAX);

  if (arahNaik && limitAtasAktif()) {
    Serial.println("Gerak NAIK dibatalkan: limit atas aktif.");
    return;
  }
  if (!arahNaik && limitBawahAktif()) {
    aktifkanHoldingLimitBawah();
    Serial.println("Gerak TURUN dibatalkan: limit bawah aktif.");
    return;
  }

  if (holdingLimitBawah) {
    if (!arahNaik) {
      Serial.println("Motor holding limit bawah; hanya arah NAIK yang diizinkan.");
      return;
    }
    holdingLimitBawah = false;
    izinKeluarLimitBawah = true;
    setMotor(false);
    delay(50);
  }

  if (limitBawahAktif() && arahNaik) izinKeluarLimitBawah = true;

  if (!motorAktif) {
    setMotor(true);
    delay(100);
  }

  digitalWrite(DIR_PIN, arahNaik ? DIR_NAIK : DIR_TURUN);
  delayMicroseconds(100);
  sedangBergerak = true;

  uint32_t periodeAwal = periodePulsaManualSerialUs;
  if (periodeAwal < PERIODE_PULSA_MULAI_US) {
    periodeAwal = PERIODE_PULSA_MULAI_US;
  }

  uint32_t pulsaRamp = jumlahPulsa / 4UL;
  if (pulsaRamp > 150UL) pulsaRamp = 150UL;
  if (pulsaRamp < 4UL) pulsaRamp = 0UL;

  Serial.println();
  Serial.println("====== GERAK MANUAL SMOOTH ======");
  Serial.print("Arah          : "); Serial.println(arahNaik ? "NAIK" : "TURUN");
  Serial.print("Jumlah pulsa  : "); Serial.println(jumlahPulsa);
  Serial.print("Periode cepat : "); Serial.print(periodePulsaManualSerialUs);
  Serial.println(" us");
  Serial.print("Pulsa ramp    : "); Serial.println(pulsaRamp);
  Serial.println("Monitor magnet live dijeda selama motor bergerak.");
  Serial.println("Kirim x atau tekan STOP untuk menghentikan.");

  for (uint32_t i = 0; i < jumlahPulsa; i++) {
    if (stopDiminta()) {
      Serial.println("Gerak manual dihentikan.");
      return;
    }

    if (arahNaik && limitAtasAktif()) {
      motorMati();
      Serial.println("LIMIT ATAS AKTIF. Motor OFF.");
      return;
    }

    if (!arahNaik && limitBawahAktif()) {
      aktifkanHoldingLimitBawah();
      Serial.println("LIMIT BAWAH AKTIF. Gerak turun dihentikan.");
      return;
    }

    uint32_t periodeSekarang = periodePulsaManualSerialUs;

    if (pulsaRamp > 0UL && i < pulsaRamp) {
      float rasio = static_cast<float>(i + 1UL) /
                    static_cast<float>(pulsaRamp);
      periodeSekarang = periodeAwal - static_cast<uint32_t>(
        rasio * static_cast<float>(periodeAwal - periodePulsaManualSerialUs)
      );
    } else if (pulsaRamp > 0UL && (jumlahPulsa - i) <= pulsaRamp) {
      uint32_t sisa = jumlahPulsa - i;
      float rasio = static_cast<float>(sisa) /
                    static_cast<float>(pulsaRamp);
      periodeSekarang = periodeAwal - static_cast<uint32_t>(
        rasio * static_cast<float>(periodeAwal - periodePulsaManualSerialUs)
      );
    }

    kirimSatuPulsaPeriode(periodeSekarang);
  }

  sedangBergerak = false;
  digitalWrite(STEP_PIN, LOW);
  updateSudut();
  Serial.print("Gerak manual selesai. Sudut relatif: ");
  Serial.print(sudutRelatif, 2);
  Serial.println(" derajat. Motor tetap ON/holding.");
  Serial.println("==================================");

  if (monitorMagnetLive) layaniMonitorMagnetLive(true);
}

uint32_t ambilAngkaSetelahPerintah(const String &cmd, uint32_t nilaiDefault) {
  int spasi = cmd.indexOf(' ');
  String angka;
  if (spasi >= 0) angka = cmd.substring(spasi + 1);
  else {
    uint16_t i = 0;
    while (i < cmd.length() && !isDigit(cmd.charAt(i))) i++;
    angka = cmd.substring(i);
  }
  angka.trim();
  uint32_t hasil = angka.toInt();
  return hasil > 0 ? hasil : nilaiDefault;
}

void tampilMenuSerial() {
  Serial.println();
  Serial.println("================================================");
  Serial.println(" MENU SERIAL - REHAB SIKU 1-DOF");
  Serial.println(" LCD: LATIHAN, UJI TARGET, PANTAU SUDUT, DATA");
  Serial.println("================================================");
  Serial.println("m / help       : tampilkan menu ini");
  Serial.println("p / status     : tampilkan status sistem");
  Serial.println("magnet         : baca status magnet satu kali");
  Serial.println("magnet on      : monitor magnet live tiap 250 ms");
  Serial.println("magnet off     : hentikan monitor magnet live");
  Serial.println("sensor         : baca sudut satu kali");
  Serial.println("target         : lihat target latihan aktif");
  Serial.println("z              : set posisi sekarang menjadi 0 deg");
  Serial.println("t30            : gerak closed-loop ke target 30 deg");
  Serial.println("t0             : kembali ke target 0 deg");
  Serial.println("u / u500       : manual NAIK, default/500 pulsa");
  Serial.println("d / d500       : manual TURUN, default/500 pulsa");
  Serial.println("pulsa 200      : set jumlah pulsa manual default");
  Serial.println("v5000          : set periode pulsa manual (us)");
  Serial.println("1              : motor ON / holding");
  Serial.println("0              : motor OFF / free");
  Serial.println("x              : stop gerakan dan motor OFF");
  Serial.println("data           : tampilkan /data_latihan.csv");
  Serial.println("last           : tampilkan data latihan terakhir");
  Serial.println("log            : tampilkan /log_sistem.csv");
  Serial.println("temp           : tampilkan /session_temp.csv");
  Serial.println("------------------------------------------------");
  Serial.println("Serial Monitor: 115200 baud, line ending Newline");
  Serial.println("================================================");
}

void prosesSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  String lower = cmd;
  lower.toLowerCase();

  if (lower == "m" || lower == "help" || lower == "menu") {
    tampilMenuSerial();
  } else if (lower == "status" || lower == "p") {
    tampilStatusSerial();
  } else if (lower == "magnet") {
    cetakDiagnostikMagnetSerial(false);
  } else if (lower == "magnet on" || lower == "magnet live" || lower == "mlive") {
    monitorMagnetLive = true;
    waktuMonitorMagnetTerakhir = 0;
    Serial.println("Monitor magnet LIVE = ON. Ketik 'magnet off' untuk berhenti.");
    layaniMonitorMagnetLive(true);
  } else if (lower == "magnet off" || lower == "moff") {
    monitorMagnetLive = false;
    Serial.println("Monitor magnet LIVE = OFF.");
  } else if (lower == "target") {
    Serial.print("Target latihan aktif = ");
    Serial.print(targetLatihanAktif, 1);
    Serial.println(" derajat.");
  } else if (lower == "sensor") {
    if (updateSudut()) {
      Serial.print("RAW="); Serial.print(rawSudut);
      Serial.print(" | ABS="); Serial.print(sudutAbsolut, 2);
      Serial.print(" deg | REL="); Serial.print(sudutRelatif, 2);
      Serial.println(" deg");
    } else {
      Serial.println("AS5600 error.");
    }
  } else if (lower == "z" || lower == "zero") {
    setZeroDariSerial();
  } else if (lower == "1" || lower == "on") {
    aktifkanMotorDariSerial();
  } else if (lower == "0" || lower == "off" || lower == "x") {
    motorMati();
    Serial.println("Motor OFF / FREE.");
  } else if (lower == "data") {
    tampilSemuaDataSerial();
  } else if (lower == "last") {
    tampilDataTerakhirSerial();
  } else if (lower == "log") {
    cetakFileSerial(FILE_LOG, "LOG SISTEM");
  } else if (lower == "temp") {
    cetakFileSerial(FILE_TEMP, "CHECKPOINT SEMENTARA");
  } else if (lower.startsWith("pulsa")) {
    uint32_t nilai = ambilAngkaSetelahPerintah(lower, pulsaManualSerial);
    pulsaManualSerial = constrain(nilai, PULSA_MANUAL_MIN, PULSA_MANUAL_MAX);
    Serial.print("Pulsa manual default = "); Serial.println(pulsaManualSerial);
  } else if (lower.startsWith("v")) {
    uint32_t nilai = ambilAngkaSetelahPerintah(lower, periodePulsaManualSerialUs);
    periodePulsaManualSerialUs = constrain(
      nilai, PERIODE_MANUAL_MIN_US, PERIODE_MANUAL_MAX_US
    );
    Serial.print("Periode pulsa manual = ");
    Serial.print(periodePulsaManualSerialUs);
    Serial.println(" us");
  } else if (lower == "u" || lower.startsWith("u ") ||
             (lower.startsWith("u") && lower.length() > 1 && isDigit(lower.charAt(1)))) {
    uint32_t jumlah = ambilAngkaSetelahPerintah(lower, pulsaManualSerial);
    gerakManualDariSerial(true, jumlah);
  } else if (lower == "d" || lower.startsWith("d ") ||
             (lower.startsWith("d") && lower.length() > 1 && isDigit(lower.charAt(1)))) {
    uint32_t jumlah = ambilAngkaSetelahPerintah(lower, pulsaManualSerial);
    gerakManualDariSerial(false, jumlah);
  } else if (lower.startsWith("t")) {
    String angka = lower.substring(1);
    angka.trim();
    if (angka.length() == 0) {
      Serial.println("Format target: t30");
      return;
    }
    float target = angka.toFloat();
    if (!zeroSiap) {
      Serial.println("Zero belum disetel. Ketik z terlebih dahulu.");
    } else {
      HasilGerak hasil = gerakKeTarget(target, true, "UJI SERIAL");
      Serial.print("Hasil gerak target: ");
      Serial.println(namaHasilGerak(hasil));
      if (updateSudut()) {
        Serial.print("Sudut akhir: "); Serial.print(sudutRelatif, 2);
        Serial.print(" | Error: "); Serial.println(target - sudutRelatif, 2);
      }
    }
  } else {
    Serial.println("Perintah tidak dikenali. Ketik m untuk menu Serial.");
  }
}

// ================================================================
// PEMERIKSAAN BOOT
// ================================================================
void tampilPemeriksaanBoot() {
  bool as5600Ada = cekAlamatI2C(AS5600_ADDR);
  bool magnetAda = as5600Ada && magnetTerdeteksi();

  lcdBaris(0, "PEMERIKSAAN SISTEM");
  lcdBaris(1, "AS5600 : " + String(as5600Ada ? "OK" : "ERROR"));
  lcdBaris(2, "LittleFS: " + String(littleFSSiap ? "OK" : "ERROR"));
  lcdBaris(3, "Magnet  : " + String(magnetAda ? "OK" : "CEK"));

  Serial.print("AS5600: "); Serial.println(as5600Ada ? "OK" : "ERROR");
  Serial.print("Magnet: "); Serial.println(magnetAda ? "OK" : "TIDAK/CEK JARAK");
  Serial.print("LittleFS: "); Serial.println(littleFSSiap ? "OK" : "ERROR");
  delay(1800);

  uint32_t daftar[100];
  uint16_t jumlah = littleFSSiap ? muatDaftarID(daftar, 100) : 0;
  lcdBaris(0, "SISTEM SIAP");
  lcdBaris(1, "Target aktif: " + String(targetLatihanAktif, 0));
  lcdBaris(2, "Data tersimpan: " + String(jumlah));
  lcdBaris(3, "OK = Menu Utama");

  bersihkanEventTombol();
  unsigned long mulai = millis();
  while (millis() - mulai < 2500UL) {
    updateSemuaTombol();
    if (ambilEvent(tombolOK)) break;
    delay(20);
  }
}

// ================================================================
// SETUP DAN LOOP
// ================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(800);

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(ENA_PIN, OUTPUT);
  pinMode(LIMIT_ATAS, INPUT_PULLUP);
  pinMode(LIMIT_BAWAH, INPUT_PULLUP);

  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);
  digitalWrite(ENA_PIN, MOTOR_MATI_LEVEL);

  initTombol(tombolBack, BTN_BACK);
  initTombol(tombolStop, BTN_STOP);
  initTombol(tombolMode, BTN_MODE);
  initTombol(tombolOK, BTN_OK);
  initTombol(tombolZero, BTN_ZERO);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(AS5600_TIMEOUT_MS);

  setupLCD();
  tampilBoot();
  setupLittleFS();
  tampilPemeriksaanBoot();

  mulaiWiFiDashboard();
  layaniWiFiDashboard(true);

  motorMati();
  zeroSiap = false;
  tampilMenuUtama();

  Serial.println("Program siap. Menu PANTAU SUDUT tersedia pada LCD.");
  Serial.println("Ketik m pada Serial Monitor untuk menu diagnostik lengkap.");
  tampilMenuSerial();
}

void loop() {
  // Jangan mematikan motor otomatis di setiap iterasi agar perintah Serial
  // '1' (holding) dapat dipakai. Saat kembali dari menu LCD, motor tetap
  // dimatikan oleh jalur menu yang sudah ada.
  digitalWrite(STEP_PIN, LOW);
  tampilMenuUtama();
  updateSemuaTombol();

  // Proteksi idle ketika motor diaktifkan dari Serial Monitor.
  if (motorAktif && limitAtasAktif()) {
    motorMati();
    Serial.println("LIMIT ATAS AKTIF saat idle. Motor otomatis OFF.");
  }

  if (motorAktif && limitBawahAktif() && !holdingLimitBawah) {
    aktifkanHoldingLimitBawah();
    Serial.println("LIMIT BAWAH AKTIF saat idle. Motor masuk mode holding.");
  }

  if (ambilEvent(tombolStop)) {
    motorMati();
    Serial.println("STOP fisik: motor OFF / FREE.");
  }

  // Saat holding karena limit bawah, tombol BACK dapat melepas holding.
  if (ambilEvent(tombolBack) && holdingLimitBawah) {
    motorMati();
    izinKeluarLimitBawah = limitBawahAktif();
    Serial.println("Holding limit bawah dilepas dengan tombol BACK.");
  }

  if (ambilEvent(tombolMode)) {
    menuUtamaAktif = static_cast<MenuUtama>(
      (menuUtamaAktif + 1) % JUMLAH_MENU_UTAMA
    );
  }

  if (ambilEvent(tombolZero)) {
    menuUtamaAktif = static_cast<MenuUtama>(
      (menuUtamaAktif + JUMLAH_MENU_UTAMA - 1) % JUMLAH_MENU_UTAMA
    );
  }

  if (ambilEvent(tombolOK)) {
    jalankanPilihanMenuUtama();
  }

  layaniMonitorMagnetLive();
  prosesSerial();
  layaniWiFiDashboard(false);
  delay(20);
}
