import sys
import re
import datetime
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


# Read log lines from stdin, ignoring any that do not start with PERF
def read_log_lines():
    for line in sys.stdin:
        if "PERF" in line:
            yield line


def parse_client():

    last_fast_time = None
    non_fast_seconds = 0
    last_commit = None

    start_time = None
    interval = 0

    # TODO add some logging for the first normal path commit to tell when a 
    # do some post processing on all the logs (including replica) to figure
    # out how long recovery takes

    commits = {
        "fast": [],
        "slow": [],
        "normal": [],
        "missed": [],
    }
    
    total_latencies = {
        "fast": 0,
        "slow": 0,
        "normal": 0,
        "missed": 0,
    }
    

    counts = {
        "fast": 0,
        "slow": 0,
        "normal": 0,
        "missed": 0,
    }
    


    for line in read_log_lines():
        tags = parse_line(line)

        if "event" not in tags or tags["event"] != "commit":
            continue

        if start_time is None:
            start_time = tags["time"]
            # Take the nearest minute
            start_time = start_time.replace(second=0, microsecond=0)

            last_commit = tags["path"]

        path = tags["path"]
        commits[path].append(tags)
        total_latencies[path] += tags["latency"]
        counts[path] += 1

        if path == "fast":
            if last_commit != "fast":
                non_fast_seconds += (tags["time"] - last_fast_time).total_seconds()
            last_fast_time = tags["time"]

        last_commit = tags["path"]

        if tags["time"] > start_time + interval * datetime.timedelta(minutes=1):
            print(f"Minute ending {(start_time + interval * datetime.timedelta(minutes=1)).strftime('%H:%M')} summary:")
            # TODO more stats

            total_commits = sum(len(commits[path]) for path in commits)
            print(f"\tnum_commits={total_commits} ")
            
            for path in commits: 
                print(f"\t{path}_num_commits = {len(commits[path])}")
                latencies = np.array([c["latency"] for c in commits[path]])
                
                if (len(commits[path]) > 0):
                    print(f"\t{path}_avg_latency = {np.mean(latencies):.0f} us")
                    print(f"\t{path}_p95_latency = {np.percentile(latencies, 95):.0f} us")
                    print(f"\t{path}_p99_latency = {np.percentile(latencies, 99):.0f} us")

                commits[path] = []

            interval += 1


    runtime = (tags["time"] - start_time).total_seconds()


    total_commits = sum(counts[path] for path in counts)

    print(f"Percent commits in fast path: {counts['fast']/total_commits:0.3f}")

    print(f"Percent time in fast path: {(runtime - non_fast_seconds)/ runtime:0.3f}")
    for path in total_latencies:
        print(f"Number of {path} commits: {counts[path]}, average latency: {total_latencies[path] / max(1, counts[path]):.0f}")






parse_client()

    