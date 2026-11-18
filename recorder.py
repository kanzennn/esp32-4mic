#!/usr/bin/env python3
"""
recorder.py — Terima audio dari ESP32 via Serial, simpan sebagai WAV.

Penggunaan:
    python recorder.py <COM_PORT>

Contoh:
    python recorder.py COM3          (Windows)
    python recorder.py /dev/ttyUSB0  (Linux/Mac)

Install dependency (sekali saja):
    pip install pyserial

File tersimpan di:
    recording/<YYYY-MM-DD_HH-MM-SS>/mic1.wav
    recording/<YYYY-MM-DD_HH-MM-SS>/mic2.wav
"""

import serial
import struct
import wave
import os
import sys
import datetime

SAMPLE_RATE  = 8000    # harus sama dengan SAMPLE_RATE di .ino
BAUD_RATE    = 921600

START_MARKER = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF])
END_MARKER   = bytes([0xCA, 0xFE, 0xBA, 0xBE, 0xCA, 0xFE, 0xBA, 0xBE])


def write_wav(path, samples):
    with wave.open(path, 'w') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(struct.pack(f'<{len(samples)}h', *samples))


def find_marker(ser, marker):
    """Baca byte satu-satu sampai urutan marker ditemukan."""
    buf = bytearray()
    ml = len(marker)
    while True:
        b = ser.read(1)
        if not b:
            continue
        buf.append(b[0])
        if len(buf) > ml:
            del buf[0]
        if bytes(buf) == marker:
            return


def collect_audio(ser):
    """
    Kumpulkan semua byte audio sampai END_MARKER diterima.
    Menggunakan sliding window untuk deteksi marker yang aman.
    """
    audio  = bytearray()
    window = bytearray()
    ml     = len(END_MARKER)

    while True:
        chunk = ser.read(2048)  # baca lebih besar per call untuk kurangi overhead system call
        if not chunk:
            continue
        window.extend(chunk)

        while len(window) >= ml:
            idx = window.find(END_MARKER)
            if idx >= 0:
                audio.extend(window[:idx])
                return bytes(audio)
            # Pindahkan byte yang sudah pasti bukan bagian marker ke buffer audio
            safe = len(window) - (ml - 1)
            audio.extend(window[:safe])
            del window[:safe]


def split_channels(raw):
    """
    Pisahkan byte interleaved [m1, m2, m3, m4, m1, m2, m3, m4, ...]
    menjadi empat list sampel int16.
    Setiap frame = 8 byte (4 × int16 little-endian).
    """
    n = len(raw) // 8  # setiap frame = 4 mic × 2 byte = 8 byte
    mic1, mic2, mic3, mic4 = [], [], [], []
    for i in range(n):
        off = i * 8
        mic1.append(struct.unpack_from('<h', raw, off)[0])
        mic2.append(struct.unpack_from('<h', raw, off + 2)[0])
        mic3.append(struct.unpack_from('<h', raw, off + 4)[0])
        mic4.append(struct.unpack_from('<h', raw, off + 6)[0])
    return mic1, mic2, mic3, mic4


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    port     = sys.argv[1]
    base_dir = os.path.dirname(os.path.abspath(__file__))
    rec_root = os.path.join(base_dir, "recording")
    os.makedirs(rec_root, exist_ok=True)

    print(f"Menghubungkan ke {port} @ {BAUD_RATE} baud...")
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=2)
        # Perbesar buffer OS agar tidak ada byte yang terbuang saat Python sibuk
        try:
            ser.set_buffer_size(rx_size=65536, tx_size=65536)
        except AttributeError:
            pass  # non-Windows, tidak perlu
    except serial.SerialException as e:
        print(f"[ERROR] Gagal buka port: {e}")
        print("Pastikan Serial Monitor Arduino IDE sudah ditutup.")
        sys.exit(1)

    print("Terhubung!")
    print("Tekan tombol pada ESP32 untuk mulai rekam.")
    print("Ctrl+C untuk keluar.\n")

    try:
        while True:
            print("Menunggu rekaman...")
            find_marker(ser, START_MARKER)

            # Baca timestamp 4 byte (uint32 little-endian) — cuma buat framing protokol,
            # nggak dipakai buat nama folder karena ini millis() sejak ESP32 boot,
            # bukan waktu asli (ESP32 nggak punya RTC/NTP). Nama folder pakai jam PC.
            ts_raw = ser.read(4)
            if len(ts_raw) < 4:
                print("[!] Timestamp tidak lengkap, ulangi.")
                continue

            folder_name = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
            folder = os.path.join(rec_root, folder_name)
            suffix = 2
            while os.path.exists(folder):
                folder = os.path.join(rec_root, f"{folder_name}_{suffix}")
                suffix += 1
            os.makedirs(folder)
            print(f"[REC] Merekam... (recording/{os.path.basename(folder)}/)")

            raw = collect_audio(ser)

            mic1, mic2, mic3, mic4 = split_channels(raw)
            duration = len(mic1) / SAMPLE_RATE

            paths = [os.path.join(folder, f"mic{n}.wav") for n in range(1, 5)]
            for path, ch in zip(paths, [mic1, mic2, mic3, mic4]):
                write_wav(path, ch)

            print(f"[OK]  Selesai — {len(mic1)} sample ({duration:.1f} detik)")
            for p in paths:
                print(f"      {p}")
            print()

    except KeyboardInterrupt:
        print("\nDihentikan.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
