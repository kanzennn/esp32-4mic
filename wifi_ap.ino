/*
 * wifi_ap.ino — ESP32 jadi WiFi Access Point sendiri (standalone, tanpa
 * jaringan lain). HP connect langsung ke SSID ini, buka browser ke IP-nya
 * untuk kontrol rekam + file manager (lihat web_ui.ino).
 */

#include <WiFi.h>
#include "esp_wifi.h"

// AP_SSID, AP_PASSWORD, apIP dideklarasikan di esp32.txt.ino (state bersama).

bool initSoftAP() {
  // Diagnosa: HP kadang nggak bisa konek ke AP ini kecuali ESP32 di-restart
  // (heap internal habis/terfragmentasi tiap kali ada station connect) --
  // log event station buat lihat sisi ESP32.
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
      const uint8_t *m = info.wifi_ap_staconnected.mac;
      Serial.printf("[WIFI-EV] STA connected %02x:%02x:%02x:%02x:%02x:%02x aid=%d\n",
                    m[0], m[1], m[2], m[3], m[4], m[5], info.wifi_ap_staconnected.aid);
    } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
      const uint8_t *m = info.wifi_ap_stadisconnected.mac;
      Serial.printf("[WIFI-EV] STA disconnected %02x:%02x:%02x:%02x:%02x:%02x aid=%d reason=%d\n",
                    m[0], m[1], m[2], m[3], m[4], m[5],
                    info.wifi_ap_stadisconnected.aid, info.wifi_ap_stadisconnected.reason);
    } else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
      Serial.println("[WIFI-EV] STA IP assigned");
    }
  });

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
  // 802.11b/g saja (tanpa 11n): matikan agregasi AMPDU yang alokasi buffer
  // RX/TX-nya besar per station -- RAM internal sisa sedikit (DMA I2S makan
  // ~64KB) dan throughput 54Mbps masih jauh lebih dari cukup buat unduh WAV.
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
  delay(100);
  apIP = WiFi.softAPIP();
  return ok;
}
