/*
 * TES SD CARD SENDIRIAN — STAGE 1 BRING-UP (lihat CLAUDE.md)
 *
 * Sketch terpisah, TANPA I2S/WiFi/OLED sama sekali. Tujuannya cuma satu:
 * pastikan SD card kebaca/tertulis dengan STABIL di pin custom (bukan
 * default VSPI/HSPI) sebelum digabung lagi sama I2S dan WiFi.
 *
 * PENTING -- WIRING: pastikan SD card sudah dipindah ke pin CUSTOM ini,
 * BUKAN port lama (default VSPI 18/19/23/5) yang kemarin bentrok sama I2S
 * Bus 1 dan bikin noise:
 *   SCK  -> D14
 *   MISO -> D34  (input-only, TIDAK ada pull-up internal -- kalau bacaan
 *                 flaky/gagal, tambah resistor pull-up eksternal 10k ke 3V3)
 *   MOSI -> D27
 *   CS   -> D13
 *   VCC  -> 3V3 | GND -> GND
 *
 * CARA PAKAI:
 *   1. Upload sketch ini, buka Serial Monitor @ 921600 baud.
 *   2. Baca hasil deteksi kartu (tipe & ukuran) di awal.
 *   3. Sketch otomatis ulang-ulang tulis+baca+verifikasi file test setiap
 *      3 detik, isinya beda tiap kali (bukan konten sama berulang) supaya
 *      nggak ketipu cache. Biarin jalan beberapa menit -- kalau ada MISO
 *      yang flaky, harusnya kelihatan sebagai kegagalan verifikasi
 *      sesekali, bukan gagal total dari awal.
 *   4. LED nyala solid = ada kegagalan verifikasi yang pernah kejadian
 *      (baca log Serial buat detail byte yang beda). LED kedip pendek tiap
 *      putaran = satu siklus tulis/baca/verifikasi sukses.
 *   5. Kalau sudah stabil lama (nggak ada FAIL), baru pertimbangkan naikkan
 *      clock SPI dari 4MHz (lihat SD_FREQ_HZ di bawah) -- jangan naikkan
 *      duluan sebelum baseline 4MHz terbukti stabil.
 */

#include <SPI.h>
#include <SD.h>
#include <FS.h>

// === DIAGNOSTIK: ganti ke 1 buat coba pin default VSPI (SCK18/MISO19/
// MOSI23/CS5) -- HANYA untuk isolasi masalah di sketch berdiri sendiri ini
// (belum ada I2S yang bentrok). Kalau native pin ini berhasil mount tapi pin
// custom di bawah gagal terus, itu tandanya masalahnya di pin custom
// (D34 butuh pull-up eksternal, atau GPIO-matrix kurang stabil) -- BUKAN
// modul/kartu/wiring rusak. Kalau native pin JUGA gagal, berarti bukan soal
// pin sama sekali, curigai modul/kartu/kontak fisik.
#define USE_NATIVE_PINS  0

#if USE_NATIVE_PINS
  #define SD_SCK   18
  #define SD_MISO  19
  #define SD_MOSI  23
  #define SD_CS    5
  SPIClass sdSPI(VSPI);
#else
  // === PIN SD CUSTOM (sama seperti sd_recorder.ino di esp32.txt.ino) ===
  #define SD_SCK   14
  #define SD_MISO  34
  #define SD_MOSI  27
  #define SD_CS    13
  SPIClass sdSPI(HSPI);
#endif

// Mulai dari 4MHz (aman untuk GPIO-matrix-routed, bukan native pin) --
// jangan dinaikkan sebelum baseline ini terbukti stabil lama.
#define SD_FREQ_HZ  4000000

#define LED_PIN  2

#define TEST_FILE     "/sdtest.bin"
#define TEST_SIZE     4096   // byte per siklus tes -- cukup buat nge-stress throughput tanpa lama-lama nulis
bool sdReady = false;

uint32_t cycleCount = 0;
uint32_t failCount  = 0;
unsigned long lastCycle = 0;
uint8_t seedByte = 0;

// ─────────────────────────────────────────────

void printCardInfo() {
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] Tidak ada kartu terdeteksi!");
    return;
  }
  const char* typeStr =
    cardType == CARD_MMC  ? "MMC"  :
    cardType == CARD_SD   ? "SDSC" :
    cardType == CARD_SDHC ? "SDHC" : "UNKNOWN";
  uint64_t sizeMB = SD.cardSize() / (1024 * 1024);
  Serial.printf("[SD] Tipe: %s | Ukuran: %llu MB\n", typeStr, sizeMB);
}

// Isi buffer dengan pola pseudo-random yang gampang diverifikasi ulang
// (LCG sederhana, seed beda tiap siklus supaya nggak ketipu cache/file lama).
void fillTestBuffer(uint8_t *buf, size_t len, uint8_t seed) {
  uint8_t v = seed;
  for (size_t i = 0; i < len; i++) {
    v = v * 197 + 3;   // LCG 8-bit sederhana, deterministik dari seed
    buf[i] = v;
  }
}

bool runWriteReadCycle() {
  static uint8_t writeBuf[TEST_SIZE];
  static uint8_t readBuf[TEST_SIZE];

  seedByte++;
  fillTestBuffer(writeBuf, TEST_SIZE, seedByte);

  SD.remove(TEST_FILE);
  unsigned long tWriteStart = millis();
  File fw = SD.open(TEST_FILE, FILE_WRITE);
  if (!fw) {
    Serial.println("[FAIL] Gagal buka file buat ditulis.");
    return false;
  }
  size_t written = fw.write(writeBuf, TEST_SIZE);
  fw.close();
  unsigned long writeMs = millis() - tWriteStart;

  if (written != TEST_SIZE) {
    Serial.printf("[FAIL] Cuma nulis %u/%u byte.\n", (unsigned)written, TEST_SIZE);
    return false;
  }

  unsigned long tReadStart = millis();
  File fr = SD.open(TEST_FILE, FILE_READ);
  if (!fr) {
    Serial.println("[FAIL] Gagal buka file buat dibaca balik.");
    return false;
  }
  size_t readN = fr.read(readBuf, TEST_SIZE);
  fr.close();
  unsigned long readMs = millis() - tReadStart;

  if (readN != TEST_SIZE) {
    Serial.printf("[FAIL] Cuma baca %u/%u byte.\n", (unsigned)readN, TEST_SIZE);
    return false;
  }

  int mismatchAt = -1;
  int mismatchCount = 0;
  for (size_t i = 0; i < TEST_SIZE; i++) {
    if (readBuf[i] != writeBuf[i]) {
      if (mismatchAt < 0) mismatchAt = (int)i;
      mismatchCount++;
    }
  }

  if (mismatchCount > 0) {
    Serial.printf("[FAIL] Verifikasi gagal -- %d byte beda (pertama di offset %d, tulis=0x%02X baca=0x%02X)\n",
                  mismatchCount, mismatchAt, writeBuf[mismatchAt], readBuf[mismatchAt]);
    return false;
  }

  float writeKBps = (TEST_SIZE / 1024.0) / (writeMs / 1000.0);
  float readKBps  = (TEST_SIZE / 1024.0) / (readMs  / 1000.0);
  Serial.printf("[OK] Siklus #%u -- tulis %lums (%.1f KB/s), baca %lums (%.1f KB/s)\n",
                cycleCount, writeMs, writeKBps, readMs, readKBps);
  return true;
}

void setup() {
  Serial.begin(921600);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println("=== TES SD CARD SENDIRIAN (Stage 1 bring-up) ===");
#if USE_NATIVE_PINS
  Serial.println("[CFG] Pakai pin NATIVE VSPI (SCK18/MISO19/MOSI23/CS5) -- mode diagnostik.");
#else
  Serial.println("[CFG] Pakai pin CUSTOM (SCK14/MISO34/MOSI27/CS13).");
#endif

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, sdSPI, SD_FREQ_HZ);

  if (!sdReady) {
    Serial.println("[SD] GAGAL mount! Cek wiring (SCK14/MISO34/MOSI27/CS13),");
    Serial.println("     dan pastikan kartu sudah diformat FAT32.");
  } else {
    Serial.println("[SD] Mount OK.");
    printCardInfo();
  }
}

void loop() {
  if (!sdReady) {
    // Coba mount ulang tiap beberapa detik kalau gagal di awal (misal kartu
    // baru dipasang setelah upload)
    delay(3000);
    Serial.println("[SD] Coba mount ulang...");
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdReady = SD.begin(SD_CS, sdSPI, SD_FREQ_HZ);
    if (sdReady) {
      Serial.println("[SD] Mount OK.");
      printCardInfo();
    }
    return;
  }

  if (millis() - lastCycle >= 3000) {
    lastCycle = millis();
    cycleCount++;
    bool ok = runWriteReadCycle();
    if (!ok) {
      failCount++;
      digitalWrite(LED_PIN, HIGH);   // latch nyala -- pernah ada kegagalan
      Serial.printf("    (total gagal sejauh ini: %u dari %u siklus)\n", failCount, cycleCount);
    } else if (failCount == 0) {
      // Belum pernah gagal -- kasih kedip pendek biar kelihatan masih hidup
      digitalWrite(LED_PIN, HIGH); delay(30); digitalWrite(LED_PIN, LOW);
    }
  }
}
