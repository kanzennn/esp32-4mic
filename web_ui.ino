/*
 * web_ui.ino — Web server WiFi (jalan di Core 0, terpisah dari loop() I2S
 * yang timing-nya sensitif di Core 1) buat pilih target rekam
 * (Responden/Huruf/Variasi), kontrol start/stop, dan file manager dari
 * browser HP. Pakai WebServer.h bawaan ESP32 core (bukan ESPAsyncWebServer)
 * supaya nggak nambah dependency library eksternal.
 *
 * Halaman punya 2 tab: "Rekam Baru" (pilih/buat target lewat /select, lalu
 * /start-/stop) dan "Lihat Data" (browse/download/hapus take yang sudah
 * ada -- read-only, TIDAK PERNAH panggil /select, lihat catatan di
 * handleSelect() dan INDEX_HTML).
 *
 * Semua akses SD (dari sini maupun dari loop() di Core 1) WAJIB dijaga
 * sdMutex — FAT/VFS ESP32 nggak aman diakses barengan dari 2 core.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <FS.h>
#include "esp_heap_caps.h"

WebServer server(80);
TaskHandle_t webServerTaskHandle = NULL;

// ── Halaman HTML disimpan di flash (PROGMEM), bukan makan RAM ──
const char INDEX_HTML[] PROGMEM = R"HTMLDOC(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>4-Mic Recorder</title>
<style>
body{font-family:sans-serif;max-width:520px;margin:0 auto;padding:12px;background:#111;color:#eee}
button{padding:12px 20px;font-size:16px;border:none;border-radius:8px;margin:4px 0;width:100%}
#rec{background:#c0392b;color:#fff}
#rec:disabled{opacity:.4}
#stop{background:#555;color:#fff}
.tabbtn{width:48%;display:inline-block;background:#222;color:#999;border-radius:8px 8px 0 0}
.tabbtn.active{background:#333;color:#fff}
.cols{display:flex;gap:6px;margin:10px 0}
.col{flex:1;min-width:0}
.col h4{margin:0 0 4px 0;font-size:12px;color:#999}
.list{background:#1a1a1a;border-radius:6px;max-height:220px;overflow-y:auto;font-size:13px}
.item{padding:7px 6px;border-bottom:1px solid #262626;cursor:pointer}
.item:last-child{border-bottom:none}
.item.sel{background:#2d5c8a;color:#fff}
.item.hasdata{color:#7fd17f}
.item.addnew{color:#4da3ff;font-weight:bold}
#target-preview{background:#222;border-radius:8px;padding:10px;margin:8px 0;min-height:1em}
.session{background:#222;border-radius:8px;padding:10px;margin:8px 0}
.session.incomplete{border:1px solid #c0392b}
a{color:#4da3ff;text-decoration:none}
small{color:#999}
#sd-status{font-size:13px;margin:4px 0;font-weight:bold}
#sd-status.sdok{color:#7fd17f}
#sd-status.sdbad{color:#c0392b}
</style></head><body>
<h2>4-Mic Recorder</h2>
<p id="sd-status">SD: memuat...</p>
<div>
  <button class="tabbtn active" id="tabbtn-rekam" onclick="switchTab('rekam')">Rekam Baru</button>
  <button class="tabbtn" id="tabbtn-lihat" onclick="switchTab('lihat')">Lihat Data</button>
</div>

<div id="view-rekam">
  <p id="status">memuat status...</p>
  <div class="cols">
    <div class="col"><h4>RESPONDEN</h4><div class="list" id="r-responden"></div></div>
    <div class="col"><h4>HURUF</h4><div class="list" id="r-huruf"></div></div>
    <div class="col"><h4>VARIASI</h4><div class="list" id="r-variasi"></div></div>
  </div>
  <div id="target-preview">Pilih responden, huruf, dan variasi dulu.</div>
  <button id="rec" onclick="startRec()" disabled>MULAI REKAM</button>
  <button id="stop" onclick="stopRec()">STOP</button>
</div>

<div id="view-lihat" style="display:none">
  <div class="cols">
    <div class="col"><h4>RESPONDEN</h4><div class="list" id="v-responden"></div></div>
    <div class="col"><h4>HURUF</h4><div class="list" id="v-huruf"></div></div>
    <div class="col"><h4>VARIASI</h4><div class="list" id="v-variasi"></div></div>
  </div>
  <h3>Take</h3>
  <div id="v-takes">Pilih responden/huruf/variasi dulu.</div>
</div>

<script>
// 28 huruf hijaiyah standar -- cuma buat tampilan, firmware cuma pakai indeksnya (1-28).
const HURUF = [
  [1,"Alif","ا"],[2,"Ba","ب"],[3,"Ta","ت"],[4,"Tsa","ث"],[5,"Jim","ج"],[6,"Ha","ح"],
  [7,"Kha","خ"],[8,"Dal","د"],[9,"Dzal","ذ"],[10,"Ra","ر"],[11,"Zay","ز"],[12,"Sin","س"],
  [13,"Syin","ش"],[14,"Shad","ص"],[15,"Dhad","ض"],[16,"Tha","ط"],[17,"Zha","ظ"],[18,"'Ain","ع"],
  [19,"Ghain","غ"],[20,"Fa","ف"],[21,"Qaf","ق"],[22,"Kaf","ك"],[23,"Lam","ل"],[24,"Mim","م"],
  [25,"Nun","ن"],[26,"Waw","و"],[27,"Ha","ه"],[28,"Ya","ي"]
];
// #6 dan #27 sama-sama transliterasi jadi "Ha" -- huruf Arab-nya yang disertakan
// di sebelahnya (ح vs ه) yang bikin nggak ketuker.
// CATATAN: semua "fungsi" di script ini SENGAJA ditulis gaya
// "const nama = (...) => {...}" (arrow function), BUKAN pakai kata kunci
// JS buat deklarasi fungsi biasa. Arduino auto-prototype-generator (ctags)
// nggak paham blok teks mentah C++ tempat halaman ini disimpan -- dia scan
// teksnya secara naif, dan pola "kata kata(...) {" (persis kayak deklarasi
// fungsi JS gaya lama) kebaca sebagai definisi fungsi C++ beneran, lalu
// di-generate prototype yang gagal compile. Gaya arrow function di atas
// nggak match pola itu sama sekali.
//
// PERINGATAN buat siapa pun yang edit blok <script> ini: JANGAN pernah
// mengetik urutan karakter tutup-kurung + delimiter + tutup-kutip milik
// wrapper string mentah C++ di file ini (lihat baris paling atas dan
// paling bawah file) di dalam komentar/teks mana pun di sini -- itu bakal
// memotong string-nya lebih awal dari yang seharusnya (persis bug yang baru
// saja terjadi gara-gara komentar penjelasan ini sendiri).
const hurufLabel = (n) => { const h = HURUF[n-1]; return h[1] + ' (' + h[2] + ')'; };

let recordingNow = false;

const switchTab = (tab) => {
  document.getElementById('view-rekam').style.display = (tab==='rekam') ? '' : 'none';
  document.getElementById('view-lihat').style.display = (tab==='lihat') ? '' : 'none';
  document.getElementById('tabbtn-rekam').className = 'tabbtn' + (tab==='rekam' ? ' active' : '');
  document.getElementById('tabbtn-lihat').className = 'tabbtn' + (tab==='lihat' ? ' active' : '');
  if (tab==='lihat') refreshLihatRespondents();
};

// ── Tab "Rekam Baru" ──
let selR = null, selH = null, selV = null;

const refreshStatus = async () => {
  const r = await fetch('/status'); const j = await r.json();
  recordingNow = j.recording;

  const sdEl = document.getElementById('sd-status');
  if (!j.sdReady) { sdEl.innerText = 'SD: Failed'; sdEl.className = 'sdbad'; }
  else if (!j.sdPresent) { sdEl.innerText = 'SD: Disconnected'; sdEl.className = 'sdbad'; }
  else { sdEl.innerText = 'SD: Connected'; sdEl.className = 'sdok'; }

  document.getElementById('status').innerText = j.recording
    ? ('REKAM: RESPONDEN ' + j.responden + ' / ' + hurufLabel(j.huruf) + ' / Variasi ' + j.variasi + ' / Take ' + j.currentTake + ' (' + j.elapsed_sec + 's)')
    : 'Idle';
  document.getElementById('rec').disabled = j.recording || !j.targetSelected || !j.sdPresent;
  // Stop SENGAJA nggak pernah di-disable berdasarkan status polling -- kalau
  // /status kebetulan lagi lambat/ke-block (lihat sdMutex di handleStatus()),
  // tombol ini nggak boleh ikut kekunci. Klik Stop pas nggak lagi rekam aman
  // (server cuma no-op), jadi nggak perlu pengunciannya.

  // Sinkronkan pilihan kolom kalau target berubah dari luar (mis. tombol
  // fisik dipencet langsung di alat, bukan dari HP ini).
  if (j.targetSelected && (selR!==j.responden || selH!==j.huruf || selV!==j.variasi)) {
    selR = j.responden; selH = j.huruf; selV = j.variasi;
    await loadRekamResponden();
    document.getElementById('target-preview').innerText =
      'Target: RESPONDEN ' + selR + ' / ' + hurufLabel(selH) + ' / Variasi ' + selV + ' / Take berikutnya ' + j.nextTake;
  }
};

const loadRekamResponden = async () => {
  const r = await fetch('/respondents'); const j = await r.json();
  const el = document.getElementById('r-responden');
  el.innerHTML = '';
  j.respondents.forEach(n => {
    const d = document.createElement('div');
    d.className = 'item' + (n===selR ? ' sel' : '');
    d.innerText = 'RESPONDEN ' + n;
    d.onclick = () => pickResponden(n);
    el.appendChild(d);
  });
  const add = document.createElement('div');
  add.className = 'item addnew';
  add.innerText = '+ ADD NEW';
  add.onclick = addResponden;
  el.appendChild(add);

  if (selR!==null) await loadRekamHuruf();
};

const addResponden = async () => {
  if (recordingNow) return;
  const r = await fetch('/respondents', {method:'POST'});
  if (!r.ok) { alert('Gagal buat responden: ' + await r.text()); return; }
  const j = await r.json();
  selR = j.created; selH = null; selV = null;
  await loadRekamResponden();
};

const pickResponden = async (n) => {
  if (recordingNow) return;
  selR = n; selH = null; selV = null;
  document.getElementById('r-variasi').innerHTML = '';
  document.getElementById('target-preview').innerText = 'Pilih huruf dan variasi.';
  document.getElementById('rec').disabled = true;
  await loadRekamResponden();
};

const loadRekamHuruf = async () => {
  const r = await fetch('/huruf?responden=' + selR); const j = await r.json();
  const has = new Set(j.withData);
  const el = document.getElementById('r-huruf');
  el.innerHTML = '';
  HURUF.forEach(h => {
    const n = h[0];
    const d = document.createElement('div');
    d.className = 'item' + (n===selH ? ' sel' : '') + (has.has(n) ? ' hasdata' : '');
    d.innerText = hurufLabel(n);
    d.onclick = () => pickHuruf(n);
    el.appendChild(d);
  });
  if (selH!==null) await loadRekamVariasi();
};

const pickHuruf = async (n) => {
  if (recordingNow) return;
  selH = n; selV = null;
  document.getElementById('target-preview').innerText = 'Pilih variasi.';
  document.getElementById('rec').disabled = true;
  await loadRekamHuruf();
  await loadRekamVariasi();
};

const loadRekamVariasi = async () => {
  const r = await fetch('/variasi?responden=' + selR + '&huruf=' + selH); const j = await r.json();
  const el = document.getElementById('r-variasi');
  el.innerHTML = '';
  j.variants.forEach(v => {
    const d = document.createElement('div');
    d.className = 'item' + (v===selV ? ' sel' : '');
    d.innerText = 'Variasi ' + v;
    d.onclick = () => pickVariasi(v);
    el.appendChild(d);
  });
  const add = document.createElement('div');
  add.className = 'item addnew';
  add.innerText = '+ ADD NEW (Variasi ' + j.nextNew + ')';
  add.onclick = () => pickVariasi(j.nextNew);
  el.appendChild(add);
};

const pickVariasi = async (v) => {
  if (recordingNow) return;
  selV = v;
  await loadRekamVariasi();
  const r = await fetch('/select?responden=' + selR + '&huruf=' + selH + '&variasi=' + selV, {method:'POST'});
  if (r.ok) {
    const j = await r.json();
    document.getElementById('target-preview').innerText =
      'Target: RESPONDEN ' + selR + ' / ' + hurufLabel(selH) + ' / Variasi ' + selV + ' / Take berikutnya ' + j.nextTake;
    document.getElementById('rec').disabled = false;
  } else {
    alert('Gagal pilih target: ' + await r.text());
  }
};

const startRec = async () => {
  const r = await fetch('/start', {method:'POST'});
  if (!r.ok) alert('Gagal mulai rekam: ' + await r.text());
  refreshStatus();
};
const stopRec = async () => {
  await fetch('/stop', {method:'POST'});
  refreshStatus();
  setTimeout(loadRekamVariasi, 500);  // refresh preview nomor take berikutnya
};

// ── Tab "Lihat Data" -- READ-ONLY, TIDAK PERNAH panggil /select di sini,
// biar browsing data nggak diam-diam ganti target yang bakal dipakai /start berikutnya.
let viewR = null, viewH = null, viewV = null;

const refreshLihatRespondents = async () => {
  const r = await fetch('/respondents'); const j = await r.json();
  const el = document.getElementById('v-responden');
  el.innerHTML = '';
  j.respondents.forEach(n => {
    const d = document.createElement('div');
    d.className = 'item' + (n===viewR ? ' sel' : '');
    d.innerText = 'RESPONDEN ' + n;
    d.onclick = () => { viewR = n; viewH = null; viewV = null; refreshLihatRespondents(); };
    el.appendChild(d);
  });
  document.getElementById('v-huruf').innerHTML = '';
  document.getElementById('v-variasi').innerHTML = '';
  document.getElementById('v-takes').innerHTML = 'Pilih responden/huruf/variasi dulu.';
  if (viewR!==null) await refreshLihatHuruf();
};

const refreshLihatHuruf = async () => {
  const r = await fetch('/huruf?responden=' + viewR); const j = await r.json();
  const el = document.getElementById('v-huruf');
  el.innerHTML = '';
  j.withData.forEach(n => {
    const d = document.createElement('div');
    d.className = 'item' + (n===viewH ? ' sel' : '');
    d.innerText = hurufLabel(n);
    d.onclick = () => { viewH = n; viewV = null; refreshLihatHuruf(); };
    el.appendChild(d);
  });
  if (j.withData.length===0) el.innerHTML = '<div class="item">(belum ada data)</div>';
  if (viewH!==null) await refreshLihatVariasi();
};

const refreshLihatVariasi = async () => {
  const r = await fetch('/variasi?responden=' + viewR + '&huruf=' + viewH); const j = await r.json();
  const el = document.getElementById('v-variasi');
  el.innerHTML = '';
  j.variants.forEach(v => {
    const d = document.createElement('div');
    d.className = 'item' + (v===viewV ? ' sel' : '');
    d.innerText = 'Variasi ' + v;
    d.onclick = () => { viewV = v; refreshLihatVariasi(); refreshTakes(); };
    el.appendChild(d);
  });
  if (viewV!==null) await refreshTakes();
};

const refreshTakes = async () => {
  const r = await fetch('/takes?responden=' + viewR + '&huruf=' + viewH + '&variasi=' + viewV);
  const j = await r.json();
  const el = document.getElementById('v-takes');
  el.innerHTML = '';
  if (j.takes.length===0) { el.innerText = '(belum ada take)'; return; }
  j.takes.forEach(t => {
    const base = '/RSPDN' + viewR + '/HRF' + viewH + '/VRS' + viewV + '/RSPDN' + viewR + '_HRF' + viewH + '_VRS' + viewV + '_' + t.take;
    const div = document.createElement('div');
    div.className = 'session' + (t.done ? '' : ' incomplete');
    const kb = t.bytes.map(b => (b/1024).toFixed(0)).join('/');
    div.innerHTML = '<b>Take ' + t.take + '</b>' + (t.done ? '' : ' (belum selesai)') +
      '<br><small>' + kb + ' KB per mic</small><br>' +
      [1,2,3,4].map(n => '<a href="/download?path=' + base + '_MIC' + n + '.wav">mic' + n + '</a>').join(' | ') +
      ' | <a href="#" onclick="delTake(\'' + base + '\')">hapus</a>';
    el.appendChild(div);
  });
};

const delTake = async (base) => {
  if (!confirm('Hapus take ini?')) return;
  const r = await fetch('/delete?scope=take&path=' + base, {method:'POST'});
  if (!r.ok) alert('Gagal hapus: ' + await r.text());
  refreshTakes();
};

loadRekamResponden();
refreshStatus();
setInterval(refreshStatus, 2000);
</script></body></html>
)HTMLDOC";

// ─────────────────────────────────────────────

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  // Sisa ruang SD di-cache, cuma dihitung ulang pas TIDAK lagi rekam.
  // SD.totalBytes()/usedBytes() dua-duanya panggil f_getfree() (2x akses SD
  // per satu request /status!) dan sama-sama butuh sdMutex -- endpoint ini
  // di-poll browser tiap 2 detik, PAS BARENGAN sama interval flush header
  // WAV pas rekam (writeFrameToSD() nahan sdMutex selama flush). Kalau
  // dihitung ulang tiap kali, /status (dan tombol Stop yang nunggu ini)
  // bisa kegantung/lambat kena rebutan mutex tepat pas paling butuh
  // responsif -- pas lagi rekam. Info sisa ruang nggak krusial di-refresh
  // tiap 2 detik pas rekam jalan, jadi aman dipakai nilai lama.
  static uint64_t cachedSdFree = 0;
  if (sdReady && !isRecording) {
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    cachedSdFree = SD.totalBytes() - SD.usedBytes();
    xSemaphoreGive(sdMutex);
  }
  uint64_t sdFree = cachedSdFree;

  String json = "{";
  json += "\"recording\":" + String(isRecording ? "true" : "false") + ",";
  json += "\"sdReady\":" + String(sdReady ? "true" : "false") + ",";
  json += "\"sdPresent\":" + String(sdPresentNow ? "true" : "false") + ",";
  json += "\"elapsed_sec\":" + String(isRecording ? (millis() - recStartTime) / 1000 : 0) + ",";
  json += "\"targetSelected\":" + String(targetSelected ? "true" : "false") + ",";
  json += "\"responden\":" + String(targetSelected ? currentRespondenIdx : -1) + ",";
  json += "\"huruf\":" + String(targetSelected ? currentHurufIdx : -1) + ",";
  json += "\"variasi\":" + String(targetSelected ? currentVariasiIdx : -1) + ",";
  json += "\"nextTake\":" + String(cachedNextTake) + ",";
  json += "\"currentTake\":" + String(currentTakeNumber) + ",";
  json += "\"currentFile\":\"" + currentSessionPath + "\",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"largestFreeBlock\":" + String((uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + ",";
  json += "\"sdFreeBytes\":" + String((uint32_t)sdFree);
  json += "}";
  server.send(200, "application/json", json);
}

// GET /respondents -- daftar folder /RSPDN{n} yang sudah ada.
void handleGetRespondents() {
  String json = "{\"respondents\":[";
  bool first = true;
  if (sdReady) {
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    File root = SD.open("/");
    if (root) {
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
        if (idx < 1) continue;
        if (!first) json += ",";
        first = false;
        json += String(idx);
      }
      root.close();
    }
    xSemaphoreGive(sdMutex);
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// POST /respondents -- buat responden baru berikutnya (RSPDN{n+1}), tanpa
// input teks sama sekali (lihat CLAUDE.md/plan: responden bukan free-text).
void handlePostRespondents() {
  if (!sdReady) { server.send(500, "text/plain", "SD belum siap"); return; }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  int idx = -1;
  bool ok = createNewRespondent(&idx);
  xSemaphoreGive(sdMutex);

  if (!ok) { server.send(500, "text/plain", "gagal buat folder responden"); return; }
  server.send(200, "application/json", "{\"created\":" + String(idx) + "}");
}

// GET /huruf?responden=N -- indeks huruf (1-28) yang sudah punya take.
void handleGetHuruf() {
  if (!server.hasArg("responden")) { server.send(400, "text/plain", "missing responden"); return; }
  int responden = server.arg("responden").toInt();

  bool hasData[29];
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  findHurufWithData(responden, hasData);
  xSemaphoreGive(sdMutex);

  String json = "{\"withData\":[";
  bool first = true;
  for (int h = 1; h <= 28; h++) {
    if (!hasData[h]) continue;
    if (!first) json += ",";
    first = false;
    json += String(h);
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// GET /variasi?responden=N&huruf=H -- nomor variasi yang sudah punya take
// (default [1] kalau belum ada sama sekali), plus nomor buat "ADD NEW".
void handleGetVariasi() {
  if (!server.hasArg("responden") || !server.hasArg("huruf")) {
    server.send(400, "text/plain", "missing responden/huruf");
    return;
  }
  int responden = server.arg("responden").toInt();
  int huruf = server.arg("huruf").toInt();

  bool found[MAX_VARIASI_TRACK + 1];
  int maxFound = 0;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  findExistingVariasi(responden, huruf, found, &maxFound);
  xSemaphoreGive(sdMutex);

  String json = "{\"variants\":[";
  if (maxFound == 0) {
    json += "1";  // belum ada rekaman -- tetap tawarkan Variasi 1 sebagai default
  } else {
    bool first = true;
    for (int v = 1; v <= maxFound; v++) {
      if (!found[v]) continue;
      if (!first) json += ",";
      first = false;
      json += String(v);
    }
  }
  json += "],\"nextNew\":" + String((maxFound < 1 ? 1 : maxFound) + 1) + "}";
  server.send(200, "application/json", json);
}

// POST /select?responden=N&huruf=H&variasi=V -- kunci target rekaman
// berikutnya. Dipakai tab "Rekam Baru" doang -- Lihat Data TIDAK BOLEH
// pernah manggil ini (browsing data nggak boleh diam-diam ganti target
// yang bakal dipakai /start berikutnya).
void handleSelect() {
  if (!server.hasArg("responden") || !server.hasArg("huruf") || !server.hasArg("variasi")) {
    server.send(400, "text/plain", "missing params");
    return;
  }
  int responden = server.arg("responden").toInt();
  int huruf = server.arg("huruf").toInt();
  int variasi = server.arg("variasi").toInt();

  if (huruf < 1 || huruf > 28) { server.send(400, "text/plain", "huruf harus 1-28"); return; }
  if (variasi < 1) { server.send(400, "text/plain", "variasi harus >=1"); return; }

  uint32_t nextTake = 1;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  bool ok = selectTarget(responden, huruf, variasi, &nextTake);
  xSemaphoreGive(sdMutex);

  if (!ok) { server.send(404, "text/plain", "responden tidak ditemukan"); return; }

  String json = "{\"ok\":true,\"responden\":" + String(responden) +
                ",\"huruf\":" + String(huruf) +
                ",\"variasi\":" + String(variasi) +
                ",\"nextTake\":" + String(nextTake) + "}";
  server.send(200, "application/json", json);
}

// GET /takes?responden=N&huruf=H&variasi=V -- daftar take + kelengkapan +
// ukuran per-mic, buat kolom ke-4 tab "Lihat Data". Streaming seperti
// /sessions dulu (bukan satu String besar) -- alasan sama, hindari
// fragmentasi heap kalau take menumpuk banyak.
void handleGetTakes() {
  if (!server.hasArg("responden") || !server.hasArg("huruf") || !server.hasArg("variasi")) {
    server.send(400, "text/plain", "missing params");
    return;
  }
  int responden = server.arg("responden").toInt();
  int huruf = server.arg("huruf").toInt();
  int variasi = server.arg("variasi").toInt();

  static TakeInfo takes[MAX_TAKE_TRACK];
  Serial.println("[TAKES] nunggu sdMutex");
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  Serial.println("[TAKES] sdMutex dipegang, listTakes...");
  int count = listTakes(responden, huruf, variasi, takes, MAX_TAKE_TRACK);
  xSemaphoreGive(sdMutex);
  Serial.printf("[TAKES] listTakes selesai count=%d free=%u largest=%u\n", count, ESP.getFreeHeap(),
                (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  Serial.println("[TAKES] header terkirim");
  server.sendContent("{\"takes\":[");
  for (int i = 0; i < count; i++) {
    if (i > 0) server.sendContent(",");
    String item = "{\"take\":" + String(takes[i].take) +
                  ",\"done\":" + String(takes[i].done ? "true" : "false") +
                  ",\"bytes\":[" + String(takes[i].bytes[0]) + "," + String(takes[i].bytes[1]) +
                  "," + String(takes[i].bytes[2]) + "," + String(takes[i].bytes[3]) + "]}";
    server.sendContent(item);
  }
  server.sendContent("]}");
  server.sendContent("");  // tutup chunked response
}

// Catatan: download menahan sdMutex selama transfer file berlangsung, jadi
// kalau ada download berjalan BARENGAN dengan sesi rekam aktif, penulisan
// SD di loop() Core 1 bakal nunggu giliran mutex — ini bisa memicu glitch
// audio (lihat tahap bring-up 6 di CLAUDE.md). Paling aman download setelah
// stop rekam, bukan sambil rekam jalan.
void handleDownload() {
  if (!server.hasArg("path")) { server.send(400, "text/plain", "missing path"); return; }
  String path = server.arg("path");
  if (path.indexOf("..") >= 0 || !path.startsWith("/RSPDN")) {
    server.send(403, "text/plain", "forbidden");
    return;
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  File f = SD.open(path, FILE_READ);
  if (!f) {
    xSemaphoreGive(sdMutex);
    server.send(404, "text/plain", "not found");
    return;
  }

  // Kirim manual per-chunk, BUKAN server.streamFile() bawaan library --
  // streamFile() baca SD 64 byte sekaligus lewat Print::write(Stream&), dan
  // seluruh loop baca-tulis itu jalan di DALAM satu panggilan
  // server.handleClient(). webServerTask cuma vTaskDelay(2) ANTAR panggilan
  // handleClient() (lihat webServerTask di bawah), bukan di tengah-tengahnya
  // -- jadi selama transfer berlangsung, task ini nggak pernah kasih napas
  // ke scheduler. Untuk file beberapa MB di atas SPI custom yang sengaja
  // dikonservatifkan ke 4MHz (lihat sd_recorder.ino), itu bisa nahan Core 0
  // cukup lama sampai task IDLE0 kelaparan dan task watchdog ESP32 panic ->
  // reboot, yang dari HP kelihatannya seperti "WiFi ESP tiba-tiba mati".
  // Buffer lebih besar (4KB, sekalian lebih cepat drpd 64 byte per baca) +
  // delay(1) tiap chunk supaya scheduler & watchdog tetap dikasih jatah.
  server.setContentLength(f.size());
  server.send(200, "audio/wav", "");

  // Buffer stdio baca default ~4KB ditahan selama transfer -- kecilkan, RAM
  // internal sempit (lihat sessionFile di session_model.ino). Sudah ada
  // dlBuf 2KB di atasnya, jadi buffer stdio tidak perlu besar.
  f.setBufferSize(512);

  static uint8_t dlBuf[2048];
  NetworkClient &client = server.client();
  const int sockFd = client.fd();
  const unsigned long STALL_LIMIT_MS = 3000;
  size_t n, total = 0, fileSize = f.size();
  bool aborted = false;
  while (!aborted && (n = f.read(dlBuf, sizeof(dlBuf))) > 0) {
    size_t sent = 0;
    unsigned long lastProgressMs = millis();
    while (sent < n) {
      // NetworkClient::write() sendiri nunggu socket siap sampai ~10 detik
      // (1s x 10 retry) -- selama itu web server (satu thread) tidak melayani
      // /status dan app kira alat putus. Cek kesiapan socket per 200ms,
      // menyerah setelah STALL_LIMIT_MS tanpa kemajuan.
      fd_set ws;
      FD_ZERO(&ws);
      FD_SET(sockFd, &ws);
      struct timeval tv = {0, 200000};
      if (select(sockFd + 1, NULL, &ws, NULL, &tv) > 0) {
        size_t w = client.write(dlBuf + sent, n - sent);
        if (w == 0) { aborted = true; break; }
        sent += w;
        lastProgressMs = millis();
      } else if (millis() - lastProgressMs > STALL_LIMIT_MS) {
        aborted = true;
        break;
      }
    }
    total += sent;
    delay(1);
  }
  if (aborted) {
    Serial.printf("[DL] MACET/batal total=%u/%u free=%u largest=%u\n", (unsigned)total, (unsigned)fileSize,
                  ESP.getFreeHeap(), (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    client.stop();
  } else {
    Serial.printf("[DL] selesai total=%u/%u\n", (unsigned)total, (unsigned)fileSize);
  }
  f.close();
  xSemaphoreGive(sdMutex);
}

bool deleteSessionFolder(const String &path) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }

  File entry;
  while ((entry = dir.openNextFile())) {
    String entryName = String(entry.name());
    int slash = entryName.lastIndexOf('/');
    if (slash >= 0) entryName = entryName.substring(slash + 1);
    bool isDir = entry.isDirectory();
    entry.close();

    String entryPath = path + "/" + entryName;
    if (isDir) deleteSessionFolder(entryPath);
    else SD.remove(entryPath);
  }
  dir.close();
  return SD.rmdir(path);
}

// scope=take: hapus 4 file _MIC{n}.wav + .done buat satu take (path = base
// path take, mis. "/RSPDN2/RSPDN2_HRF5_VRS1_3").
// scope=respondent: hapus seluruh folder responden (path = "/RSPDN{r}").
void handleDelete() {
  if (!server.hasArg("path") || !server.hasArg("scope")) {
    server.send(400, "text/plain", "missing path/scope");
    return;
  }
  String path = server.arg("path");
  String scope = server.arg("scope");
  if (path.indexOf("..") >= 0 || !path.startsWith("/RSPDN")) {
    server.send(403, "text/plain", "forbidden");
    return;
  }

  if (scope == "take") {
    if (isRecording && path.equals(currentSessionPath)) {
      server.send(409, "text/plain", "take ini sedang direkam");
      return;
    }
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    for (int n = 1; n <= 4; n++) SD.remove(path + "_MIC" + String(n) + ".wav");
    SD.remove(path + ".done");
    xSemaphoreGive(sdMutex);
    server.send(200, "text/plain", "OK");
  } else if (scope == "respondent") {
    if (isRecording && path.equals("/RSPDN" + String(currentRespondenIdx))) {
      server.send(409, "text/plain", "responden ini sedang direkam");
      return;
    }
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    bool ok = deleteSessionFolder(path);
    xSemaphoreGive(sdMutex);
    server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "gagal hapus");
  } else {
    server.send(400, "text/plain", "scope harus take/respondent");
  }
}

// Start/stop request dari web nggak langsung panggil startRecording()/
// stopRecording() di sini (Core 0) — cukup set flag, biar loop() di Core 1
// yang eksekusi di awal iterasi berikutnya. Menjaga semua mutasi file SD
// tetap di satu core saja.
void handleStart() {
  if (!targetSelected) {
    server.send(409, "text/plain", "pilih target dulu");
    return;
  }
  webStartRequested = true;
  server.send(200, "text/plain", "OK");
}

void handleStop() {
  webStopRequested = true;
  server.send(200, "text/plain", "OK");
}

// ─────────────────────────────────────────────

// Bungkus handler dengan log heap sebelum/sesudah -- diagnosa endpoint mana
// yang menghabiskan blok kontigu RAM internal (/status sengaja dilewati,
// dipanggil tiap 2 detik oleh app dan bakal membanjiri Serial).
void onLogged(const char *uri, HTTPMethod method, WebServer::THandlerFunction fn, bool log = true) {
  if (!log) { server.on(uri, method, fn); return; }
  server.on(uri, method, [uri, fn]() {
    uint32_t f0 = ESP.getFreeHeap(), l0 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[WEB>] %s mulai\n", uri);
    fn();
    Serial.printf("[WEB] %s free=%u->%u largest=%u->%u\n", uri, f0, ESP.getFreeHeap(), l0,
                  (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  });
}

void webServerTask(void *param) {
  onLogged("/", HTTP_GET, handleRoot);
  onLogged("/status", HTTP_GET, handleStatus, false);
  onLogged("/respondents", HTTP_GET, handleGetRespondents);
  onLogged("/respondents", HTTP_POST, handlePostRespondents);
  onLogged("/huruf", HTTP_GET, handleGetHuruf);
  onLogged("/variasi", HTTP_GET, handleGetVariasi);
  onLogged("/select", HTTP_POST, handleSelect);
  onLogged("/takes", HTTP_GET, handleGetTakes);
  onLogged("/download", HTTP_GET, handleDownload);
  onLogged("/delete", HTTP_POST, handleDelete);
  onLogged("/start", HTTP_POST, handleStart);
  onLogged("/stop", HTTP_POST, handleStop);
  server.begin();

  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));  // kasih napas ke task WiFi internal di Core 0
  }
}

void startWebServerTask() {
  xTaskCreatePinnedToCore(
    webServerTask, "webServerTask",
    10240,                  // stack besar — handler pakai String/JSON lumayan boros
    NULL,
    1,                       // prioritas di bawah task WiFi internal
    &webServerTaskHandle,
    0                        // Core 0 — Core 1 tetap fokus I2S + SD write
  );
}
