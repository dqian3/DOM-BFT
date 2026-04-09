#!/usr/bin/env python3
"""Sweep over send rates and collect throughput/latency results for OooBFT.

Usage:
    python sweep.py local --config configs/gcloud.yaml --protocol dombft --rates 500 750 1000 1500
    python sweep.py remote --config configs/gcloud.yaml --protocol flutter --rates 500 750 1000
    python sweep.py remote --config configs/gcloud.yaml --rates 500 1000 --transport tcp
    python sweep.py analyze --dir logs/sweep_20260408_120000
"""

import argparse
import json
import os
import shutil
import sys
import time
from datetime import datetime

import glob as globmod

from bench import (
    apply_bench_overrides,
    resolve_remote_cluster,
    resolve_local_cluster,
    remote_targets,
    parse_client_output,
    print_aggregate_results,
    _remote_run,
    _remote_upload_keys,
    _local_dombft,
    _local_flutter,
    DOMBFT_BINARIES,
    FLUTTER_BINARIES,
    PROJECT_ROOT,
)
from config_model import generate_config
from config_model import load_cluster_config
from remote import load_remote


def run_one_local(config, protocol, rate, transport, log_dir):
    """Run a single local benchmark at the given send rate."""
    config = apply_bench_overrides(config, send_rate=rate, transport=transport)
    resolved = resolve_local_cluster(config, protocol)

    config_path = generate_config(resolved, protocol, log_dir)

    if protocol == "flutter":
        _local_flutter(resolved, config_path, log_dir)
    else:
        _local_dombft(resolved, config_path, log_dir)

    return _collect_client_outputs(log_dir, resolved, protocol)


def run_one_remote(config, protocol, rate, transport, remote_obj, log_dir, skip_keys=False):
    """Run a single remote benchmark at the given send rate."""
    config = apply_bench_overrides(config, send_rate=rate, transport=transport)
    resolved = resolve_remote_cluster(config, protocol, remote_obj)

    binaries = FLUTTER_BINARIES if protocol == "flutter" else DOMBFT_BINARIES
    _remote_run(resolved, config, remote_obj, protocol, log_dir, binaries, skip_keys=skip_keys)

    return _collect_client_outputs(log_dir, resolved, protocol)


def _collect_client_outputs(log_dir, resolved, protocol):
    """Read client log files and parse results."""
    n_clients = len(resolved.clients)
    client_outputs = []
    for i in range(n_clients):
        for pattern in [f"flutter_client{i}.log", f"client{i}.log", f"client_{i}.log", f"client_{i}.stdout"]:
            path = os.path.join(log_dir, pattern)
            try:
                with open(path) as f:
                    output = f.read()
                if output.strip():
                    client_outputs.append(output)
                    break
            except FileNotFoundError:
                continue

    parsed = [parse_client_output(o) for o in client_outputs]
    parsed = [p for p in parsed if p]
    return client_outputs, parsed


def _aggregate(rate, transport, parsed):
    entry = {
        "transport": transport,
        "rate": rate,
        "num_clients": len(parsed),
        "total_committed": sum(p.get("committed", 0) for p in parsed),
    }
    lat_clients = [p for p in parsed if "latency_p50" in p]
    if lat_clients:
        for key in ("latency_p50", "latency_p95", "latency_p99", "latency_max"):
            entry[key] = sum(p[key] for p in lat_clients) / len(lat_clients)
    return entry


def print_summary(all_results, sweep_dir):
    if not all_results:
        print("No results")
        return

    print(f"\n{'=' * 90}")
    print(f"  SWEEP SUMMARY")
    print(f"{'=' * 90}")

    header = f"{'Transport':>10} {'Rate':>8} {'Clients':>8} {'Committed':>10} {'p50(ms)':>10} {'p95(ms)':>10} {'p99(ms)':>10} {'max(ms)':>10}"
    print(header)
    print("-" * len(header))

    for r in all_results:
        print(
            f"{r.get('transport', '?'):>10} "
            f"{r.get('rate', 0):>8} "
            f"{r.get('num_clients', 0):>8} "
            f"{r.get('total_committed', 0):>10} "
            f"{r.get('latency_p50', 0):>10.2f} "
            f"{r.get('latency_p95', 0):>10.2f} "
            f"{r.get('latency_p99', 0):>10.2f} "
            f"{r.get('latency_max', 0):>10.2f}"
        )

    print(f"{'=' * 90}")
    print(f"Full results: {sweep_dir}/sweep_results.json")


def analyze(sweep_dir):
    results_path = os.path.join(sweep_dir, "sweep_results.json")
    if os.path.exists(results_path):
        with open(results_path) as f:
            all_results = json.load(f)
        print_summary(all_results, sweep_dir)
        return

    rate_dirs = sorted(globmod.glob(os.path.join(sweep_dir, "*_rate_*")), key=os.path.getmtime)
    if not rate_dirs:
        print(f"No results found in {sweep_dir}")
        sys.exit(1)

    all_results = []
    for rate_dir in rate_dirs:
        summary_path = os.path.join(rate_dir, "summary.json")
        if os.path.exists(summary_path):
            with open(summary_path) as f:
                all_results.append(json.load(f))

    print_summary(all_results, sweep_dir)


def main():
    ap = argparse.ArgumentParser(description="Sweep send rates for OooBFT throughput/latency curve.")
    sub = ap.add_subparsers(dest="command", required=True)

    for mode in ["local", "remote"]:
        p = sub.add_parser(mode)
        p.add_argument("--config", required=True)
        p.add_argument("--protocol", default="dombft", choices=["dombft", "flutter"])
        p.add_argument("--rates", type=int, nargs="+", required=True)
        p.add_argument("--transport", default="tcp")
        p.add_argument("--output-dir", default=None)

    a = sub.add_parser("analyze")
    a.add_argument("--dir", required=True)

    args = ap.parse_args()

    if args.command == "analyze":
        analyze(args.dir)
        return

    config = load_cluster_config(args.config)
    protocol = args.protocol
    transport = args.transport
    rates = args.rates
    is_remote = args.command == "remote"

    remote_obj = None
    if is_remote:
        remote_obj = load_remote({"platform": config.platform, "zone": config.zone,
                                   "project": config.project, "user": config.user,
                                   "key_file": config.key_file})
        vms = remote_targets(config)
        if hasattr(remote_obj, '_discover_all'):
            remote_obj._discover_all(vms)
        if hasattr(remote_obj, 'check_vms_running'):
            remote_obj.check_vms_running(vms)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    sweep_dir = args.output_dir or os.path.join(PROJECT_ROOT, "logs", f"sweep_{timestamp}")
    os.makedirs(sweep_dir, exist_ok=True)
    shutil.copy2(args.config, os.path.join(sweep_dir, os.path.basename(args.config)))

    print(f"=== Sweep: protocol={protocol}, transport={transport}, rates={rates} ===")
    print(f"Output: {sweep_dir}")

    all_results = []

    # For remote sweeps, upload keys once before the first run
    keys_uploaded = False
    if is_remote:
        first_resolved = resolve_remote_cluster(
            apply_bench_overrides(config, send_rate=rates[0], transport=transport),
            protocol, remote_obj)
        first_log = os.path.join(sweep_dir, f"{transport}_rate_{rates[0]}")
        _remote_upload_keys(first_resolved, config, remote_obj, protocol, first_log)
        keys_uploaded = True

    for rate in rates:
        print(f"\n{'#' * 60}")
        print(f"  send_rate={rate} transport={transport}")
        print(f"{'#' * 60}")

        log_dir = os.path.join(sweep_dir, f"{transport}_rate_{rate}")

        if is_remote:
            _, parsed = run_one_remote(config, protocol, rate, transport, remote_obj, log_dir, skip_keys=True)
        else:
            _, parsed = run_one_local(config, protocol, rate, transport, log_dir)

        entry = _aggregate(rate, transport, parsed)
        all_results.append(entry)

        with open(os.path.join(log_dir, "summary.json"), "w") as f:
            json.dump(entry, f, indent=2)

        if rate != rates[-1]:
            print("\nWaiting 10s before next run...")
            time.sleep(10)

    with open(os.path.join(sweep_dir, "sweep_results.json"), "w") as f:
        json.dump(all_results, f, indent=2)

    print_summary(all_results, sweep_dir)


if __name__ == "__main__":
    main()
