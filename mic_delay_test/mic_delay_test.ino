/*
 * TES DELAY BUS1 — 4 MIC INMP441, TANPA SD/WIFI/OLED
 *
 * Sketch terpisah, cuma buat eksperimen cepat fix delay ~650ms antara
 * mic1/2 (Bus 0) vs mic3/4 (Bus 1) yang ketemu di analisis rekaman
 * 2026-07-09. Kirim audio interleaved via Serial pakai protokol lama
 * (START_MARKER + timestamp + audio + END_MARKER) supaya bisa langsung
 * dipakai bareng recorder.py yang sudah ada di project ini — TIDAK PERLU
 * ubah recorder.py sama sekali.
 *
 * WIRING (sama seperti esp32.txt.ino, SD card TIDAK dipakai di sketch ini):
 *   Bus I2S 0 (Mic 1 & 2): SCK->D26 | WS->D25 | SD->D33
 *   Bus I2S 1 (Mic 3 & 4): SCK->D18 | WS->D5  | SD->D19
 *   LED: GPIO2 | BTN: D4->GND (INPUT_PULLUP)
 *   OLED: SDA->D21, SCL->D22 (alamat I2C 0x3C) -- opsional, kalau nggak
 *   dipasang, display.begin() gagal dan sketch lanjut jalan tanpa OLED
 *   (grafik cuma lewat Serial Plotter).
 *
 * OLED cuma nampilin grafik pas TIDAK merekam (persis seperti pola di
 * esp32.txt.ino) -- dimatikan saat recording, karena refresh OLED sempat
 * dicurigai jadi salah satu sumber noise di mic3/4 (lihat catatan noise-test
 * di esp32.txt.ino). Menjaga sketch tes ini tetap sama perilakunya biar
 * eksperimen delay nggak kena variabel tambahan yang nggak perlu.
 *
 * CARA PAKAI:
 *   1. Upload sketch ini.
 *   2. Buat lihat grafik level mic realtime (Arduino IDE > Tools > Serial
 *      Plotter, atau Ctrl+Shift+L), baud 921600 -- HANYA jalan saat TIDAK
 *      merekam (pas recording, Serial dipakai buat kirim audio binary ke
 *      recorder.py, jadi teks plotter otomatis berhenti supaya nggak
 *      nyampur/korup datanya).
 *   3. Jalankan: python recorder.py COMx (tutup Serial Plotter dulu kalau
 *      port lagi dipakai bareng, keduanya nggak bisa buka port yang sama).
 *   4. Tekan tombol fisik -> mulai rekam, tekan lagi -> stop (sama seperti biasa).
 *   5. Tepuk sekali di tengah ke-4 mic pas rekam, cek hasilnya.
 *   6. Ganti nilai DMA_BUF_COUNT_BUS1 di bawah, upload ulang, ulangi tes,
 *      bandingin apakah delay mic3/4 berkurang. Awasi juga apakah glitch
 *      "knock" mic3/4 (Bus1 drift/overflow) jadi lebih sering muncul kalau
 *      angkanya diturunkan terlalu jauh -- itu trade-off yang lagi dicari titik
 *      seimbangnya, lihat CLAUDE.md.
 */

#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─────────────────────────────────────────────
// INI YANG DIUBAH-UBAH BUAT EKSPERIMEN
// Firmware asli pakai 24 (butuh besar biar Bus1 nggak overflow/drop sample
// karena drift tanpa APLL) -- dugaan sementara: ini juga penyebab delay
// ~650ms, karena kapasitas antrian besar = banyak audio "nunggu" sebelum
// dikirim. Coba turunkan bertahap (misal 16, lalu 12, lalu 8) dan lihat
// dua hal: (a) apakah delay berkurang, (b) apakah glitch knock makin sering.
#define DMA_BUF_COUNT_BUS1   24
// ─────────────────────────────────────────────

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

// === AUDIO ===
#define SAMPLE_RATE   8000
#define BUFFER_LEN    512       // int32_t per i2s_read per bus
#define MIC1_GAIN     4         // mic1 lemah secara hardware -- kompensasi gain digital
#define GRAPH_MAX    15000

int32_t i2sBuf0[BUFFER_LEN];   // Bus 0: Mic 1 & 2
int32_t i2sBuf1[BUFFER_LEN];   // Bus 1: Mic 3 & 4

// === PROTOKOL SERIAL (sama persis dengan recorder.py) ===
const uint8_t START_MARKER[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
const uint8_t END_MARKER[8]   = {0xCA, 0xFE, 0xBA, 0xBE, 0xCA, 0xFE, 0xBA, 0xBE};

// === OLED ===
#define OLED_WIDTH  128
#define OLED_HEIGHT  64
#define OLED_RESET   -1
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
bool oledOk = false;

// Circular buffer waveform (128 kolom = lebar OLED)
#define HIST_LEN 128
int16_t histMic1[HIST_LEN], histMic2[HIST_LEN];
int16_t histMic3[HIST_LEN], histMic4[HIST_LEN];
uint8_t histIdx = 0;
unsigned long lastOledUpdate = 0;

bool isRecording          = false;
unsigned long lastBtnTime = 0;
bool lastBtnState         = HIGH;

// ─────────────────────────────────────────────

void initI2S(i2s_port_t port, int bck, int ws, int sd, bool apll, int dmaBufCount) {
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
  i2s_driver_install(port, &cfg, 0, NULL);
  i2s_set_pin(port, &pins);
}

void startRecording() {
  isRecording = true;
  Serial.write(START_MARKER, 8);
  uint32_t ts = millis();
  Serial.write((uint8_t*)&ts, 4);   // cuma buat framing, bukan timestamp asli (lihat recorder.py)
}

void stopRecording() {
  isRecording = false;
  Serial.write(END_MARKER, 8);
  Serial.flush();
}

// ─────────────────────────────────────────────
// Gambar satu grafik waveform dari circular buffer histMicX (persis pola
// drawGraph() di esp32.txt.ino, biar hasilnya kebaca sama)
void drawGraph(int16_t *hist, int bottomY, int graphH) {
  for (uint8_t x = 0; x < HIST_LEN; x++) {
    uint8_t i = (histIdx + x) & 0x7F;
    int h = map(constrain((int)hist[i], 0, GRAPH_MAX), 0, GRAPH_MAX, 0, graphH);
    if (h > 0) display.drawFastVLine(x, bottomY - h, h, SSD1306_WHITE);
  }
}

void drawOLED_idle() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 1); display.print("1");
  drawGraph(histMic1, 14, 14);
  display.drawFastHLine(0, 15, 128, SSD1306_WHITE);

  display.setCursor(0, 17); display.print("2");
  drawGraph(histMic2, 30, 14);
  display.drawFastHLine(0, 31, 128, SSD1306_WHITE);

  display.setCursor(0, 33); display.print("3");
  drawGraph(histMic3, 46, 14);
  display.drawFastHLine(0, 47, 128, SSD1306_WHITE);

  display.setCursor(0, 49); display.print("4");
  drawGraph(histMic4, 63, 15);

  display.display();
}

void setup() {
  Serial.setTxBufferSize(8192);
  Serial.begin(921600);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BTN_PIN, INPUT_PULLUP);

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Mic Delay Test");
    display.println("DMA_BUF_BUS1=");
    display.println(DMA_BUF_COUNT_BUS1);
    display.display();
    delay(1500);
  }

  initI2S(I2S_NUM_0, I2S0_SCK, I2S0_WS, I2S0_SD, true, 4);
  initI2S(I2S_NUM_1, I2S1_SCK, I2S1_WS, I2S1_SD, false, DMA_BUF_COUNT_BUS1);

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}

void loop() {
  // ── Tombol fisik start/stop ──
  bool btnNow = digitalRead(BTN_PIN);
  if (btnNow == LOW && lastBtnState == HIGH && (millis() - lastBtnTime > 50)) {
    lastBtnTime = millis();
    if (!isRecording) {
      startRecording();
      digitalWrite(LED_PIN, HIGH);
    } else {
      stopRecording();
      digitalWrite(LED_PIN, LOW);
    }
  }
  lastBtnState = btnNow;

  // ── Baca kedua bus I2S (selalu, sama seperti firmware asli) ──
  size_t bytesRead0 = 0, bytesRead1 = 0;
  i2s_read(I2S_NUM_0, i2sBuf0, sizeof(i2sBuf0), &bytesRead0, portMAX_DELAY);
  i2s_read(I2S_NUM_1, i2sBuf1, sizeof(i2sBuf1), &bytesRead1, portMAX_DELAY);

  int pairs = min(bytesRead0, bytesRead1) / (2 * sizeof(int32_t));

  static int16_t frameBuf[BUFFER_LEN * 4];   // interleaved [m1,m2,m3,m4]... buat dikirim langsung

  long sumMic1 = 0, sumMic2 = 0, sumMic3 = 0, sumMic4 = 0;

  int n = 0;
  for (int j = 0; j < pairs; j++) {
    int idx = j * 2;

    int32_t raw1 = i2sBuf0[idx + 1];  // Mic 1 LEFT
    int32_t raw2 = i2sBuf0[idx];      // Mic 2 RIGHT
    int32_t raw3 = i2sBuf1[idx + 1];  // Mic 3 LEFT
    int32_t raw4 = i2sBuf1[idx];      // Mic 4 RIGHT

    int32_t mic1 = (raw1 >> 16) * MIC1_GAIN;
    mic1 = constrain(mic1, -32768, 32767);

    int16_t s2 = (int16_t)(raw2 >> 16);
    int16_t s3 = (int16_t)(raw3 >> 16);
    int16_t s4 = (int16_t)(raw4 >> 16);

    frameBuf[n++] = (int16_t)mic1;
    frameBuf[n++] = s2;
    frameBuf[n++] = s3;
    frameBuf[n++] = s4;

    sumMic1 += abs(mic1);
    sumMic2 += abs(s2);
    sumMic3 += abs(s3);
    sumMic4 += abs(s4);
  }

  if (isRecording && n > 0) {
    Serial.write((uint8_t*)frameBuf, n * sizeof(int16_t));
  }

  // ── Grafik realtime (Serial Plotter) -- cuma pas TIDAK merekam, biar
  // nggak nyampur sama stream audio binary yang dikirim ke recorder.py ──
  if (!isRecording && pairs > 0) {
    Serial.print("M1:"); Serial.print(sumMic1 / pairs);
    Serial.print(",M2:"); Serial.print(sumMic2 / pairs);
    Serial.print(",M3:"); Serial.print(sumMic3 / pairs);
    Serial.print(",M4:"); Serial.println(sumMic4 / pairs);
  }

  // ── Grafik realtime di OLED -- juga cuma pas TIDAK merekam (lihat catatan
  // noise-isolation di header file ini) ──
  if (pairs > 0) {
    histMic1[histIdx] = (int16_t)constrain((int)(sumMic1 / pairs), 0, 32767);
    histMic2[histIdx] = (int16_t)constrain((int)(sumMic2 / pairs), 0, 32767);
    histMic3[histIdx] = (int16_t)constrain((int)(sumMic3 / pairs), 0, 32767);
    histMic4[histIdx] = (int16_t)constrain((int)(sumMic4 / pairs), 0, 32767);
    histIdx = (histIdx + 1) & 0x7F;
  }

  unsigned long now = millis();
  if (oledOk && !isRecording && (now - lastOledUpdate >= 80)) {
    lastOledUpdate = now;
    drawOLED_idle();
  }
}
