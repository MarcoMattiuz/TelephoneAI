#!/usr/bin/env python3
"""Parse ESP-IDF AMP log lines and play reconstructed audio.

Expected line payload format (any prefix/suffix is tolerated):
    AMP,<timestamp_ms>,<frame_count>,<s0>,<s1>,...,<sN>
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import wave
from pathlib import Path

AMP_PATTERN = re.compile(r"AMP,(\d+),(\d+),(.*)")
S24_MIN = -(1 << 23)
S24_MAX = (1 << 23) - 1
I16_MIN = -32768
I16_MAX = 32767


def parse_amp_samples(log_path: Path) -> list[int]:
    samples: list[int] = []

    for raw_line in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        match = AMP_PATTERN.search(raw_line)
        if not match:
            continue

        frame_count = int(match.group(2))
        payload = match.group(3)
        parsed: list[int] = []

        if payload:
            for token in payload.split(","):
                token = token.strip()
                if not token:
                    continue
                try:
                    parsed.append(int(token))
                except ValueError:
                    continue

        if frame_count > 0 and len(parsed) > frame_count:
            parsed = parsed[:frame_count]

        if parsed:
            samples.extend(parsed)

    return samples


def to_int16(samples_s24: list[int], gain: float) -> bytes:
    if not samples_s24:
        return b""

    clamped_s24 = [max(S24_MIN, min(S24_MAX, s)) for s in samples_s24]
    peak = max(abs(s) for s in clamped_s24)
    if peak == 0:
        return b"\x00\x00" * len(clamped_s24)

    scale = (I16_MAX / float(peak)) * gain

    pcm = bytearray()
    for s in clamped_s24:
        v = int(round(s * scale))
        v = max(I16_MIN, min(I16_MAX, v))
        pcm.extend(v.to_bytes(2, byteorder="little", signed=True))

    return bytes(pcm)


def write_wav(wav_path: Path, pcm_s16le: bytes, sample_rate: int) -> None:
    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_s16le)


def play_wav_macos(wav_path: Path) -> None:
    afplay = shutil.which("afplay")
    if afplay is None:
        raise RuntimeError("afplay non trovato sul sistema")

    subprocess.run([afplay, str(wav_path)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Riproduce audio da righe AMP nel log ESP-IDF")
    parser.add_argument("log_file", type=Path, help="Percorso del file log (es. amp.log)")
    parser.add_argument("--sample-rate", type=int, default=16000, help="Sample rate I2S (default: 16000)")
    parser.add_argument("--gain", type=float, default=0.9, help="Gain post-normalizzazione (default: 0.9)")
    parser.add_argument("--wav-out", type=Path, default=Path("amp_reconstructed.wav"), help="File WAV di output")
    parser.add_argument("--no-play", action="store_true", help="Non riprodurre audio, salva solo WAV")
    args = parser.parse_args()

    if args.sample_rate <= 0:
        print("Errore: sample-rate deve essere > 0", file=sys.stderr)
        return 2

    if args.gain <= 0:
        print("Errore: gain deve essere > 0", file=sys.stderr)
        return 2

    samples = parse_amp_samples(args.log_file)
    if not samples:
        print("Nessun campione AMP trovato nel log", file=sys.stderr)
        return 1

    pcm = to_int16(samples, args.gain)
    write_wav(args.wav_out, pcm, args.sample_rate)

    duration_s = len(samples) / float(args.sample_rate)
    print(f"Campioni: {len(samples)}")
    print(f"Durata: {duration_s:.2f} s")
    print(f"WAV: {args.wav_out}")

    if not args.no_play:
        play_wav_macos(args.wav_out)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
