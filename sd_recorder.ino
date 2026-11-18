/*
 * sd_recorder.ino — Tulis 4 file WAV (mic1-4) langsung ke SD card per sesi
 * rekam, pengganti jalur lama "stream lewat Serial ke recorder.py".
 *
 * Pin SPI SD CUSTOM (bukan default VSPI/HSPI) — VSPI default (18,19,23,5)
 * bentrok dengan I2S Bus 1 (SCK=18,WS=5,SD=19). GPIO12 (HSPI MISO default)
 * juga dihindari karena strapping pin (nentuin tegangan flash saat boot;
 * pull-up MISO dari modul SD bisa bikin ESP32 gagal boot). MISO dipindah
 * ke GPIO34 (input-only, aman karena MISO memang cuma dibaca ESP32).
 *
 * Karena bukan pin native HSPI/VSPI, bus di-routing lewat GPIO matrix —
 * clock SPI dibatasi lebih rendah dari mode native. Mulai pelan (4MHz),
 * baru naikkan kalau sudah terbukti stabil (lihat CLAUDE.md tahap bring-up).
 */

#include <SPI.h>
#include <SD.h>
#include <FS.h>

#define SD_SCK  14
#define SD_MISO 34   // input-only, tidak ada pull-up internal — tambah pull-up eksternal 10k ke 3V3 kalau bacaan flaky
#define SD_MOSI 27
#define SD_CS   13

SPIClass sdSPI(HSPI);

File sessionFile[4];              // mic1..mic4, index 0..3
uint32_t sessionSampleCount = 0;  // jumlah sample PER CHANNEL di sesi aktif
unsigned long lastHeaderPatchMs = 0;
bool sdReady = false;

// ─────────────────────────────────────────────

// File penanda kecil, dibuat sekali seumur kartu -- satu-satunya tujuannya
// adalah kasih checkSDPresent() sesuatu yang PASTI ada buat dibaca. Dulu
// checkSDPresent() pakai /next_id.txt untuk ini, tapi itu cuma lazy-created
// pas rekaman pertama kali (jadi di kartu baru yang belum pernah dipakai
// rekam, checkSDPresent() bakal gagal terus tiap detik dan trigger
// remount+repair berulang). Dibuat langsung setelah mount berhasil supaya
// selalu ada sejak awal.
void ensureSDMarker() {
  if (sdReady && !SD.exists("/sd_marker.txt")) {
    File f = SD.open("/sd_marker.txt", FILE_WRITE);
    if (f) { f.write((uint8_t)1); f.close(); }
  }
}

bool initSDCard() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  // max_files dinaikkan dari default (5) ke 10 -- struktur folder nested
  // (RSPDN/HRF/VRS) butuh beberapa direktori kebuka bersamaan pas
  // repairIncompleteTakesAtBoot() jalan-jalan 3 level, plus file WAV yang
  // lagi direkam/diperbaiki -- 5 terlalu mepet buat itu semua sekaligus.
  sdReady = SD.begin(SD_CS, sdSPI, 4000000, "/sd", 10);
  ensureSDMarker();
  return sdReady;
}

// ─────────────────────────────────────────────
// Coba mount ulang setelah kartu diyakini dicabut. SD.begin() SENDIRIAN
// TIDAK CUKUP di sini -- SDFS::begin() early-return true kalau internal
// pdrv-nya masih ke-set dari mount sebelumnya, tanpa re-probe hardware sama
// sekali. Jadi kartu baru yang dipasang nggak akan pernah kedeteksi kecuali
// SD.end() dipanggil dulu buat reset state itu.
bool remountSDCard() {
  SD.end();
  // max_files dinaikkan dari default (5) ke 10 -- struktur folder nested
  // (RSPDN/HRF/VRS) butuh beberapa direktori kebuka bersamaan pas
  // repairIncompleteTakesAtBoot() jalan-jalan 3 level, plus file WAV yang
  // lagi direkam/diperbaiki -- 5 terlalu mepet buat itu semua sekaligus.
  sdReady = SD.begin(SD_CS, sdSPI, 4000000, "/sd", 10);
  ensureSDMarker();
  return sdReady;
}

// ─────────────────────────────────────────────
// Cek keberadaan kartu SD secara REAL-TIME. SD.cardType()/cardSize() TIDAK
// bisa dipakai untuk ini -- keduanya cuma baca field yang di-cache sejak
// SD.begin() pertama kali, jadi tetap "kelihatan" ada walau kartu sudah
// dicabut belakangan. Satu-satunya cara pasti adalah paksa 1 transaksi SPI
// fisik ke kartu: baca 1 byte sungguhan dari /sd_marker.txt (file yang sudah
// pasti ada, lihat ensureSDMarker()). Kalau kartu dicabut, transaksi ini
// gagal/timeout (dibatasi waktu oleh driver, bukan hang selamanya) dan
// File::read() balikin 0.
bool checkSDPresent() {
  if (!sdReady) return false;
  File f = SD.open("/sd_marker.txt", FILE_READ);
  if (!f) return false;
  uint8_t b;
  int n = f.read(&b, 1);
  f.close();
  return n == 1;
}

// ─────────────────────────────────────────────
// Header WAV kanonik 44-byte, mono/16-bit/8000Hz. Ditulis placeholder saat
// file dibuka (dataBytes=0), dipatch ulang berkala selama rekam, dan
// dipatch final saat stop — supaya kalau listrik putus di tengah rekam,
// paling banter cuma kehilangan ~2 detik akurasi ukuran, bukan seluruh file.
void writeWavHeader(File &f, uint32_t dataBytes) {
  uint32_t chunkSize     = 36 + dataBytes;
  uint32_t sampleRate    = SAMPLE_RATE;
  uint32_t byteRate      = SAMPLE_RATE * 2;  // mono * 16-bit
  uint32_t subchunk1Size = 16;
  uint16_t audioFormat   = 1;                // PCM
  uint16_t numChannels   = 1;
  uint16_t blockAlign    = 2;
  uint16_t bitsPerSample = 16;

  f.seek(0);
  f.write((const uint8_t*)"RIFF", 4);
  f.write((uint8_t*)&chunkSize, 4);
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  f.write((uint8_t*)&subchunk1Size, 4);
  f.write((uint8_t*)&audioFormat, 2);
  f.write((uint8_t*)&numChannels, 2);
  f.write((uint8_t*)&sampleRate, 4);
  f.write((uint8_t*)&byteRate, 4);
  f.write((uint8_t*)&blockAlign, 2);
  f.write((uint8_t*)&bitsPerSample, 2);
  f.write((const uint8_t*)"data", 4);
  f.write((uint8_t*)&dataBytes, 4);
}

// ─────────────────────────────────────────────
// Dipanggil tiap iterasi loop() saat isRecording — tulis batch sample
// per-channel, lalu patch header berkala (~2 detik sekali).
void writeFrameToSD(int16_t *m1, int16_t *m2, int16_t *m3, int16_t *m4, int count) {
  if (!sdReady || count <= 0) return;

  sessionFile[0].write((uint8_t*)m1, count * 2);
  sessionFile[1].write((uint8_t*)m2, count * 2);
  sessionFile[2].write((uint8_t*)m3, count * 2);
  sessionFile[3].write((uint8_t*)m4, count * 2);
  sessionSampleCount += count;

  if (millis() - lastHeaderPatchMs >= 2000) {
    lastHeaderPatchMs = millis();
    uint32_t dataBytes = sessionSampleCount * 2;
    for (int n = 0; n < 4; n++) {
      uint32_t pos = sessionFile[n].position();
      writeWavHeader(sessionFile[n], dataBytes);
      sessionFile[n].seek(pos);
      // WAJIB flush() -- write() cuma masuk buffer stdio internal, dan FAT
      // TIDAK nyimpen ukuran file yang akurat ke directory entry di disk
      // sampai di-sync. Tanpa ini, kalau listrik diputus, baik data audio
      // yang belum ke-flush MAUPUN metadata ukuran file di FAT sama-sama
      // hilang -- itu penyebab file cuma tersisa ~1KB meski sudah rekam
      // 8 detik. flush() (fflush+fsync di ESP32 core) paksa keduanya
      // benar-benar nyampe fisik kartu SD tiap ~2 detik.
      sessionFile[n].flush();
    }
  }
}

// closeSessionFiles()/repairIncompleteSessionsAtBoot() (sesi /rec_XXXX lama)
// sudah digantikan closeCurrentTake()/repairIncompleteTakesAtBoot() di
// session_model.ino, yang paham struktur responden/huruf/variasi/take.
