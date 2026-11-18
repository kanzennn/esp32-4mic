/*
 * 4 MIC INMP441 — REKAM LANGSUNG KE SD CARD, KONTROL VIA WIFI AP + WEB UI
 *
 * File ini cuma orchestration (setup/loop). Modul lain (tab .ino terpisah,
 * dikompilasi bareng otomatis oleh Arduino IDE):
 *   - sd_recorder.ino  : primitif WAV/SD level-rendah (tulis 4 file per take, patch header, flush)
 *   - session_model.ino: model data Responden/Huruf/Variasi/Take di atas sd_recorder.ino
 *   - wifi_ap.ino      : ESP32 jadi WiFi Access Point sendiri
 *   - web_ui.ino       : web server (Core 0) — pilih target, file manager, start/stop dari HP
 *
 * OLED:
 *   - Boot    : splash SSID/IP WiFi + status SD selama beberapa detik
 *   - Idle    : 4 grafik waveform (M1–M4) + indikator status SD di pojok kanan atas
 *   - Recording: status bar "● REC MM:SS" + 4 grafik waveform (diaktifkan lagi
 *     2026-07-16 -- sempat dimatikan buat tes isolasi noise mic3/4 vs refresh
 *     OLED, tapi knock glitch itu sudah dikonfirmasi dari DMA drift Bus 1,
 *     bukan OLED, jadi aman diaktifkan. Re-test kalau noise muncul lagi.
 *
 * WIRING:
 *   Bus I2S 0 (Mic 1 & 2): SCK→D26 | WS→D25 | SD→D33
 *     Mic1 L/R → GND (LEFT)  | Mic2 L/R → 3V3 (RIGHT)
 *
 *   Bus I2S 1 (Mic 3 & 4): SCK→D18 | WS→D5 | SD→D19
 *     Mic3 L/R → GND (LEFT)  | Mic4 L/R → 3V3 (RIGHT)
 *
 *   Semua mic: VDD→3V3 | GND→GND
 *   LED  : GPIO 2  |  OLED : SDA→D21, SCL→D22  |  BTN : D4→GND
 *   SD Card (SPI custom, lihat sd_recorder.ino): SCK→D14 | MISO→D34 | MOSI→D27 | CS→D13 
 *
 * LIBRARY: Adafruit SSD1306 | Adafruit GFX
 *
 * CARA PAKAI:
 *   1. Upload sketch
 *   2. Connect HP ke WiFi "ESP32-4MIC" (password di wifi_ap.ino), buka
 *      browser ke IP yang tampil di OLED
 *   3. Di tab "Rekam Baru": pilih/buat Responden → pilih Huruf → pilih/buat
 *      Variasi → tekan Rekam (mengunci target, lalu start/stop lewat
 *      tombol web -- take baru per tekan, nomor otomatis lanjut). Rekam
 *      SEKARANG CUMA DARI APP/WEB -- tombol fisik ESP32 tidak lagi
 *      mulai/stop rekam.
 *   4. Tombol fisik ESP32: tekan sebentar = tampilkan info WiFi/SD di OLED
 *      (handy buat reconnect HP tanpa reboot) | tekan-tahan (≥600ms) =
 *      restart alat.
 *   5. Tab "Lihat Data": browse/download/hapus take yang sudah direkam,
 *      atau cabut SD card langsung ke PC (folder /RSPDN{n}/)
 *
 * CATATAN: Dua bus I2S berjalan independen. Ada kemungkinan drift
 *   beberapa sample antar pasangan mic — cukup untuk keperluan riset.
 *   `recorder.py` (jalur Serial-ke-PC lama) masih ada di repo tapi sudah
 *   tidak dipakai jalur ini — lihat CLAUDE.md.
 */

#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

// Forward declaration manual — Arduino auto-prototype-generator (ctags) gagal
// mendeteksi kedua fungsi ini karena raw string literal HTML besar di
// web_ui.ino (lihat INDEX_HTML) mengacaukan hitungan kurung kurawalnya.
bool initSoftAP();
void startWebServerTask();

// sdReady didefinisikan di sd_recorder.ino, tapi tab itu ke-compile SETELAH
// file ini (urutan alfabetis) -- perlu extern biar bisa dipakai di
// drawOLED_idle() tanpa nunggu urutan concatenation.
extern bool sdReady;
bool checkSDPresent();  // didefinisikan di sd_recorder.ino, dipanggil dari loop() di file ini
bool remountSDCard();   // didefinisikan di sd_recorder.ino

// Didefinisikan di session_model.ino -- forward declaration manual karena
// file ini (esp32.txt.ino) selalu ke-compile PALING AWAL, sebelum
// session_model.ino sempat mendefinisikan fungsi-fungsi ini.
bool startTakeForCurrentTarget();
void closeCurrentTake();
void repairIncompleteTakesAtBoot();

// MAX_VARIASI_TRACK dan struct TakeInfo juga HARUS di sini (bukan di
// session_model.ino tempat aslinya dipakai) -- alasannya sama seperti kasus
// initSoftAP/startWebServerTask: Arduino auto-prototype-generator (ctags)
// nge-hoist prototype findExistingVariasi()/listTakes() (yang makai
// keduanya di parameter) ke titik yang SANGAT AWAL di file gabungan, jauh
// sebelum session_model.ino sempat mendefinisikan macro/struct-nya sendiri
// -- bikin "MAX_VARIASI_TRACK"/"TakeInfo" belum dikenal di titik hoist itu.
#define MAX_VARIASI_TRACK 64
#define MAX_TAKE_TRACK    256
struct TakeInfo {
  uint32_t take;
  bool done;
  uint32_t bytes[4];
};

// === PIN I2S BUS 0 (Mic 1 & 2) ===
#define I2S0_SCK  26
#define I2S0_WS   25
#define I2S0_SD   33

// === PIN I2S BUS 1 (Mic 3 & 4) ===
#define I2S1_SCK  18
#define I2S1_WS   5
#define I2S1_SD   19

// === PIN LAIN ===
#define LED_PIN    2
#define BTN_PIN    4
#define LONG_PRESS_MS  600   // ditahan >= ini = restart alat; kurang dari ini (short press) = tampilkan info WiFi/SD

// === OLED ===
#define OLED_WIDTH  128
#define OLED_HEIGHT  64
#define OLED_RESET   -1
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

// === AUDIO ===
#define SAMPLE_RATE   8000
#define BUFFER_LEN    512       // int32_t per i2s_read per bus
#define THRESHOLD     5000
#define GRAPH_MAX    15000
#define MIC1_GAIN     4     // mic1 lemah secara hardware — kompensasi gain digital

int32_t i2sBuf0[BUFFER_LEN];   // Bus 0: Mic 1 & 2
int32_t i2sBuf1[BUFFER_LEN];   // Bus 1: Mic 3 & 4

// Circular buffer waveform (128 kolom = lebar OLED)
#define HIST_LEN 128
int16_t histMic1[HIST_LEN], histMic2[HIST_LEN];
int16_t histMic3[HIST_LEN], histMic4[HIST_LEN];
uint8_t histIdx = 0;

// === STATE ===
esp_err_t i2s1_install_err = ESP_OK, i2s1_pin_err = ESP_OK;
bool isRecording          = false;
unsigned long lastBtnTime = 0;
bool lastBtnState         = HIGH;
unsigned long btnPressStartMs   = 0;   // waktu tombol mulai ditekan, buat hitung durasi short (info) / long (restart) press
unsigned long infoScreenUntilMs = 0;   // >millis() berarti lagi nampilin layar info (short-press), bukan idle/recording
unsigned long recStartTime   = 0;
unsigned long lastOledUpdate = 0;
unsigned long lastHeapLogMs  = 0;
bool sdPresentNow      = true;   // hasil cek real-time (bukan cuma status mount saat boot)

// Target rekaman saat ini (responden/huruf/variasi) -- diisi lewat POST
// /select (web_ui.ino), dipakai startTakeForCurrentTarget() (session_model.ino)
// buat nentuin nama folder/file. SENGAJA tidak direset oleh
// closeCurrentTake(), supaya app bisa /start berkali-kali buat "ulangi
// target terakhir" (take baru per panggilan) tanpa perlu /select ulang.
int      currentRespondenIdx = -1;
int      currentHurufIdx     = -1;
int      currentVariasiIdx   = -1;
bool     targetSelected      = false;
uint32_t cachedNextTake      = 1;   // preview murah buat /status, bukan otoritatif
uint32_t currentTakeNumber   = 0;   // 0 = tidak sedang rekam

// Dipakai bareng sd_recorder.ino & web_ui.ino (semua tab .ino di folder
// sketch yang sama dikompilasi jadi satu file oleh Arduino IDE, jadi
// variabel global di sini otomatis kelihatan dari tab lain tanpa extern —
// asal dideklarasikan di file utama ini, yang selalu ditempatkan pertama).
SemaphoreHandle_t sdMutex;         // wajib pegang ini sebelum panggil SD.*/File.* dari core manapun
volatile bool webStartRequested = false;
volatile bool webStopRequested  = false;
String currentSessionPath = "";    // contoh: "/RSPDN2/RSPDN2_HRF5_VRS1_3" (base path take aktif, tanpa suffix _MICn.wav/.done), diisi session_model.ino

// WiFi AP — konstanta & state dideklarasikan di sini (bukan di wifi_ap.ino)
// supaya kelihatan dari setup() di file ini juga; initSoftAP() (di
// wifi_ap.ino) yang mengisi apIP.
const char* AP_SSID     = "ESP32-4MIC";
const char* AP_PASSWORD = "12345678";   // WPA2, minimal 8 karakter
IPAddress apIP;

// ─────────────────────────────────────────────

void initI2S(i2s_port_t port, int bck, int ws, int sd, bool apll = true, int dmaBufCount = 4) {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = dmaBufCount,
    .dma_buf_len          = 256,
    .use_apll             = apll,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = bck,
    .ws_io_num    = ws,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = sd
  };
  esp_err_t r1 = i2s_driver_install(port, &cfg, 0, NULL);
  esp_err_t r2 = i2s_set_pin(port, &pins);
  if (port == I2S_NUM_1) { i2s1_install_err = r1; i2s1_pin_err = r2; }
}

// ─────────────────────────────────────────────

void startRecording() {
  if (!targetSelected) {
    Serial.println("[REC] Belum ada target (responden/huruf/variasi) yang dipilih — pilih dulu lewat web UI");
    return;
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  bool ok = startTakeForCurrentTarget();
  xSemaphoreGive(sdMutex);

  if (!ok) {
    Serial.println("[SD] Gagal mulai take baru (SD belum siap / gagal buat file)");
    return;
  }

  recStartTime = millis();
  isRecording  = true;
  Serial.printf("[REC] Mulai: %s\n", currentSessionPath.c_str());
}

void stopRecording() {
  isRecording = false;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  closeCurrentTake();
  xSemaphoreGive(sdMutex);
  Serial.println("[REC] Selesai, file ditutup.");
}

// ─────────────────────────────────────────────
// Gambar satu grafik waveform dari circular buffer histMicX ke kolom
// selebar `width` piksel mulai dari `xOffset`. HIST_LEN (128) sampel
// dirata-rata jadi `width` kolom (mis. width=64 -> tiap kolom = rata-rata
// 2 sampel) supaya rentang waktu yang ditampilkan tetap sama walau kolomnya
// lebih sempit dari lebar layar penuh.
void drawGraph(int16_t *hist, int xOffset, int bottomY, int width, int graphH) {
  int group = HIST_LEN / width;
  if (group < 1) group = 1;
  for (int x = 0; x < width; x++) {
    uint32_t sum = 0;
    for (int g = 0; g < group; g++) {
      uint8_t i = (histIdx + x * group + g) & (HIST_LEN - 1);
      sum += (uint16_t)hist[i];
    }
    int avg = sum / group;
    int h = map(constrain(avg, 0, GRAPH_MAX), 0, GRAPH_MAX, 0, graphH);
    if (h > 0) display.drawFastVLine(xOffset + x, bottomY - h, h, SSD1306_WHITE);
  }
}

// ─────────────────────────────────────────────
// Grid 2x2 waveform M1-M4 (dipakai bareng layar idle & recording), mulai
// dari y=gridTop, tiap sel 64x27 (M1|M2 baris atas, M3|M4 baris bawah).
void drawMicGrid(int gridTop) {
  const int cellW = 64, cellH = 27;
  struct { int16_t *hist; int col; int row; const char *label; } cells[4] = {
    { histMic1, 0, 0, "1" },
    { histMic2, 1, 0, "2" },
    { histMic3, 0, 1, "3" },
    { histMic4, 1, 1, "4" },
  };

  for (int c = 0; c < 4; c++) {
    int x0 = cells[c].col * cellW;
    int y0 = gridTop + cells[c].row * cellH;
    display.setCursor(x0 + 1, y0);
    display.print(cells[c].label);
    drawGraph(cells[c].hist, x0, y0 + cellH - 1, cellW, 18);
  }

  display.drawFastVLine(cellW, gridTop, cellH * 2, SSD1306_WHITE);
  display.drawFastHLine(0, gridTop + cellH, 128, SSD1306_WHITE);
}

// ─────────────────────────────────────────────
// OLED info: SSID/IP WiFi + status SD -- sama kontennya kayak splash boot,
// dipakai lagi buat tombol fisik yang di-short-press (biar nggak perlu
// reboot cuma buat ngintip ulang IP/SSID-nya).
void drawOLED_bootInfo() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("4-Mic Recorder");
  display.println();
  display.println("WiFi AP:");
  display.println(AP_SSID);
  display.println(apIP.toString());
  display.println();
  if (!sdReady)           display.println("SD: Failed");
  else if (!sdPresentNow) display.println("SD: Disconnected");
  else                    display.println("SD: Connected");
  display.display();
}

// ─────────────────────────────────────────────
// OLED restarting: feedback singkat sebelum ESP.restart() -- karena restart
// itu abrupt (motong koneksi WiFi klien seketika), kasih tau dulu daripada
// cuma tiba-tiba mati.
void drawOLED_restarting() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 24);
  display.println("Restarting...");
  display.display();
}

// ─────────────────────────────────────────────
// OLED idle: status bar SD (real-time) di atas + grid 2x2 waveform M1-M4.
//   Row 0-9   : status bar SD ("SD: Connected" / "Disconnected" / "Failed")
//   Row 10-63 : grid 2 kolom x 2 baris
void drawOLED_idle() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Status bar SD -- real-time, bukan cuma status mount saat boot
  display.setCursor(0, 1);
  if (!sdReady)           display.print("SD: Failed");
  else if (!sdPresentNow) display.print("SD: Disconnected");
  else                    display.print("SD: Connected");
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);

  drawMicGrid(10);
  display.display();
}

// ─────────────────────────────────────────────
// OLED recording: status bar "● MM:SS  SD:.." di atas + grid 2x2 waveform.
//   Row 0-9   : dot REC + durasi rekaman + status SD (ringkas, ruang sempit)
//   Row 10-63 : grid 2 kolom x 2 baris
void drawOLED_recording() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.fillCircle(4, 4, 3, SSD1306_WHITE);   // dot indikator REC

  unsigned long elapsed = (millis() - recStartTime) / 1000;
  char ts[6];
  snprintf(ts, sizeof(ts), "%02lu:%02lu", elapsed / 60, elapsed % 60);
  display.setCursor(11, 1);
  display.print(ts);

  // Status SD diringkas (bukan "Connected"/"Disconnected" penuh) --
  // barisnya cuma 128px, harus berbagi sama dot REC + timer.
  display.setCursor(55, 1);
  if (!sdReady)           display.print("SD:Fail");
  else if (!sdPresentNow) display.print("SD:Disc");
  else                    display.print("SD:OK");

  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);

  drawMicGrid(10);
  display.display();
}

// ─────────────────────────────────────────────
// Task terpisah (Core 0, sama seperti web server) buat cek keberadaan SD
// real-time. checkSDPresent() paksa 1 transaksi SPI fisik -- kalau kartu
// dicabut, itu nunggu timeout driver (bisa ratusan ms - detik) sebelum
// gagal. Awalnya ini dipanggil langsung dari loop() di Core 1, tapi itu
// bikin OLED (dan seluruh loop, termasuk timing I2S) ngelag nunggu timeout
// itu tiap kali kartu nggak ada. Dipisah ke task sendiri di Core 0 supaya
// yang nge-lag cuma task ini, bukan loop() utama.
void sdPresenceCheckTask(void *param) {
  for (;;) {
    if (!isRecording) {
      xSemaphoreTake(sdMutex, portMAX_DELAY);
      if (sdPresentNow) {
        // Sebelumnya dianggap ada -- cukup tes ringan (baca 1 byte).
        sdPresentNow = checkSDPresent();
      } else {
        // Sebelumnya dianggap dicabut -- coba mount ulang (mungkin kartu
        // baru saja dipasang lagi, sama atau berbeda). Kalau berhasil,
        // jalankan lagi repair take belum selesai, sama seperti kalau baru
        // boot -- jaga-jaga kartu yang baru dipasang bawa take belum ditutup.
        sdPresentNow = remountSDCard();
        if (sdPresentNow) repairIncompleteTakesAtBoot();
      }
      xSemaphoreGive(sdMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ─────────────────────────────────────────────

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(921600);

  delay(500);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BTN_PIN, INPUT_PULLUP);

  sdMutex = xSemaphoreCreateMutex();

  Serial.printf("[HEAP] pre-WiFi: free=%u largestBlock=%u\n",
                ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  bool sdOk = initSDCard();
  Serial.printf("[SD] init %s\n", sdOk ? "OK" : "GAGAL");
  if (sdOk) repairIncompleteTakesAtBoot();

  bool apOk = initSoftAP();
  Serial.printf("[WiFi] AP %s, SSID=%s IP=%s\n", apOk ? "OK" : "GAGAL", AP_SSID, apIP.toString().c_str());
  Serial.printf("[HEAP] post-WiFi: free=%u largestBlock=%u\n",
                ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  startWebServerTask();

  xTaskCreatePinnedToCore(
    sdPresenceCheckTask, "sdPresenceCheck",
    2560,               // stack kecil -- cuma buka/baca 1 byte lalu tidur
    NULL,
    1,                   // prioritas rendah, sama seperti web server task
    NULL,
    0                     // Core 0 -- Core 1 tetap fokus I2S + SD write recording
  );

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    // lanjut tanpa OLED
  } else {
    drawOLED_bootInfo();
    delay(4000);
    drawOLED_idle();
  }

  initI2S(I2S_NUM_0, I2S0_SCK, I2S0_WS, I2S0_SD, true);
  initI2S(I2S_NUM_1, I2S1_SCK, I2S1_WS, I2S1_SD, false, 24);  // wajib false: APLL dipakai bersama, tidak boleh dipakai 2 bus sekaligus. (Sempat dicoba 48, tapi kemungkinan bikin i2s_driver_install gagal karena RAM internal habis -> i2s_read hang selamanya -> seluruh loop() freeze, tombol/LED/OLED tidak respons. Dikembalikan ke 24. Sekarang ada WiFi juga yang makan RAM — cek log [HEAP] di atas kalau ada masalah serupa.)

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}

// ─────────────────────────────────────────────

void loop() {
  // ── Cek tombol fisik -- sekarang khusus info/restart, BUKAN rekam lagi.
  // Rekam cuma lewat app/web (/start, /stop). Dipisah jadi 2 edge (tekan
  // lalu lepas) supaya bisa bedain short press (info) dari long press
  // (restart). ──
  bool btnNow = digitalRead(BTN_PIN);
  if (btnNow == LOW && lastBtnState == HIGH && (millis() - lastBtnTime > 50)) {
    // baru ditekan -- cuma catat waktu, actionnya nunggu dilepas
    lastBtnTime = millis();
    btnPressStartMs = millis();
  }
  if (btnNow == HIGH && lastBtnState == LOW && (millis() - lastBtnTime > 50)) {
    // baru dilepas -- putuskan short/long press dari durasi ditahan
    lastBtnTime = millis();
    unsigned long held = millis() - btnPressStartMs;

    if (held >= LONG_PRESS_MS) {
      // Long press -- restart alat, kapan pun (termasuk pas lagi rekam:
      // take yang sedang jalan otomatis diperbaiki pas boot ulang lewat
      // repairIncompleteTakesAtBoot(), sama seperti kalau listrik putus
      // mendadak -- lihat catatan flush()/`.done` marker di sd_recorder.ino).
      Serial.println("[BTN] Long press -- restart ESP32");
      drawOLED_restarting();
      delay(600);
      ESP.restart();
    } else {
      // Short press -- tampilkan layar info WiFi/SD (kayak splash boot)
      // beberapa detik. Kalau pas lagi rekam, layar recording tetap
      // diprioritaskan (lihat bagian "Update OLED" di bawah).
      infoScreenUntilMs = millis() + 4000;
    }
  }
  lastBtnState = btnNow;

  // ── Cek request start/stop dari web UI (di-set dari task Core 0) ──
  // Dieksekusi di sini (Core 1), bukan langsung dari handler web, supaya
  // semua mutasi file SD tetap di satu core saja.
  if (webStartRequested) {
    webStartRequested = false;
    if (!isRecording && targetSelected) {
      startRecording();
      digitalWrite(LED_PIN, HIGH);
    }
  }
  if (webStopRequested) {
    webStopRequested = false;
    if (isRecording) {
      stopRecording();
      digitalWrite(LED_PIN, LOW);
      drawOLED_idle();
    }
  }

  // ── Baca kedua I2S bus ──
  size_t bytesRead0 = 0, bytesRead1 = 0;
  i2s_read(I2S_NUM_0, i2sBuf0, sizeof(i2sBuf0), &bytesRead0, portMAX_DELAY);
  i2s_read(I2S_NUM_1, i2sBuf1, sizeof(i2sBuf1), &bytesRead1, portMAX_DELAY);

  int pairs = min(bytesRead0, bytesRead1) / (2 * sizeof(int32_t));

  long long sumMic1 = 0, sumMic2 = 0, sumMic3 = 0, sumMic4 = 0;
  int count = 0;

  // Buffer per-channel (bukan interleaved lagi) — langsung cocok buat
  // ditulis ke 4 file WAV terpisah di SD.
  static int16_t mic1Buf[BUFFER_LEN], mic2Buf[BUFFER_LEN];
  static int16_t mic3Buf[BUFFER_LEN], mic4Buf[BUFFER_LEN];
  int recCount = 0;

  for (int j = 0; j < pairs; j++) {
    int idx = j * 2;

    int32_t raw1 = i2sBuf0[idx + 1];  // Mic 1 LEFT
    int32_t raw2 = i2sBuf0[idx];      // Mic 2 RIGHT
    int32_t raw3 = i2sBuf1[idx + 1];  // Mic 3 LEFT
    int32_t raw4 = i2sBuf1[idx];      // Mic 4 RIGHT

    int32_t mic1 = (raw1 >> 16) * MIC1_GAIN;
    mic1 = constrain(mic1, -32768, 32767);

    sumMic1 += abs(mic1);
    sumMic2 += abs(raw2 >> 14);
    sumMic3 += abs(raw3 >> 14);
    sumMic4 += abs(raw4 >> 14);
    count++;

    if (isRecording) {
      mic1Buf[recCount] = (int16_t)mic1;
      mic2Buf[recCount] = (int16_t)(raw2 >> 16);
      mic3Buf[recCount] = (int16_t)(raw3 >> 16);
      mic4Buf[recCount] = (int16_t)(raw4 >> 16);
      recCount++;
    }
  }

  // ── Tulis ke SD ──
  if (isRecording && recCount > 0) {
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    writeFrameToSD(mic1Buf, mic2Buf, mic3Buf, mic4Buf, recCount);
    xSemaphoreGive(sdMutex);
  }

  // ── Update circular buffer waveform ──
  if (count > 0) {
    histMic1[histIdx] = (int16_t)constrain((int)(sumMic1 / count), 0, 32767);
    histMic2[histIdx] = (int16_t)constrain((int)(sumMic2 / count), 0, 32767);
    histMic3[histIdx] = (int16_t)constrain((int)(sumMic3 / count), 0, 32767);
    histMic4[histIdx] = (int16_t)constrain((int)(sumMic4 / count), 0, 32767);
    histIdx = (histIdx + 1) & 0x7F;
  }

  // ── Serial Plotter (hanya saat idle) ──
  if (!isRecording && count > 0) {
    int a1 = (int)(sumMic1/count), a2 = (int)(sumMic2/count);
    int a3 = (int)(sumMic3/count), a4 = (int)(sumMic4/count);
    bool active = (a1 > THRESHOLD || a2 > THRESHOLD ||
                   a3 > THRESHOLD || a4 > THRESHOLD);
    digitalWrite(LED_PIN, active ? HIGH : LOW);
    Serial.print("M1:"); Serial.print(a1);
    Serial.print(",M2:"); Serial.print(a2);
    Serial.print(",M3:"); Serial.print(a3);
    Serial.print(",M4:"); Serial.print(a4);
    Serial.print(",Batas:"); Serial.print(THRESHOLD);
    Serial.print(",B1raw0:"); Serial.print(i2sBuf1[0]);
    Serial.print(",B1raw1:"); Serial.print(i2sBuf1[1]);
    Serial.print(",I2S1err:"); Serial.print(i2s1_install_err);
    Serial.print("/"); Serial.println(i2s1_pin_err);
  }

  // ── Log heap PAS LAGI REKAM (buat diagnosa web nggak bisa diakses setelah
  // rekam lama) -- printout normal di atas sengaja mati pas rekam, jadi ini
  // satu-satunya jendela buat lihat kondisi heap real-time selama itu.
  if (millis() - lastHeapLogMs >= (isRecording ? 2000 : 5000)) {
    lastHeapLogMs = millis();
    Serial.printf("[HEAP-%s] t=%lus free=%u largestBlock=%u apStations=%d\n",
                  isRecording ? "REC" : "IDLE",
                  (isRecording ? (millis() - recStartTime) : millis()) / 1000,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  WiFi.softAPgetStationNum());
  }

  // ── Update OLED ──
  // Layar info (dari short-press tombol fisik) menang atas idle selama
  // belum timeout -- tapi kalau mulai rekam di tengah-tengah itu (dari
  // app/web, satu-satunya cara rekam sekarang), recording tetap harus
  // langsung kelihatan, bukan info.
  unsigned long now = millis();
  if (isRecording) infoScreenUntilMs = 0;  // batalkan info screen kalau ternyata mulai rekam
  bool showInfo = !isRecording && now < infoScreenUntilMs;
  if (now - lastOledUpdate >= (isRecording ? 100 : 80)) {
    lastOledUpdate = now;
    if (isRecording) drawOLED_recording();
    else if (showInfo) drawOLED_bootInfo();
    else drawOLED_idle();
  }
}
