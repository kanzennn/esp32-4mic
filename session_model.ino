/*
 * session_model.ino — model data Responden/Huruf/Variasi/Take, di atas
 * primitif WAV/SD level-rendah di sd_recorder.ino (writeWavHeader,
 * writeFrameToSD, sessionFile[]/sessionSampleCount/lastHeaderPatchMs, flush
 * discipline -- semua itu TIDAK disentuh di sini, sengaja, karena sudah
 * lewat debugging keras buat benar).
 *
 * Struktur di SD card: nested per level --
 *   /RSPDN{r}/HRF{h}/VRS{v}/RSPDN{r}_HRF{h}_VRS{v}_{take}_MIC{m}.wav
 * Folder HRF{h} dan VRS{v} SENGAJA baru dibuat (mkdir) pas take pertama
 * kali direkam buat kombinasi itu (lihat ensureComboFolder(), dipanggil
 * dari startTakeForCurrentTarget()) -- bukan pas huruf/variasi cuma
 * "dipilih" di UI -- supaya folder kosong nggak pernah kebuat kalau
 * belum ada audio sama sekali.
 *
 * Nggak ada file counter per kombinasi (bakal kebanyakan file kecil) --
 * "huruf/variasi mana yang sudah ada" cukup dicek lewat SD.exists() ke
 * foldernya langsung (karena sekarang ada 1 folder per huruf/variasi),
 * dan "take mana yang sudah ada" tetap discan dari nama file di dalam
 * folder VRS{v}-nya.
 *
 * File ini ke-compile alfabetis SETELAH sd_recorder.ino tapi SEBELUM
 * web_ui.ino -- jadi web_ui.ino bisa manggil fungsi-fungsi di sini tanpa
 * forward declaration (pola yang sama seperti sd_recorder.ino's fungsi
 * yang langsung dipakai web_ui.ino). esp32.txt.ino (selalu ke-compile
 * PALING AWAL) yang perlu forward declaration manual buat manggil ke sini.
 *
 * MAX_VARIASI_TRACK/MAX_TAKE_TRACK/struct TakeInfo didefinisikan di
 * esp32.txt.ino (BUKAN di sini) -- fungsi yang makai keduanya di parameter
 * (findExistingVariasi, listTakes) proto-nya di-hoist ctags ke titik yang
 * lebih awal dari definisi di file ini, jadi definisinya harus ada di file
 * yang ke-compile paling awal. Lihat komentar di esp32.txt.ino.
 */

#include <SD.h>
#include <FS.h>

// ─────────────────────────────────────────────
// Helper nama path -- dipakai berulang di bawah, satu tempat biar konsisten.

String comboPrefix(int responden, int huruf, int variasi) {
  return "RSPDN" + String(responden) + "_HRF" + String(huruf) + "_VRS" + String(variasi) + "_";
}

// Folder tempat take-take kombinasi ini disimpan: /RSPDN{r}/HRF{h}/VRS{v}
String comboFolder(int responden, int huruf, int variasi) {
  return "/RSPDN" + String(responden) + "/HRF" + String(huruf) + "/VRS" + String(variasi);
}

String takeBasePath(int responden, int huruf, int variasi, uint32_t take) {
  return comboFolder(responden, huruf, variasi) + "/" + comboPrefix(responden, huruf, variasi) + String(take);
}

// Pastikan /RSPDN{r}, /RSPDN{r}/HRF{h}, /RSPDN{r}/HRF{h}/VRS{v} ada --
// dibuat satu level per satu level (SD.mkdir() TIDAK otomatis buat parent
// yang belum ada, beda dari "mkdir -p"). Dipanggil cuma pas mau mulai take
// beneran, bukan pas huruf/variasi cuma dipilih di UI.
bool ensureComboFolder(int responden, int huruf, int variasi) {
  String rspFolder = "/RSPDN" + String(responden);
  String hrfFolder = rspFolder + "/HRF" + String(huruf);
  String vrsFolder = hrfFolder + "/VRS" + String(variasi);
  if (!SD.exists(rspFolder) && !SD.mkdir(rspFolder)) return false;
  if (!SD.exists(hrfFolder) && !SD.mkdir(hrfFolder)) return false;
  if (!SD.exists(vrsFolder) && !SD.mkdir(vrsFolder)) return false;
  return true;
}

// ─────────────────────────────────────────────

int findNextRespondentIndex() {
  int maxIdx = 0;
  File root = SD.open("/");
  if (!root) return 1;

  File entry;
  while ((entry = root.openNextFile())) {
    bool isDir = entry.isDirectory();
    String name = String(entry.name());
    entry.close();
    if (!isDir) continue;

    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.startsWith("RSPDN")) continue;

    int idx = name.substring(5).toInt();
    if (idx > maxIdx) maxIdx = idx;
  }
  root.close();
  return maxIdx + 1;
}

bool createNewRespondent(int *outIdx) {
  int idx = findNextRespondentIndex();
  String folder = "/RSPDN" + String(idx);
  if (!SD.mkdir(folder)) return false;
  if (outIdx) *outIdx = idx;
  return true;
}

// ─────────────────────────────────────────────
// Huruf mana (1..28) yang punya minimal 1 take buat responden ini -- sejak
// ada folder per-huruf, ini cukup cek keberadaan foldernya langsung
// (SD.exists), nggak perlu scan/parsing nama file lagi.
void findHurufWithData(int responden, bool hasData[29]) {
  for (int i = 0; i <= 28; i++) hasData[i] = false;
  for (int h = 1; h <= 28; h++) {
    String folder = "/RSPDN" + String(responden) + "/HRF" + String(h);
    hasData[h] = SD.exists(folder);
  }
}

// ─────────────────────────────────────────────
// Nomor variasi (1..MAX_VARIASI_TRACK) mana yang sudah punya folder, buat
// pasangan (responden,huruf) tertentu -- scan subfolder VRS{v} di dalam
// /RSPDN{r}/HRF{h}/. found[1] selalu jadi opsi yang ditawarkan meski belum
// ada rekaman sama sekali (aturan "default Variasi 1" -- itu ditangani di
// endpoint /variasi web_ui.ino, bukan di sini, fungsi ini cuma laporan apa
// adanya dari hasil scan).
void findExistingVariasi(int responden, int huruf, bool found[MAX_VARIASI_TRACK + 1], int *maxFound) {
  for (int i = 0; i <= MAX_VARIASI_TRACK; i++) found[i] = false;
  *maxFound = 0;

  String folder = "/RSPDN" + String(responden) + "/HRF" + String(huruf);
  File dir = SD.open(folder);
  if (!dir) return;

  File entry;
  while ((entry = dir.openNextFile())) {
    bool isDir = entry.isDirectory();
    String name = String(entry.name());
    entry.close();
    if (!isDir) continue;

    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.startsWith("VRS")) continue;

    int variasi = name.substring(3).toInt();
    if (variasi >= 1 && variasi <= MAX_VARIASI_TRACK) {
      found[variasi] = true;
      if (variasi > *maxFound) *maxFound = variasi;
    }
  }
  dir.close();
}

// ─────────────────────────────────────────────
// Nomor take berikutnya buat kombinasi (responden,huruf,variasi) --
// max(nomor take yang sudah ada)+1, dilihat dari file *_MIC1.wav di dalam
// folder VRS{v}-nya. Selalu di-scan ulang (bukan dari cache) tiap kali
// dipanggil dari startTakeForCurrentTarget() -- itu yang bikin nggak
// mungkin ada tabrakan nama file meski preview di /status sempat basi.
uint32_t findNextTakeNumber(int responden, int huruf, int variasi) {
  uint32_t maxTake = 0;
  String folder = comboFolder(responden, huruf, variasi);
  File dir = SD.open(folder);
  if (!dir) return 1;  // folder belum ada = belum ada take sama sekali

  String prefix = comboPrefix(responden, huruf, variasi);
  String suffix = "_MIC1.wav";

  File entry;
  while ((entry = dir.openNextFile())) {
    bool isDir = entry.isDirectory();
    String name = String(entry.name());
    entry.close();
    if (isDir) continue;

    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.startsWith(prefix) || !name.endsWith(suffix)) continue;

    String takeStr = name.substring(prefix.length(), name.length() - suffix.length());
    uint32_t take = (uint32_t)takeStr.toInt();
    if (take > maxTake) maxTake = take;
  }
  dir.close();
  return maxTake + 1;
}

// ─────────────────────────────────────────────
// Pilih target rekaman saat ini (dipanggil dari POST /select). Divalidasi:
// folder responden harus sudah ada, huruf 1..28, variasi>=1. Folder
// HRF{h}/VRS{v} BELUM tentu ada di titik ini (baru dibuat pas take pertama
// direkam) -- makanya validasinya cuma cek folder respondennya, bukan
// keseluruhan path kombinasi. targetSelected dan currentRespondenIdx/
// HurufIdx/VariasiIdx didefinisikan di esp32.txt.ino (file pertama, jadi
// terlihat di sini tanpa extern).
bool selectTarget(int responden, int huruf, int variasi, uint32_t *outNextTake) {
  String folder = "/RSPDN" + String(responden);
  if (!SD.exists(folder)) return false;
  if (huruf < 1 || huruf > 28) return false;
  if (variasi < 1) return false;

  currentRespondenIdx = responden;
  currentHurufIdx     = huruf;
  currentVariasiIdx   = variasi;
  targetSelected      = true;
  cachedNextTake       = findNextTakeNumber(responden, huruf, variasi);

  if (outNextTake) *outNextTake = cachedNextTake;
  return true;
}

// ─────────────────────────────────────────────
// Mulai take baru buat target yang sedang dipilih. Dipanggil dengan
// sdMutex sudah dipegang caller (sama seperti openSessionFiles() dulu).
bool startTakeForCurrentTarget() {
  if (!sdReady || !targetSelected) return false;
  if (!ensureComboFolder(currentRespondenIdx, currentHurufIdx, currentVariasiIdx)) return false;

  uint32_t take = findNextTakeNumber(currentRespondenIdx, currentHurufIdx, currentVariasiIdx);
  String base = takeBasePath(currentRespondenIdx, currentHurufIdx, currentVariasiIdx, take);
  currentSessionPath = base;
  currentTakeNumber  = take;

  for (int n = 0; n < 4; n++) {
    String path = base + "_MIC" + String(n + 1) + ".wav";
    sessionFile[n] = SD.open(path, FILE_WRITE);
    if (!sessionFile[n]) return false;
    // Buffer stdio default per file ~4KB (st_blksize FAT) x 4 file = ~19KB
    // blok kontigu hilang dari RAM internal -> largestFreeBlock jatuh ke ~1KB
    // selama rekaman, lebih kecil dari buffer RX per koneksi WebServer (~1.4KB),
    // jadi request dari app gagal dan WiFi kelihatan "mati". Satu tulisan per
    // loop cuma 512B per channel, jadi buffer 512B tidak mengurangi throughput.
    // Wajib dipanggil tepat setelah open, sebelum I/O apapun.
    sessionFile[n].setBufferSize(512);
    writeWavHeader(sessionFile[n], 0);
    Serial.printf("[HEAP-OPEN] mic%d free=%u largestBlock=%u\n", n + 1,
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }

  sessionSampleCount = 0;
  lastHeaderPatchMs  = millis();
  return true;
}

// Tutup take yang sedang jalan. currentRespondenIdx/HurufIdx/VariasiIdx dan
// targetSelected SENGAJA tidak disentuh -- itu yang bikin app bisa panggil
// /start lagi untuk "ulangi target terakhir" tanpa perlu /select ulang.
void closeCurrentTake() {
  if (!sdReady) return;

  uint32_t dataBytes = sessionSampleCount * 2;
  for (int n = 0; n < 4; n++) {
    if (!sessionFile[n]) continue;
    writeWavHeader(sessionFile[n], dataBytes);
    sessionFile[n].close();
  }

  File doneFile = SD.open(currentSessionPath + ".done", FILE_WRITE);
  if (doneFile) doneFile.close();

  cachedNextTake      = currentTakeNumber + 1;
  currentSessionPath  = "";
  currentTakeNumber   = 0;
}

// ─────────────────────────────────────────────
// Dipanggil sekali di setup() dan lagi tiap kali sdPresenceCheckTask()
// berhasil remount kartu yang baru dipasang. Jalan-jalan 3 level
// (RSPDN -> HRF -> VRS), cari take yang belum ada penanda ".done"
// (listrik/reset putus di tengah take itu), hitung ulang ukuran WAV dari
// ukuran file asli, patch headernya biar tetap kebaca. Beda dari skema
// lama (repairIncompleteSessionsAtBoot, per-folder rename jadi
// "_incomplete"): di sini file WAV-nya TIDAK di-rename sama sekali (nama
// tetap stabil buat script downstream) dan ".done" SENGAJA tidak ditulis
// di sini -- take yang kena repair tetap tertandai "belum selesai"
// selamanya di browse UI, karena header yang direkonstruksi dari ukuran
// file bukan jaminan sama persis kayak stop yang bersih.
void repairIncompleteTakesAtBoot() {
  if (!sdReady) return;

  File root = SD.open("/");
  if (!root) return;

  File rspEntry;
  while ((rspEntry = root.openNextFile())) {
    bool rspIsDir = rspEntry.isDirectory();
    String rspName = String(rspEntry.name());
    rspEntry.close();
    if (!rspIsDir) continue;
    int s1 = rspName.lastIndexOf('/');
    if (s1 >= 0) rspName = rspName.substring(s1 + 1);
    if (!rspName.startsWith("RSPDN")) continue;
    String rspPath = "/" + rspName;

    File hrfDir = SD.open(rspPath);
    if (!hrfDir) continue;

    File hrfEntry;
    while ((hrfEntry = hrfDir.openNextFile())) {
      bool hrfIsDir = hrfEntry.isDirectory();
      String hrfName = String(hrfEntry.name());
      hrfEntry.close();
      if (!hrfIsDir) continue;
      int s2 = hrfName.lastIndexOf('/');
      if (s2 >= 0) hrfName = hrfName.substring(s2 + 1);
      if (!hrfName.startsWith("HRF")) continue;
      String hrfPath = rspPath + "/" + hrfName;

      File vrsDir = SD.open(hrfPath);
      if (!vrsDir) continue;

      File vrsEntry;
      while ((vrsEntry = vrsDir.openNextFile())) {
        bool vrsIsDir = vrsEntry.isDirectory();
        String vrsName = String(vrsEntry.name());
        vrsEntry.close();
        if (!vrsIsDir) continue;
        int s3 = vrsName.lastIndexOf('/');
        if (s3 >= 0) vrsName = vrsName.substring(s3 + 1);
        if (!vrsName.startsWith("VRS")) continue;
        String vrsPath = hrfPath + "/" + vrsName;

        File fileDir = SD.open(vrsPath);
        if (!fileDir) continue;

        File fEntry;
        while ((fEntry = fileDir.openNextFile())) {
          bool fIsDir = fEntry.isDirectory();
          String fName = String(fEntry.name());
          fEntry.close();
          if (fIsDir) continue;
          int s4 = fName.lastIndexOf('/');
          if (s4 >= 0) fName = fName.substring(s4 + 1);
          if (!fName.endsWith("_MIC1.wav")) continue;

          String base = fName.substring(0, fName.length() - String("_MIC1.wav").length());
          String basePath = vrsPath + "/" + base;
          if (SD.exists(basePath + ".done")) continue;  // take ini sudah ditutup bersih

          for (int n = 1; n <= 4; n++) {
            String wavPath = basePath + "_MIC" + String(n) + ".wav";
            if (!SD.exists(wavPath)) continue;
            // "r+" -- BUKAN FILE_WRITE ("w"), yang truncate ke 0 byte saat dibuka.
            File f = SD.open(wavPath, "r+");
            if (!f) continue;
            uint32_t total = f.size();
            uint32_t dataBytes = (total > 44) ? (total - 44) : 0;
            writeWavHeader(f, dataBytes);
            f.close();
          }
        }
        fileDir.close();
      }
      vrsDir.close();
    }
    hrfDir.close();
  }
  root.close();
}

// ─────────────────────────────────────────────
// Daftar take buat kombinasi (responden,huruf,variasi) tertentu -- dipakai
// GET /takes. outArr harus muat minimal maxOut elemen (caller di
// web_ui.ino biasanya pakai MAX_TAKE_TRACK).
int listTakes(int responden, int huruf, int variasi, TakeInfo *outArr, int maxOut) {
  int count = 0;
  String folder = comboFolder(responden, huruf, variasi);
  File dir = SD.open(folder);
  if (!dir) return 0;  // folder belum ada = belum ada take

  String prefix = comboPrefix(responden, huruf, variasi);
  String suffix = "_MIC1.wav";

  File entry;
  while ((entry = dir.openNextFile())) {
    bool isDir = entry.isDirectory();
    String name = String(entry.name());
    entry.close();
    if (isDir) continue;

    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.startsWith(prefix) || !name.endsWith(suffix)) continue;
    if (count >= maxOut) continue;  // terus scan (buat konsistensi), tapi berhenti nyimpen

    String takeStr = name.substring(prefix.length(), name.length() - suffix.length());
    uint32_t take = (uint32_t)takeStr.toInt();
    String basePath = folder + "/" + prefix + takeStr;

    Serial.printf("[TAKES] take %u free=%u largest=%u\n", (unsigned)take, ESP.getFreeHeap(),
                  (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    outArr[count].take = take;
    outArr[count].done = SD.exists(basePath + ".done");
    for (int n = 0; n < 4; n++) {
      String wavPath = basePath + "_MIC" + String(n + 1) + ".wav";
      File wf = SD.open(wavPath, FILE_READ);
      outArr[count].bytes[n] = wf ? wf.size() : 0;
      if (wf) wf.close();
    }
    count++;
  }
  dir.close();
  return count;
}
