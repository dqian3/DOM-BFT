#!/usr/bin/env python3

import re
import sys
import matplotlib.pyplot as plt
import numpy as np
import argparse


def parse_latency_logs(log_file):
    """Parse bench_simple_rpc logs and extract latency samples."""
    all_latencies = []
    all_timestamps = []
    all_sequences = []

    pattern = r"LATENCY_SAMPLE seq=(\d+) latency_us=(\d+) recv_time=(\d+)"

    with open(log_file, "r") as f:
        for line in f:
            match = re.search(pattern, line)
            if match:
                seq = int(match.group(1))
                latency_us = int(match.group(2))
                recv_time = int(match.group(3))

                all_sequences.append(seq)
                all_latencies.append(latency_us)
                all_timestamps.append(recv_time)

    # Filter out first 5 seconds of measurements
    if all_timestamps:
        start_time = min(all_timestamps)
        warmup_cutoff = start_time + 5_000_000  # 5 seconds in microseconds

        filtered_data = [(seq, lat, ts) for seq, lat, ts in
                        zip(all_sequences, all_latencies, all_timestamps)
                        if ts >= warmup_cutoff]

        if filtered_data:
            sequences, latencies, timestamps = zip(*filtered_data)
            print(f"Filtered out {len(all_latencies) - len(latencies)} samples from first 5 seconds (warmup period)")
            return list(sequences), list(latencies), list(timestamps)
        else:
            print("Warning: All samples were within the first 5 seconds - returning empty data")
            return [], [], []

    return all_sequences, all_latencies, all_timestamps


def generate_histogram(latencies, output_file=None):
    """Generate latency histogram."""
    plt.figure(figsize=(10, 6))

    # Convert to milliseconds for better readability
    latencies_ms = [lat / 1000.0 for lat in latencies]

    # Simple histogram with automatic binning
    plt.hist(latencies_ms, bins=30, alpha=0.7, edgecolor="black")
    plt.xlabel("Latency (ms)")
    plt.ylabel("Frequency")
    plt.title(f"Latency Distribution (n={len(latencies)})")
    plt.grid(True, alpha=0.3)

    plt.tight_layout()

    if output_file:
        plt.savefig(output_file, dpi=300, bbox_inches="tight")
        print(f"Histogram saved to {output_file}")
    else:
        plt.show()


def generate_timeline(sequences, latencies, timestamps, output_file=None):
    """Generate latency timeline."""
    plt.figure(figsize=(12, 6))

    # Convert timestamps to relative time in seconds
    if timestamps:
        start_time = min(timestamps)
        rel_times = [
            (ts - start_time) / 1_000_000.0 for ts in timestamps
        ]  # Convert to seconds
        latencies_ms = [lat / 1000.0 for lat in latencies]

        plt.scatter(rel_times, latencies_ms, alpha=0.6, s=1)
        plt.xlabel("Time (seconds)")
        plt.ylabel("Latency (ms)")
        plt.title("Latency vs Time")
        plt.grid(True, alpha=0.3)

    plt.tight_layout()

    if output_file:
        plt.savefig(output_file, dpi=300, bbox_inches="tight")
        print(f"Timeline saved to {output_file}")
    else:
        plt.show()


def print_statistics(sequences, latencies, timestamps):
    """Print detailed statistics."""
    if not latencies:
        print("No latency data found!")
        return

    latencies_ms = [lat / 1000.0 for lat in latencies]

    print("\n=== Latency Statistics ===")
    print(f"Total samples: {len(latencies)}")
    print(f"Mean latency: {np.mean(latencies_ms):.3f} ms")
    print(f"Median latency: {np.median(latencies_ms):.3f} ms")
    print(f"Min latency: {np.min(latencies_ms):.3f} ms")
    print(f"Max latency: {np.max(latencies_ms):.3f} ms")
    print(f"Std deviation: {np.std(latencies_ms):.3f} ms")

    percentiles = [50, 90, 95, 99, 99.9]
    print("\nPercentiles:")
    for p in percentiles:
        print(f"  P{p}: {np.percentile(latencies_ms, p):.3f} ms")

    # Check for packet drops
    if sequences:
        expected_total = max(sequences) - min(sequences) + 1
        actual_total = len(sequences)
        drops = expected_total - actual_total
        if drops > 0:
            drop_rate = (drops / expected_total) * 100
            print("\n=== Packet Drop Statistics ===")
            print(f"Expected packets: {expected_total}")
            print(f"Received packets: {actual_total}")
            print(f"Dropped packets: {drops}")
            print(f"Drop rate: {drop_rate:.2f}%")
        else:
            print("\n=== Packet Drop Statistics ===")
            print("No packet drops detected")

    # Timeline statistics
    if timestamps and len(timestamps) > 1:
        duration = (max(timestamps) - min(timestamps)) / 1_000_000.0
        throughput = len(timestamps) / duration
        print("\n=== Timeline Statistics ===")
        print(f"Duration: {duration:.2f} seconds")
        print(f"Throughput: {throughput:.2f} packets/second")


def main():
    parser = argparse.ArgumentParser(
        description="Analyze latencies from bench_simple_rpc logs"
    )
    parser.add_argument("log_file", help="Path to the log file")
    parser.add_argument("--hist", help="Save histogram to file")
    parser.add_argument("--timeline", help="Save timeline to file")
    parser.add_argument(
        "--no-show", action="store_true", help="Don't show plots interactively"
    )

    args = parser.parse_args()

    try:
        sequences, latencies, timestamps = parse_latency_logs(args.log_file)

        if not latencies:
            print("No LATENCY_SAMPLE entries found in the log file!")
            print("Make sure the log contains lines with 'LATENCY_SAMPLE' tag.")
            return 1

        print_statistics(sequences, latencies, timestamps)

        # Generate histogram
        if args.hist or not args.no_show:
            generate_histogram(latencies, args.hist)

        # Generate timeline
        if args.timeline or not args.no_show:
            generate_timeline(sequences, latencies, timestamps, args.timeline)

        if not args.no_show and not (args.hist or args.timeline):
            plt.show()

    except FileNotFoundError:
        print(f"Error: Log file '{args.log_file}' not found!")
        return 1
    except Exception as e:
        print(f"Error processing log file: {e}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
