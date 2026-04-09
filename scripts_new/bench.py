#!/usr/bin/env python3
"""Benchmark automation for OooBFT (DOM-BFT / Flutter).

Usage:
    python bench.py local --protocol dombft --config configs/local.yaml
    python bench.py local --protocol flutter --config configs/flutter.yaml
    python bench.py remote --config configs/gcloud.yaml --protocol dombft
    python bench.py upload --config configs/gcloud.yaml
    python bench.py genkeys --config configs/gcloud.yaml
    python bench.py vm-start --config configs/gcloud.yaml
    python bench.py vm-stop --config configs/gcloud.yaml
    python bench.py vm-keep-alive --config configs/gcloud.yaml
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

from concurrent.futures import ThreadPoolExecutor, as_completed

import yaml

from remote import load_remote, GCloudRemote
from config_model import (
    ConfigError,
    apply_bench_overrides,
    generate_config,
    load_cluster_config,
    remote_targets,
    resolve_local_cluster,
    resolve_remote_cluster,
)

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DOMBFT_BINARIES = ["dombft_replica", "dombft_proxy", "dombft_client"]
FLUTTER_BINARIES = ["flutter_replica", "flutter_client"]
ALL_BINARIES = DOMBFT_BINARIES + FLUTTER_BINARIES
ALL_PROCESS_NAMES = ALL_BINARIES  # for killall


def bazel_build():
    """Build all process binaries."""
    subprocess.run(["bazel", "build", "//processes/..."], cwd=PROJECT_ROOT, check=True)


def binary_path(binary_name):
    """Resolve path to a built bazel binary."""
    process_map = {
        "dombft_replica": "processes/replica/dombft_replica",
        "dombft_proxy": "processes/proxy/dombft_proxy",
        "dombft_client": "processes/client/dombft_client",
        "flutter_replica": "processes/flutter/flutter_replica",
        "flutter_client": "processes/flutter/flutter_client",
    }
    rel = process_map.get(binary_name, binary_name)
    return os.path.join(PROJECT_ROOT, "bazel-bin", rel)


def ensure_keys(resolved):
    """No-op kept for sweep.py compatibility — generate_config handles keys."""
    pass


# --- Output parsing ---

def parse_client_output(output):
    """Parse PERF lines from client log output into metrics dict."""
    result = {}
    latencies = []

    for line in output.splitlines():
        m = re.search(r"PERF event=commit.*latency=(\d+)", line)
        if m:
            latencies.append(int(m.group(1)))

    if latencies:
        latencies.sort()
        n = len(latencies)
        result["committed"] = n
        result["latency_p50"] = latencies[n * 50 // 100] / 1000.0
        result["latency_p95"] = latencies[n * 95 // 100] / 1000.0
        result["latency_p99"] = latencies[n * 99 // 100] / 1000.0
        result["latency_max"] = latencies[-1] / 1000.0

    return result


def print_aggregate_results(client_outputs):
    """Parse all client outputs and print aggregated results."""
    parsed = [parse_client_output(o) for o in client_outputs if o]
    parsed = [p for p in parsed if p]

    if not parsed:
        print("\nWarning: no client results to aggregate")
        return

    total_committed = sum(p.get("committed", 0) for p in parsed)

    print("\n" + "=" * 50)
    print(f"=== System Aggregate ({len(parsed)} clients) ===")
    print("=" * 50)
    print(f"Total committed: {total_committed}")

    if all("latency_p50" in p for p in parsed):
        # Merge all latencies would be better, but avg of percentiles is a rough approximation
        avg_p50 = sum(p["latency_p50"] for p in parsed) / len(parsed)
        avg_p95 = sum(p["latency_p95"] for p in parsed) / len(parsed)
        avg_p99 = sum(p["latency_p99"] for p in parsed) / len(parsed)
        avg_max = sum(p["latency_max"] for p in parsed) / len(parsed)
        print(f"Latency (avg across clients): p50={avg_p50:.2f}ms  p95={avg_p95:.2f}ms  p99={avg_p99:.2f}ms  max={avg_max:.2f}ms")


# --- Local execution ---

def cmd_local(args):
    """Run a protocol benchmark locally."""
    config = load_cluster_config(args.config)
    config = apply_bench_overrides(config,
        send_rate=args.send_rate,
        runtime_secs=args.duration_secs,
        transport=args.transport,
    )
    protocol = args.protocol
    resolved = resolve_local_cluster(config, protocol)

    log_dir = args.log_dir or os.path.join(PROJECT_ROOT, "logs")

    # Generate config + keys in one step (like aspen-bft's generate command)
    config_path = generate_config(resolved, protocol, log_dir)

    if protocol == "dombft":
        _local_dombft(resolved, config_path, log_dir)
    elif protocol == "flutter":
        _local_flutter(resolved, config_path, log_dir)
    else:
        print(f"Unknown protocol: {protocol}", file=sys.stderr)
        sys.exit(1)


def _local_dombft(resolved, config_path, log_dir):
    n_replicas = len(resolved.replicas)
    n_proxies = len(resolved.proxies)
    n_clients = len(resolved.clients)
    runtime = resolved.bench.runtime_secs
    batch_size = resolved.bench.batch_size

    print(f"=== Running DOM-BFT: {n_replicas} replicas, {n_proxies} proxies, {n_clients} clients ===")
    print(f"    transport={resolved.bench.transport} send_rate={resolved.bench.send_rate} runtime={runtime}s")

    subprocess.run(f"killall {' '.join(DOMBFT_BINARIES)} 2>/dev/null", shell=True)
    subprocess.run(f"rm -f {log_dir}/replica* {log_dir}/proxy* {log_dir}/client*", shell=True)

    all_log_files = []
    replicas = []
    for i in range(n_replicas):
        lf = open(os.path.join(log_dir, f"replica{i}.log"), "w")
        all_log_files.append(lf)
        p = subprocess.Popen([
            binary_path("dombft_replica"),
            "-config", config_path, "-replicaId", str(i), "--batchSize", str(batch_size),
        ], stdout=lf, stderr=lf)
        replicas.append(p)
        print(f"  Started replica {i}")

    proxies = []
    for i in range(n_proxies):
        lf = open(os.path.join(log_dir, f"proxy{i}.log"), "w")
        all_log_files.append(lf)
        p = subprocess.Popen([
            binary_path("dombft_proxy"),
            "-config", config_path, "-proxyId", str(i),
        ], stdout=lf, stderr=lf)
        proxies.append(p)
        print(f"  Started proxy {i}")

    time.sleep(3)

    client_procs = []
    for i in range(n_clients):
        lf = open(os.path.join(log_dir, f"client{i}.log"), "w")
        all_log_files.append(lf)
        p = subprocess.Popen([
            binary_path("dombft_client"),
            "-config", config_path, "-clientId", str(i), "-v", "1",
        ], stdout=lf, stderr=lf)
        client_procs.append(p)
        print(f"  Started client {i}")

    wait_timeout = runtime + 30
    print(f"Waiting up to {wait_timeout}s for clients (runtime={runtime}s)...")
    deadline = time.time() + wait_timeout
    for i, p in enumerate(client_procs):
        remaining = max(1, deadline - time.time())
        try:
            p.wait(timeout=remaining)
            print(f"  Client {i} finished")
        except subprocess.TimeoutExpired:
            print(f"  Client {i} timed out, killing...")
            p.kill()
            p.wait()

    time.sleep(2)
    subprocess.run(f"killall -SIGINT {' '.join(DOMBFT_BINARIES)} 2>/dev/null", shell=True)
    time.sleep(2)
    for p in replicas + proxies:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    for f in all_log_files:
        f.close()

    client_outputs = []
    for i in range(n_clients):
        path = os.path.join(log_dir, f"client{i}.log")
        with open(path) as f:
            output = f.read()
        client_outputs.append(output)

    print(f"\nLogs in {log_dir}/")
    print_aggregate_results(client_outputs)


def _local_flutter(resolved, config_path, log_dir):
    n_replicas = len(resolved.replicas)
    n_clients = len(resolved.clients)
    runtime = resolved.bench.runtime_secs
    clock_interval = resolved.bench.clock_broadcast_interval
    base_bet = resolved.bench.initial_bet
    bet_increment = resolved.bench.bet_increment

    print(f"=== Running Flutter: {n_replicas} replicas, {n_clients} clients ===")
    print(f"    transport={resolved.bench.transport} send_rate={resolved.bench.send_rate} runtime={runtime}s")

    subprocess.run(f"killall {' '.join(FLUTTER_BINARIES)} 2>/dev/null", shell=True)
    subprocess.run(f"rm -f {log_dir}/flutter*", shell=True)

    all_log_files = []
    replicas = []
    for i in range(n_replicas):
        lf = open(os.path.join(log_dir, f"flutter_replica{i}.log"), "w")
        all_log_files.append(lf)
        p = subprocess.Popen([
            binary_path("flutter_replica"),
            "-config", config_path, "-replicaId", str(i),
            "-clockBroadcastInterval", str(clock_interval),
        ], stdout=lf, stderr=lf)
        replicas.append(p)

    time.sleep(3)

    client_procs = []
    for i in range(n_clients):
        lf = open(os.path.join(log_dir, f"flutter_client{i}.log"), "w")
        all_log_files.append(lf)
        p = subprocess.Popen([
            binary_path("flutter_client"),
            "-config", config_path, "-clientId", str(i),
            "-baseBetOffset", str(base_bet), "-betIncrement", str(bet_increment),
        ], stdout=lf, stderr=lf)
        client_procs.append(p)

    deadline = time.time() + runtime + 30
    for i, p in enumerate(client_procs):
        remaining = max(1, deadline - time.time())
        try:
            p.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            p.kill()

    subprocess.run(f"killall -SIGINT {' '.join(FLUTTER_BINARIES)} 2>/dev/null", shell=True)
    time.sleep(2)
    for p in replicas:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    for f in all_log_files:
        f.close()

    client_outputs = []
    for i in range(n_clients):
        path = os.path.join(log_dir, f"flutter_client{i}.log")
        with open(path) as f:
            output = f.read()
        client_outputs.append(output)

    print(f"\nLogs in {log_dir}/")
    print_aggregate_results(client_outputs)


# --- Remote execution ---

def cmd_upload(args):
    """Build and upload binaries to remote VMs.

    With --upload-once: upload to first VM, then distribute via internal scp.
    Otherwise: upload from local machine to all VMs in parallel.
    """
    config = load_cluster_config(args.config)
    remote = load_remote({"platform": config.platform, "zone": config.zone, "project": config.project,
                          "user": config.user, "key_file": config.key_file})
    vms = remote_targets(config)

    print("=== Killing existing processes ===")
    for bn in ALL_PROCESS_NAMES:
        remote.kill_process(vms, bn)

    print("=== Building ===")
    bazel_build()

    # Collect binaries that exist
    binaries = []
    for bn in ALL_BINARIES:
        bp = binary_path(bn)
        if os.path.exists(bp):
            binaries.append((bn, bp))

    upload_once = getattr(args, "upload_once", False)
    total = len(binaries) * len(vms)
    done = [0]
    lock = __import__("threading").Lock()

    def progress(bn, vm):
        with lock:
            done[0] += 1
            print(f"  [{done[0]}/{total}] {bn} -> {vm}")

    if upload_once and len(vms) > 1:
        # Upload to first VM, then distribute internally
        pivot = vms[0]

        # Resolve internal IPs for scp between VMs
        if hasattr(remote, "get_all_ips"):
            all_ips = remote.get_all_ips(vms)
        else:
            all_ips = {vm: remote.get_ip(vm) for vm in vms}

        print(f"=== Uploading to {pivot}, then distributing ===")
        for bn, bp in binaries:
            remote.scp_upload(bp, pivot, f"~/{bn}")
            remote.ssh(pivot, f"chmod +x ~/{bn}")
            progress(bn, pivot)

        # Distribute from pivot to others via internal network
        other_vms = [vm for vm in vms if vm != pivot]
        def distribute(bn, vm):
            ip = all_ips[vm]
            remote.ssh(pivot, f"scp -o StrictHostKeyChecking=no ~/{bn} {ip}:~/{bn}")
            remote.ssh(vm, f"chmod +x ~/{bn}")
            progress(bn, vm)

        with ThreadPoolExecutor(max_workers=len(other_vms)) as pool:
            futures = [pool.submit(distribute, bn, vm) for bn, _ in binaries for vm in other_vms]
            for f in as_completed(futures):
                f.result()
    else:
        # Upload from local to all VMs in parallel
        print(f"=== Uploading to {len(vms)} VMs ===")
        def upload_one(bn, bp, vm):
            remote.scp_upload(bp, vm, f"~/{bn}")
            remote.ssh(vm, f"chmod +x ~/{bn}")
            progress(bn, vm)

        with ThreadPoolExecutor(max_workers=min(8, len(vms) * len(binaries))) as pool:
            futures = [pool.submit(upload_one, bn, bp, vm) for bn, bp in binaries for vm in vms]
            for f in as_completed(futures):
                f.result()

    print(f"Upload complete. {total} binaries deployed.")


def cmd_remote(args):
    """Run benchmark on remote VMs."""
    config = load_cluster_config(args.config)
    config = apply_bench_overrides(config,
        send_rate=args.send_rate,
        runtime_secs=args.duration_secs,
        transport=args.transport,
    )
    protocol = args.protocol
    remote = load_remote({"platform": config.platform, "zone": config.zone, "project": config.project,
                          "user": config.user, "key_file": config.key_file})

    vms = remote_targets(config)
    if hasattr(remote, '_discover_all'):
        remote._discover_all(vms)
    if hasattr(remote, 'check_vms_running'):
        print("=== Checking VM status ===")
        remote.check_vms_running(vms)

    resolved = resolve_remote_cluster(config, protocol, remote)

    log_dir = args.log_dir or os.path.join(PROJECT_ROOT, "logs")
    os.makedirs(log_dir, exist_ok=True)

    if protocol == "dombft":
        _remote_run(resolved, config, remote, protocol, log_dir, DOMBFT_BINARIES)
    elif protocol == "flutter":
        _remote_run(resolved, config, remote, protocol, log_dir, FLUTTER_BINARIES)
    else:
        print(f"Unknown protocol: {protocol}", file=sys.stderr)
        sys.exit(1)


def _remote_upload_keys(resolved, config, remote, protocol, log_dir):
    """Generate and upload keys to all VMs. Only needs to run once per cluster."""
    all_vms = remote_targets(config)

    config_path = generate_config(resolved, protocol, log_dir, remote_keys_prefix="keys")

    keys_tar = os.path.join(log_dir, "keys.tar.gz")
    subprocess.run(["tar", "czf", keys_tar, "-C", log_dir, "keys"], check=True)

    print(f"Uploading keys to {len(all_vms)} VMs...")
    def _upload(vm):
        remote.scp_upload(keys_tar, vm, "~/keys.tar.gz")
        remote.ssh(vm, "rm -rf keys && tar xzf keys.tar.gz && rm keys.tar.gz")

    with ThreadPoolExecutor(max_workers=len(all_vms)) as pool:
        futures = {pool.submit(_upload, vm): vm for vm in all_vms}
        for f in as_completed(futures):
            vm = futures[f]
            try:
                f.result()
                print(f"  {vm} keys uploaded")
            except Exception as e:
                print(f"  {vm} failed: {e}")

    os.unlink(keys_tar)


def _remote_upload_config(resolved, config, remote, protocol, log_dir):
    """Generate and upload just the config (keys already on VMs)."""
    all_vms = remote_targets(config)
    config_path = generate_config(resolved, protocol, log_dir, remote_keys_prefix="keys")

    print(f"Uploading config to {len(all_vms)} VMs...")
    with ThreadPoolExecutor(max_workers=len(all_vms)) as pool:
        futures = [pool.submit(remote.scp_upload, config_path, vm, "~/config.yaml") for vm in all_vms]
        for f in as_completed(futures):
            f.result()


def _remote_run(resolved, config, remote, protocol, log_dir, binaries, skip_keys=False):
    """Generic remote run for any protocol."""
    replica_vms = config.replica.vms
    client_vms = config.client.vms
    proxy_vms = config.proxy.vms if config.proxy else []
    all_vms = remote_targets(config)
    runtime = resolved.bench.runtime_secs

    if skip_keys:
        _remote_upload_config(resolved, config, remote, protocol, log_dir)
    else:
        _remote_upload_keys(resolved, config, remote, protocol, log_dir)
        _remote_upload_config(resolved, config, remote, protocol, log_dir)

    # Kill existing
    for bn in binaries:
        remote.kill_process(all_vms, bn)
    time.sleep(1)

    # Start replicas
    replica_procs = []
    for i, vm in enumerate(replica_vms):
        print(f"Starting replica {i} on {vm}...")
        if protocol == "flutter":
            cmd = f"~/flutter_replica -config ~/config.yaml -replicaId {i} -clockBroadcastInterval {resolved.bench.clock_broadcast_interval}"
        else:
            cmd = f"~/dombft_replica -config ~/config.yaml -replicaId {i} --batchSize {resolved.bench.batch_size}"
        p = remote.ssh(vm, f"{cmd} >~/replica_{i}.stdout 2>~/replica_{i}.log", bg=True)
        replica_procs.append((vm, p))

    # Start proxies (dombft only)
    proxy_procs = []
    if protocol == "dombft" and proxy_vms:
        for i, vm in enumerate(proxy_vms):
            print(f"Starting proxy {i} on {vm}...")
            p = remote.ssh(vm, f"~/dombft_proxy -config ~/config.yaml -proxyId {i} >~/proxy_{i}.stdout 2>~/proxy_{i}.log", bg=True)
            proxy_procs.append((vm, p))

    time.sleep(2)

    # Start clients
    client_procs = []
    for i, vm in enumerate(client_vms):
        print(f"Starting client {i} on {vm}...")
        if protocol == "flutter":
            cmd = f"~/flutter_client -config ~/config.yaml -clientId {i} -baseBetOffset {resolved.bench.initial_bet} -betIncrement {resolved.bench.bet_increment}"
        else:
            cmd = f"~/dombft_client -config ~/config.yaml -clientId {i}"
        p = remote.ssh(vm, f"{cmd} >~/client_{i}.stdout 2>~/client_{i}.log", bg=True)
        client_procs.append((vm, p))

    # Wait for clients
    wait_timeout = runtime * 2
    print(f"Waiting up to {wait_timeout}s for clients (runtime={runtime}s)...")
    deadline = time.time() + wait_timeout
    for i, (vm, p) in enumerate(client_procs):
        remaining = max(0, deadline - time.time())
        try:
            p.wait(timeout=remaining)
            print(f"  Client {i} ({vm}) finished")
        except subprocess.TimeoutExpired:
            print(f"  Client {i} ({vm}) timed out")
            p.kill()
            p.wait()

    time.sleep(3)
    for bn in binaries:
        remote.kill_process(all_vms, bn)
    time.sleep(2)

    # Download logs
    print(f"Downloading logs to {log_dir}/...")
    def _dl(role, idx, vm, suffix):
        fname = f"{role}_{idx}.{suffix}"
        try:
            remote.scp_download(vm, f"~/{fname}", os.path.join(log_dir, fname))
        except Exception as e:
            print(f"  Warning: {fname} from {vm}: {e}")

    with ThreadPoolExecutor(max_workers=len(all_vms) * 2) as pool:
        futures = []
        for i, vm in enumerate(replica_vms):
            for s in ("stdout", "log"):
                futures.append(pool.submit(_dl, "replica", i, vm, s))
        for i, vm in enumerate(client_vms):
            for s in ("stdout", "log"):
                futures.append(pool.submit(_dl, "client", i, vm, s))
        for f in as_completed(futures):
            f.result()

    # Print results
    client_outputs = []
    for i, vm in enumerate(client_vms):
        for suffix in ["log", "stdout"]:
            path = os.path.join(log_dir, f"client_{i}.{suffix}")
            try:
                with open(path) as f:
                    output = f.read()
                if output.strip():
                    print(f"\n--- Client {i} ({vm}) [{suffix}] ---")
                    # Only print last 30 lines to avoid flooding
                    lines = output.strip().split("\n")
                    if len(lines) > 30:
                        print(f"  ... ({len(lines) - 30} lines omitted)")
                    for line in lines[-30:]:
                        print(f"  {line}")
                    if suffix == "log":
                        client_outputs.append(output)
            except FileNotFoundError:
                pass

    print_aggregate_results(client_outputs)


# --- Run arbitrary command on all VMs ---

def cmd_run_cmd(args):
    """Run an arbitrary shell command on all VMs."""
    config = load_cluster_config(args.config)
    remote = load_remote({"platform": config.platform, "zone": config.zone, "project": config.project,
                          "user": config.user, "key_file": config.key_file})
    vms = remote_targets(config)
    command = args.cmd
    print(f"Running on {len(vms)} VMs: {command}")
    results = remote.run_on_all(vms, command)
    for vm in vms:
        r = results.get(vm)
        if isinstance(r, Exception):
            print(f"  {vm}: ERROR: {r}")
        elif r is not None and r.stdout.strip():
            print(f"  {vm}: {r.stdout.strip()}")


# --- VM management ---

def cmd_vm_start(args):
    config = load_cluster_config(args.config)
    remote = load_remote({"platform": config.platform, "zone": config.zone, "project": config.project})
    if not isinstance(remote, GCloudRemote):
        print("Error: vm-start only supported for gcloud", file=sys.stderr); sys.exit(1)
    vms = remote_targets(config)
    print(f"Starting VMs: {vms}")
    remote.vm_start(vms)
    print("Scheduling auto-shutdown in 60 minutes...")
    remote.run_on_all(vms, "nohup sudo shutdown -h +60 >/dev/null 2>&1 &")
    print("Done. Run 'sync-clocks' next.")


def cmd_vm_stop(args):
    config = load_cluster_config(args.config)
    remote = load_remote({"platform": config.platform, "zone": config.zone, "project": config.project})
    if not isinstance(remote, GCloudRemote):
        print("Error: vm-stop only supported for gcloud", file=sys.stderr); sys.exit(1)
    vms = remote_targets(config)
    print(f"Stopping VMs: {vms}")
    remote.vm_stop(vms)


def cmd_vm_status(args):
    config = load_cluster_config(args.config)
    remote = load_remote({"platform": config.platform, "zone": config.zone, "project": config.project})
    if not isinstance(remote, GCloudRemote):
        print("Error: vm-status only supported for gcloud", file=sys.stderr); sys.exit(1)
    vms = remote_targets(config)
    statuses = remote.vm_status(vms)
    for vm in vms:
        print(f"  {vm:<30} {statuses.get(vm, 'UNKNOWN')}")


def sync_clocks(remote, vms):
    print(f"Syncing clocks on {len(vms)} VMs...")
    results = remote.run_on_all(vms, "sudo chronyc -a 'burst 4/4' && sleep 10 && sudo chronyc -a makestep && sleep 5 && chronyc sources")
    for vm in vms:
        r = results.get(vm)
        if isinstance(r, Exception):
            print(f"\n--- {vm} --- ERROR: {r}")
        elif r is not None:
            print(f"\n--- {vm} ---")
            print(r.stdout.strip() if r.stdout else "(no output)")


def cmd_sync_clocks(args):
    config = load_cluster_config(args.config)
    remote = load_remote({"platform": config.platform, "zone": config.zone, "project": config.project})
    vms = remote_targets(config)
    sync_clocks(remote, vms)


def cmd_vm_keep_alive(args):
    config = load_cluster_config(args.config)
    remote = load_remote({"platform": config.platform, "zone": config.zone, "project": config.project})
    if not isinstance(remote, GCloudRemote):
        print("Error: vm-keep-alive only supported for gcloud", file=sys.stderr); sys.exit(1)
    vms = remote_targets(config)

    statuses = remote.vm_status(vms)
    stopped = [vm for vm in vms if statuses.get(vm) != "RUNNING"]
    if stopped:
        print(f"Starting stopped VMs: {stopped}")
        remote.vm_start(stopped)
        remote.run_on_all(vms, "nohup sudo shutdown -h +60 >/dev/null 2>&1 &")
        time.sleep(15)
        sync_clocks(remote, vms)
    else:
        print("All VMs already running.")
        remote.run_on_all(vms, "sudo shutdown -c 2>/dev/null; nohup sudo shutdown -h +60 >/dev/null 2>&1 &")

    print(f"Keeping alive {len(vms)} VMs. Heartbeat every 30 min. Ctrl-C to stop.")
    try:
        while True:
            time.sleep(30 * 60)
            print(f"[{time.strftime('%H:%M:%S')}] Resetting shutdown timer...")
            remote.run_on_all(vms, "sudo shutdown -c 2>/dev/null; nohup sudo shutdown -h +60 >/dev/null 2>&1 &")
    except KeyboardInterrupt:
        print("\nStopping VMs...")
        remote.vm_stop(vms)


# --- Main ---

def main():
    parser = argparse.ArgumentParser(description="OooBFT Benchmark Automation")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # local
    lp = subparsers.add_parser("local", help="Run benchmark locally")
    lp.add_argument("--protocol", default="dombft", choices=["dombft", "flutter"])
    lp.add_argument("--config", required=True)
    lp.add_argument("--log-dir", default=None)
    lp.add_argument("--send-rate", type=int, default=None)
    lp.add_argument("--duration-secs", type=int, default=None)
    lp.add_argument("--transport", default=None, help="Override transport (nng, tcp, udp)")

    # remote
    rp = subparsers.add_parser("remote", help="Run benchmark on remote VMs")
    rp.add_argument("--config", required=True)
    rp.add_argument("--protocol", default="dombft", choices=["dombft", "flutter"])
    rp.add_argument("--log-dir", default=None)
    rp.add_argument("--send-rate", type=int, default=None)
    rp.add_argument("--duration-secs", type=int, default=None)
    rp.add_argument("--transport", default=None)

    # upload
    up = subparsers.add_parser("upload", help="Build and upload binaries")
    up.add_argument("--config", required=True)
    up.add_argument("--upload-once", action="store_true", default=False,
                    help="Upload to one VM then distribute internally (faster for large binaries)")

    # Run arbitrary command on all VMs
    cp = subparsers.add_parser("cmd", help="Run a shell command on all VMs")
    cp.add_argument("--config", required=True)
    cp.add_argument("cmd", help="Shell command to run")

    # VM management
    for cmd_name in ["vm-start", "vm-stop", "vm-status", "vm-keep-alive", "sync-clocks"]:
        sp = subparsers.add_parser(cmd_name)
        sp.add_argument("--config", required=True)

    args = parser.parse_args()

    commands = {
        "local": cmd_local,
        "remote": cmd_remote,
        "upload": cmd_upload,
        "cmd": cmd_run_cmd,
        "vm-start": cmd_vm_start,
        "vm-stop": cmd_vm_stop,
        "vm-status": cmd_vm_status,
        "vm-keep-alive": cmd_vm_keep_alive,
        "sync-clocks": cmd_sync_clocks,
    }

    commands[args.command](args)


if __name__ == "__main__":
    main()
