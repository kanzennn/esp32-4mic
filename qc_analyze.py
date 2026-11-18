#!/usr/bin/env python3
"""
qc_analyze.py — QC + hitung energi per-channel untuk dataset rekaman 4-mic.

Untuk tiap sesi di recording/<sesi>/mic1-4.wav:
    1. Deteksi glitch (lompatan sampel akibat drift/overflow DMA Bus 1).
    2. Perbaiki glitch pendek lewat interpolasi linear; glitch panjang cuma
       ditandai (tidak diperbaiki otomatis, karena interpolasi panjang
       berisiko menghapus sinyal asli).
    3. Hitung energi per-frame (short-time RMS, ~25ms/frame) dan energi
       keseluruhan per channel — dipakai nanti sebagai fitur klasifikasi
       makhraj (rasio energi mulut/hidung/tenggorokan).

File asli di recording/ TIDAK diubah. Hasil (WAV yang sudah dibersihkan +
laporan QC + energi per-frame) disimpan terpisah di recording_cleaned/.

Penggunaan:
    pip install numpy   # satu kali
    python qc_analyze.py                  # proses semua sesi di recording/
    python qc_analyze.py --session 2928   # proses satu sesi saja
"""

import argparse
import csv
import json
import os
import wave

import numpy as np

SAMPLE_RATE = 8000
FRAME_MS = 25
HOP_MS = 10
MAX_REPAIR_LEN = 20      # sampel; glitch lebih panjang dari ini cuma ditandai, tidak diperbaiki
CLUSTER_GAP = 50         # sampel; jarak antar lompatan yang masih dianggap 1 event glitch
MIN_JUMP_THRESH = 2000   # ambang minimum lompatan sampel (skala int16)
JUMP_MULTIPLIER = 40     # ambang = median(diff) * JUMP_MULTIPLIER (adaptif per channel)


def read_wav_int16(path):
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        n = w.getnframes()
        data = w.readframes(n)
    return np.frombuffer(data, dtype=np.int16).copy(), sr


def write_wav_int16(path, samples, sr):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(samples.astype(np.int16).tobytes())


def detect_glitch_events(x):
    """Cari lompatan sampel abnormal, kelompokkan jadi event (start, end inklusif)."""
    if len(x) < 2:
        return []
    diffs = np.abs(np.diff(x.astype(np.int64)))
    med = np.median(diffs) + 1e-6
    thresh = max(MIN_JUMP_THRESH, med * JUMP_MULTIPLIER)
    spike_idx = np.where(diffs > thresh)[0]
    if len(spike_idx) == 0:
        return []

    events = []
    cur = [spike_idx[0]]
    for i in spike_idx[1:]:
        if i - cur[-1] <= CLUSTER_GAP:
            cur.append(i)
        else:
            events.append((cur[0], cur[-1] + 1))
            cur = [i]
    events.append((cur[0], cur[-1] + 1))
    return events


def repair_glitches(x, events):
    """Interpolasi linear glitch pendek. Return (array_diperbaiki, list_detail_event)."""
    x = x.copy()
    details = []
    for start, end in events:
        length = end - start
        repaired = False
        if length <= MAX_REPAIR_LEN and start > 0 and end < len(x) - 1:
            lo, hi = x[start - 1], x[end + 1]
            x[start:end + 2] = np.linspace(lo, hi, num=(end + 2 - start), dtype=x.dtype)
            repaired = True
        details.append({
            "start_sample": int(start),
            "end_sample": int(end),
            "start_time_s": round(start / SAMPLE_RATE, 4),
            "length_samples": int(length),
            "repaired": repaired,
        })
    return x, details


def frame_energy(x, sr, frame_ms=FRAME_MS, hop_ms=HOP_MS):
    """Short-time RMS energy per frame. Return (times_s, rms, rms_db)."""
    frame_len = max(1, int(sr * frame_ms / 1000))
    hop_len = max(1, int(sr * hop_ms / 1000))
    xf = x.astype(np.float64)

    n_frames = max(0, (len(xf) - frame_len) // hop_len + 1)
    times, rms_vals = [], []
    for i in range(n_frames):
        start = i * hop_len
        seg = xf[start:start + frame_len]
        rms = np.sqrt(np.mean(seg ** 2)) if len(seg) > 0 else 0.0
        times.append(start / sr)
        rms_vals.append(rms)

    rms_vals = np.array(rms_vals)
    rms_db = 20 * np.log10(np.maximum(rms_vals, 1.0) / 32768.0)
    return np.array(times), rms_vals, rms_db


def process_session(session_dir, out_root, session_name):
    out_dir = os.path.join(out_root, session_name)
    os.makedirs(out_dir, exist_ok=True)

    session_report = {"session": session_name, "mics": {}}
    summary_rows = []

    for n in range(1, 5):
        mic_name = f"mic{n}"
        in_path = os.path.join(session_dir, f"{mic_name}.wav")
        if not os.path.exists(in_path):
            continue

        x, sr = read_wav_int16(in_path)
        events = detect_glitch_events(x)
        x_clean, details = repair_glitches(x, events)

        n_repaired = sum(1 for d in details if d["repaired"])
        n_flagged = len(details) - n_repaired

        out_wav = os.path.join(out_dir, f"{mic_name}.wav")
        write_wav_int16(out_wav, x_clean, sr)

        times, rms_vals, rms_db = frame_energy(x_clean, sr)
        energy_csv = os.path.join(out_dir, f"energy_{mic_name}.csv")
        with open(energy_csv, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["time_s", "rms", "rms_db"])
            for t, r, rdb in zip(times, rms_vals, rms_db):
                writer.writerow([f"{t:.4f}", f"{r:.2f}", f"{rdb:.2f}"])

        overall_rms = float(np.sqrt(np.mean(x_clean.astype(np.float64) ** 2)))
        overall_rms_db = float(20 * np.log10(max(overall_rms, 1.0) / 32768.0))

        session_report["mics"][mic_name] = {
            "num_samples": int(len(x)),
            "duration_s": round(len(x) / sr, 3),
            "num_glitch_events": len(details),
            "num_repaired": n_repaired,
            "num_flagged_unrepaired": n_flagged,
            "overall_rms": round(overall_rms, 2),
            "overall_rms_db": round(overall_rms_db, 2),
            "glitch_events": details,
        }

        summary_rows.append({
            "session": session_name,
            "mic": mic_name,
            "duration_s": round(len(x) / sr, 3),
            "num_glitch_events": len(details),
            "num_repaired": n_repaired,
            "num_flagged_unrepaired": n_flagged,
            "overall_rms_db": round(overall_rms_db, 2),
        })

    report_path = os.path.join(out_dir, "qc_report.json")
    with open(report_path, "w") as f:
        json.dump(session_report, f, indent=2)

    return summary_rows


def main():
    parser = argparse.ArgumentParser(description="QC + energi per-channel untuk dataset rekaman 4-mic")
    parser.add_argument("--recording-dir", default="recording", help="Folder sumber rekaman (default: recording)")
    parser.add_argument("--out-dir", default="recording_cleaned", help="Folder output (default: recording_cleaned)")
    parser.add_argument("--session", default=None, help="Proses satu sesi saja (nama folder), default: semua sesi")
    args = parser.parse_args()

    base_dir = os.path.dirname(os.path.abspath(__file__))
    recording_root = os.path.join(base_dir, args.recording_dir)
    out_root = os.path.join(base_dir, args.out_dir)

    if not os.path.isdir(recording_root):
        print(f"[ERROR] Folder tidak ditemukan: {recording_root}")
        return

    if args.session:
        sessions = [args.session]
    else:
        sessions = sorted(
            d for d in os.listdir(recording_root)
            if os.path.isdir(os.path.join(recording_root, d))
        )

    if not sessions:
        print("Tidak ada sesi rekaman ditemukan.")
        return

    os.makedirs(out_root, exist_ok=True)
    all_rows = []

    for session_name in sessions:
        session_dir = os.path.join(recording_root, session_name)
        print(f"[QC] Memproses sesi: {session_name}")
        rows = process_session(session_dir, out_root, session_name)
        for r in rows:
            flag = " <-- CEK GLITCH PANJANG" if r["num_flagged_unrepaired"] > 0 else ""
            print(f"      {r['mic']}: {r['num_glitch_events']} glitch "
                  f"({r['num_repaired']} diperbaiki, {r['num_flagged_unrepaired']} ditandai), "
                  f"RMS={r['overall_rms_db']:.1f} dBFS{flag}")
        all_rows.extend(rows)

    summary_path = os.path.join(out_root, "qc_summary.csv")
    with open(summary_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "session", "mic", "duration_s", "num_glitch_events",
            "num_repaired", "num_flagged_unrepaired", "overall_rms_db",
        ])
        writer.writeheader()
        writer.writerows(all_rows)

    print(f"\n[OK] Selesai. {len(sessions)} sesi diproses.")
    print(f"     WAV bersih + energi per-frame: {out_root}/<sesi>/")
    print(f"     Ringkasan semua sesi: {summary_path}")


if __name__ == "__main__":
    main()
