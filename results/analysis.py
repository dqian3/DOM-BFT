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


# In[3]:


events = []
with open(sys.argv[1]) as f:
    for line in f:
        events.append(parse_line(line))


# In[4]:


events = sorted(events, key=lambda x: x['time'])

start_time = events[0]['time'] + datetime.timedelta(seconds=15)
end_time = events[-1]['time'] - datetime.timedelta(seconds=15)

commits = list(filter(lambda x: x["event"] == "commit", events))

n_clients = max(events, key=lambda x: x["client_id"] if "client_id" in x else 0)["client_id"] + 1



# In[7]:


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






