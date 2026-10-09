#!/usr/bin/env python3
"""Parse ESP-IDF AMP CSV logs and plot 24-bit amplitude samples."""

import argparse
import re
from pathlib import Path

import matplotlib.pyplot as plt

AMP_PATTERN = re.compile(r"AMP,(\d+),(\d+),(.*)")


def parse_amp_log(log_path: Path):
    samples = []
    chunk_timestamps_ms = []

    for raw_line in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        match = AMP_PATTERN.search(raw_line)
        if not match:
            continue

        ts_ms = int(match.group(1))
        frame_count = int(match.group(2))
        payload = match.group(3)

        values = []
        if payload:
            for token in payload.split(","):
                token = token.strip()
                if not token:
                    continue
                try:
                    values.append(int(token))
                except ValueError:
                    # Ignore malformed token and keep parsing the rest of the line.
                    continue

        if frame_count and len(values) > frame_count:
            values = values[:frame_count]

        if values:
            chunk_timestamps_ms.extend([ts_ms] * len(values))
            samples.extend(values)

    return samples, chunk_timestamps_ms


def build_time_axis(sample_count: int, sample_rate_hz: int):
    return [i / float(sample_rate_hz) for i in range(sample_count)]


def main():
    parser = argparse.ArgumentParser(description="Plot 24-bit amplitude samples from ESP-IDF AMP logs")
    parser.add_argument("log_file", type=Path, help="Path to serial monitor log file")
    parser.add_argument("--sample-rate", type=int, default=16000, help="Sample rate used by I2S capture (default: 16000)")
    parser.add_argument("--max-samples", type=int, default=0, help="Plot at most N samples (0 = all)")
    parser.add_argument("--save", type=Path, default=None, help="Optional output image path (e.g. amp_plot.png)")
    args = parser.parse_args()

    samples, chunk_ts = parse_amp_log(args.log_file)
    if not samples:
        raise SystemExit("No AMP lines found in log file.")

    if args.max_samples > 0:
        samples = samples[: args.max_samples]
        chunk_ts = chunk_ts[: args.max_samples]

    time_axis_s = build_time_axis(len(samples), args.sample_rate)

    fig, ax = plt.subplots(figsize=(14, 5))
    ax.plot(time_axis_s, samples, linewidth=0.8)
    ax.set_title("INMP441 24-bit Amplitude")
    ax.set_xlabel("Time [s]")
    ax.set_ylabel("Amplitude (signed 24-bit)")
    ax.grid(True, alpha=0.3)

    if chunk_ts:
        ax.text(
            0.01,
            0.98,
            f"chunk_ts(ms): first={chunk_ts[0]}, last={chunk_ts[-1]}",
            transform=ax.transAxes,
            verticalalignment="top",
            fontsize=9,
            bbox={"facecolor": "white", "alpha": 0.6, "edgecolor": "none"},
        )

    fig.tight_layout()

    if args.save is not None:
        fig.savefig(args.save, dpi=150)
        print(f"Saved plot to {args.save}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
