# Basic log parsing

import re
import datetime
import sys
import numpy as np


def parse_time(line):
    match = re.search(f"([0-9]*:[0-9]*:[0-9]*.[0-9]*)", line)
    time_str = match.group(1)
    return datetime.datetime.strptime(time_str, "%H:%M:%S.%f")


def parse_tags(line):
    tags = {}
    line = line.split("PERF ")[1]

    for token in line.split():
        [tag, value] = token.split("=")
        tags[tag] = value
        try:
            tags[tag] = int(value)
        except ValueError as verr:
            pass

    return tags


def parse_line(line):
    time = parse_time(line)
    tags = parse_tags(line)

    tags["time"] = time
    return tags


if __name__ == "__main__":
    events = []
    with open(sys.argv[1]) as f:
        for line in f:
            if "DUMP" in line:
                continue
            events.append(parse_line(line))

    events = sorted(events, key=lambda x: x["time"])

    start_time = events[0]["time"] + datetime.timedelta(seconds=10)
    end_time = events[-1]["time"] - datetime.timedelta(seconds=10)

    commits = list(filter(lambda x: x["event"] == "commit", events))

    n_clients = (
        max(events, key=lambda x: x["client_id"] if "client_id" in x else 0)[
            "client_id"
        ]
        + 1
    )

    # Get general stats
    event = list(
        filter(lambda x: x["time"] > start_time and x["time"] < end_time, events)
    )

    commits = list(
        filter(lambda x: x["time"] > start_time and x["time"] < end_time, commits)
    )

    events = list(
        filter(lambda x: x["time"] > start_time and x["time"] < end_time, events)
    )

    runtime = (commits[-1]["time"] - commits[0]["time"]).total_seconds()
    print(f"Runtime: {runtime:.3f} s")
    print("number of clients: ", n_clients)
    print(f"Total Throughput: {len(commits) / runtime:.0f} req/s")

    latencies = np.array([c["latency"] for c in commits])
    print(f"Num commits: {len(commits)}")
    print(f"p5 latency: {np.percentile(latencies, 5):.0f} us")
    print(f"Average latency: {np.mean(latencies):.0f} us")
    print(f"p95 latency: {np.percentile(latencies, 95):.0f} us")
    print(f"p99 latency: {np.percentile(latencies, 99):.0f} us")

    fast = list(filter(lambda x: x["path"] == "fast" and x["queued"] == 0, commits))
    normal = list(filter(lambda x: x["path"] == "fast" and x["queued"], commits))
    slow = list(filter(lambda x: x["path"] == "slow", commits))

    print("Fast path:")
    print(f"\tNum commits: {len(fast)} {len(fast) / len(commits)}")
    if len(fast) > 0:
        print(
            f"\tAverage latency: {sum(c['latency'] for c in fast) / len(fast):.0f} us"
        )

        # Break down fast path latency by client
        print("\n\tFast path breakdown by client:")
        client_counts = []
        client_avg_latencies = []
        client_p50_latencies = []
        client_p95_latencies = []
        client_p99_latencies = []

        for client_id in range(n_clients):
            client_fast = [c for c in fast if c.get("client_id") == client_id]
            if len(client_fast) > 0:
                client_latencies = np.array([c["latency"] for c in client_fast])
                client_counts.append(len(client_fast))
                client_avg_latencies.append(int(np.mean(client_latencies)))
                client_p50_latencies.append(int(np.percentile(client_latencies, 50)))
                client_p95_latencies.append(int(np.percentile(client_latencies, 95)))
                client_p99_latencies.append(int(np.percentile(client_latencies, 99)))
            else:
                client_counts.append(0)
                client_avg_latencies.append(0)
                client_p50_latencies.append(0)
                client_p95_latencies.append(0)
                client_p99_latencies.append(0)

        print(f"\t  Counts:  {client_counts}")
        print(f"\t  Avg:     {client_avg_latencies}")
        print(f"\t  p50:     {client_p50_latencies}")
        print(f"\t  p95:     {client_p95_latencies}")
        print(f"\t  p99:     {client_p99_latencies}")

    print("Fast Queued path:")
    print(f"\tNum commits: {len(normal)}  {len(normal) / len(commits)}")
    if len(normal) > 0:
        print(
            f"\tAverage latency: {sum(c['latency'] for c in normal) / len(normal):.0f} us"
        )

    print("Slow path:")
    print(f"\tNum commits: {len(slow)}  {len(slow) / len(commits)}")
    if len(slow) > 0:
        print(
            f"\tAverage latency: {sum(c['latency'] for c in slow) / len(slow):.0f} us"
        )

    # Find the highest and lowest rounds (number of repair rounds in window)
    min_round = min(c["round"] for c in commits if "round" in c)
    max_round = max(c["round"] for c in commits if "round" in c)
    print("Number of repair rounds: ", max_round - min_round)

    n_align = len(list(e for e in events if e["event"] == "align"))
    print("Number of alignments", n_align)

    # Break down alignments by replica
    alignments = [e for e in events if e["event"] == "align"]
    if len(alignments) > 0:
        # Get number of replicas
        n_replicas = max(e.get("replicaId", 0) for e in events if "replicaId" in e) + 1

        print("\n  Alignment breakdown by replica:")
        replica_align_counts = []
        for replica_id in range(n_replicas):
            replica_aligns = [e for e in alignments if e.get("replicaId") == replica_id]
            replica_align_counts.append(len(replica_aligns))

        print(f"    Counts: {replica_align_counts}")

    # Analyse percent of time in the fast path
    last_fast_time = None
    non_fast_seconds = 0
    non_fast_periods = []
    last_commit = None

    start_time = None

    for tags in commits:
        if start_time is None:
            start_time = tags["time"]
            # Take the nearest minute
            start_time = start_time.replace(second=0, microsecond=0)
            last_fast_time = tags["time"]
            last_commit = tags["path"]

        if tags["path"] == "fast" and tags["queued"] == 0:
            if last_commit != "fast":
                # TODO Instead of just summing non fast path periods, actually output them so we can get a timeline
                # Ideally each period should also include the number of types of commits
                # So for each period here, we should output the counts of each non fast path commit
                non_fast_seconds += (tags["time"] - last_fast_time).total_seconds()
            last_fast_time = tags["time"]

        last_commit = tags["path"]
        if "queued" in tags and tags["queued"] == 1:
            last_commit = "fast_queued"

    runtime = (tags["time"] - start_time).total_seconds()
    print(f"Percent time in fast path: {(runtime - non_fast_seconds) / runtime:0.3f}")
