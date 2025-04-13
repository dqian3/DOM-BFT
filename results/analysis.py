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
            if "DUMP" in line: continue
            events.append(parse_line(line))
    
    
    
    events = sorted(events, key=lambda x: x['time'])
    
    start_time = events[0]['time'] + datetime.timedelta(seconds=30)
    end_time = events[-1]['time'] - datetime.timedelta(seconds=30)
    
    commits = list(filter(lambda x: x["event"] == "commit", events))
    
    n_clients = max(events, key=lambda x: x["client_id"] if "client_id" in x else 0)["client_id"] + 1
    
    
    
    
    # Get general stats
    commits = list(filter(lambda x: x['time'] > start_time and x['time'] < end_time, commits))
    
    
    runtime = (commits[-1]["time"] - commits[0]["time"]).total_seconds()
    print(f"Runtime: {runtime:.3f} s")
    print("number of clients: ", n_clients)
    print(f"Total Throughput: {len(commits) / runtime:.0f} req/s")
    
    latencies = np.array([c['latency'] for c in commits])
    print(f"Num commits: {len(commits)}")
    print(f"Average latency: {np.mean(latencies):.0f} us")
    print(f"p95 latency: {np.percentile(latencies, 95):.0f} us")
    print(f"p99 latency: {np.percentile(latencies, 99):.0f} us")
    
    
    fast = list(filter(lambda x: x["path"] == "fast", commits))
    normal = list(filter(lambda x: x["path"] == "normal", commits))
    slow = list(filter(lambda x: x["path"] == "slow", commits))
    
    print("Fast path:")
    print(f"\tNum commits: {len(fast)}")
    if len(fast) > 0:
        print(f"\tAverage latency: {sum(c['latency'] for c in fast) / len(fast):.0f} us")
    
    
    print("Normal path:")
    print(f"\tNum commits: {len(normal)}")
    if len(normal) > 0:
        print(f"\tAverage latency: {sum(c['latency'] for c in normal) / len(normal):.0f} us")
    
    print("Slow path:")
    print(f"\tNum commits: {len(slow)}")
    if len(slow) > 0:
        print(f"\tAverage latency: {sum(c['latency'] for c in slow) / len(slow):.0f} us")
    


    # Peak throughput window


    import numpy as np

    w_size = 10 #s
    resolution = 1 #s

    end = (commits[-1]["time"] - start_time).total_seconds()

    for c in commits:
        c["t"] = (c["time"] - start_time).total_seconds()

    w_start = 0
    i = 0
    j = 0

    commit_counts = []
    max_commits = 0
    max_window = None

    while w_start + w_size < end:
        while (commits[i]["t"] < w_start):
            i += 1
        while (commits[j]["t"] <= w_start + w_size):
            j += 1

        if j - i > max_commits:
            max_window = (i, j)
            max_commits = j - i

        w_start += resolution


    window_latencies = np.array([c['latency'] for c in commits[i:j]])
    print(f"Finding best 30s window")
 
    print(f"Max throughput over window of ten seconds: {max_commits / 10}")
    print(f"Average latency in window: {np.mean(window_latencies):.0f} us")
 


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

        if tags["path"] == "fast":
            if last_commit != "fast":
                # TODO Instead of just summing non fast path periods, actually output them so we can get a timeline
                # Ideally each period should also include the number of types of commits
                # So for each period here, we should output the counts of each non fast path commit
                non_fast_seconds += (tags["time"] - last_fast_time).total_seconds()
            last_fast_time = tags["time"]

        last_commit = tags["path"]


    runtime = (tags["time"] - start_time).total_seconds()
    print(f"Percent time in fast path: {(runtime - non_fast_seconds)/ runtime:0.3f}")




