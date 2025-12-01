import os
import time

import yaml
from fabric import Connection, ThreadingGroup, SerialGroup
from invoke import task
import invoke

# =================================================
#               Helper functions
# =================================================


def resolve(c, ip, platform):
    if platform == "gcloud":
        return f"ubuntu@{ip}"
    elif platform == "cloudlab":
        return f"root@{ip}"
    else:
        raise ValueError(f"Unknown platform {platform}")


def arun_on(ip, logfile, timeout, profile=False):
    def perf_prefix(prof_file):
        return f"env LD_PRELOAD='/home/dqian/libprofiler.so' CPUPROFILE={prof_file} CPUPROFILE_FREQUENCY={10} "

    # Previous versions of this function logged directly to local files using a command like
    #   log = open(logfile, "w")
    #     ...
    #   conn.run(command + " 2>&1", **kwargs, asynchronous=True, warn=True, out_stream=log)
    # This was changed to use the remote machine's filesystem to avoid issues with this outstream flushing

    def arun(command, **kwargs):
        conn = Connection(ip)

        if profile:
            command = perf_prefix(os.path.splitext(logfile)[0] + ".prof") + command

        print(f"Running {command} on {ip}, logging on remote machine {logfile}")
        return conn.run(
            command + f" &>{logfile}",
            **kwargs,
            asynchronous=True,
            timeout=timeout,
            warn=True,
        )

    return arun


def get_logs(c, ips, log_prefix):
    for id, ip in enumerate(ips):
        conn = Connection(ip)
        print(f"Getting {log_prefix}{id}.log.gz")
        conn.run(f"rm -f {log_prefix}{id}.log.gz", hide=True)
        conn.run(
            f"gzip {log_prefix}{id}.log", hide=True, warn=True
        )  # original files are too large
        conn.get(f"{log_prefix}{id}.log.gz", "../logs/")


def get_process_ips(config_file, resolve):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    replicas = [resolve(ip) for ip in config["replica"]["ips"]]
    proxies = [resolve(ip) for ip in config["proxy"]["ips"]]
    clients = [resolve(ip) for ip in config["client"]["ips"]]
    return replicas, proxies, clients


def get_flutter_process_ips(config_file, resolve):
    """Get process IPs for Flutter (no proxies)"""
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    replicas = [resolve(ip) for ip in config["replica"]["ips"]]
    clients = [resolve(ip) for ip in config["client"]["ips"]]
    return replicas, clients


def get_all_ips(config_file, resolve):
    return set(
        ip for ip_list in get_process_ips(config_file, resolve) for ip in ip_list
    )


# =================================================
#             Main experiment tasks
# =================================================


@task
def logs(c, config_file="../configs/remote-prod.yaml", resolve=lambda x: x):
    # ips of each process
    c.run("rm -f ../logs/*.log")

    replicas, proxies, clients = get_process_ips(config_file, resolve)

    get_logs(c, replicas, "replica")
    get_logs(c, proxies, "proxy")
    get_logs(c, clients, "client")


@task
def run(
    # Invoke context
    c,
    # Necessary args to run dombft
    config_file="../config/remote-prod.yaml",  # Path to the config file on the local machine
    # function to resolve addresses in the config file to accesible addresses
    resolve=lambda x: x,
    # Options for logging/output to fetch
    v=5,
    dom_logs=False,
    profile=False,
    filter_client_logs=False,
    # Optional args to modify the dombft experiments
    prot="dombft",
    batch_size=1,
    num_crashed=0,
    slow_path_freq=0,
    normal_path_freq=0,
    view_change_freq=0,
    drop_checkpoint_freq=0,
    commit_local_in_view_change=False,
    max_view_change=0,
):
    config_file = os.path.abspath(config_file)

    with open(config_file) as f:
        cfg = yaml.load(f, Loader=yaml.Loader)

    runtime = cfg["client"]["runtimeSeconds"]
    print(f"Running for {runtime} seconds")

    replicas, proxies, clients = get_process_ips(config_file, resolve)

    replica_path = "./dombft_replica"
    proxy_path = "./dombft_proxy"
    client_path = "./dombft_client"

    if cfg.get("resiliency") == "5f+1":
        f = len(replicas) // 5
    else:
        # Otherwise, we assume 3f+1 resiliency
        f = len(replicas) // 3

    # Clear out ssh keys to avoid issues with authentication
    #     -> Hao: this actually causes issues:Could not open a connection to your authentication agent. Why?
    # c.run("ssh-add -D")

    group = ThreadingGroup(*get_all_ips(config_file, resolve))

    # Kill previous runs
    group.run(
        "killall dombft_proxy dombft_replica dombft_client",
        warn=True,
        hide="both",
    )

    # Give replicas the config file
    group.put(config_file)
    group.put("filter_logs.py")

    remote_config_file = os.path.basename(config_file)

    client_handles = []
    other_handles = []

    c.run("mkdir -p ../logs")

    # Run a dummy command with pty to log a session so that the machine doesn't shutdown from being inactive
    group.run("echo ''", pty=True)

    print("Starting replicas")
    for id, ip in enumerate(replicas):
        swap_arg = ""
        if normal_path_freq != 0 and id < 1:
            swap_arg = f"-swapFreq {normal_path_freq}"
        if slow_path_freq != 0 and (id % 2) == 0:
            swap_arg = f"-swapFreq {slow_path_freq}"

        drop_checkpoint_arg = ""
        if id == 0 and drop_checkpoint_freq != 0:
            drop_checkpoint_arg = f"-checkpointDropFreq {drop_checkpoint_freq}"
        view_change_arg = ""

        if (id % 2) == 0:
            if view_change_freq != 0:
                view_change_arg = f"-viewChangeFreq {view_change_freq}"
            if commit_local_in_view_change and id == 0:
                view_change_arg += " -commitLocalInViewChange"
            if max_view_change != 0:
                view_change_arg += f" -viewChangeNum {max_view_change}"

        if len(replicas) - id - 1 < num_crashed:
            crashed_arg = "-crashed"
        else:
            crashed_arg = ""

        batch_size_arg = f"--batchSize {batch_size}"

        arun = arun_on(ip, f"replica{id}.log", timeout=10 + runtime, profile=profile)
        hdl = arun(
            f"{replica_path} -prot {prot} -v {v} -config {remote_config_file} -replicaId {id} {batch_size_arg} {crashed_arg} {swap_arg} {view_change_arg} {drop_checkpoint_arg}"
        )
        other_handles.append(hdl)

    print("Starting proxies")
    for id, ip in enumerate(proxies):
        arun = arun_on(ip, f"proxy{id}.log", timeout=10 + runtime, profile=profile)
        hdl = arun(f"{proxy_path} -v {v} -config {remote_config_file} -proxyId {id}")
        other_handles.append(hdl)

    time.sleep(2)

    print("Starting clients")
    for id, ip in enumerate(clients):
        arun = arun_on(ip, f"client{id}.log", timeout=10 + runtime, profile=profile)

        if filter_client_logs:
            suffix = " 2>&1 | python3 -u filter_logs.py "
        else:
            suffix = " "

        hdl = arun(
            f"{client_path} -v {v} -config {remote_config_file} -clientId {id} {suffix}"
        )
        client_handles.append(hdl)

    try:
        # join on the client processes, which should end
        for hdl in client_handles:
            hdl.join()

    finally:
        print("Clients done, waiting for other processes to finish...")

        # kill these processes and then join
        group.run(
            "killall -SIGINT dombft_replica dombft_proxy",
            warn=True,
            hide="both",
        )

        for hdl in other_handles:
            try:
                hdl.join()
            except invoke.exceptions.CommandTimedOut as e:
                print(f"{e}")

        c.run("rm -f ../logs/*", warn=True)

        get_logs(c, replicas, "replica")
        get_logs(c, clients, "client")

        if dom_logs:
            get_logs(c, proxies, "proxy")


@task
def flutter(
    c,
    config_file="../configs/flutter_remote.yaml",
    resolve=lambda x: x,
    v=5,
    profile=False,
    filter_client_logs=False,
):
    """Run Flutter protocol experiments"""
    config_file = os.path.abspath(config_file)

    with open(config_file) as f:
        cfg = yaml.load(f, Loader=yaml.Loader)

    runtime = cfg["client"]["runtimeSeconds"]
    print(f"Running Flutter for {runtime} seconds")

    replicas, clients = get_flutter_process_ips(config_file, resolve)

    replica_path = "./flutter_replica"
    client_path = "./flutter_client"

    # Flutter requires 5f+1 replicas
    f = len(replicas) // 5

    # Extract Flutter-specific config parameters
    clock_broadcast_interval = cfg.get("replica", {}).get(
        "clockBroadcastInterval", 50000
    )
    base_bet_offset = cfg.get("client", {}).get("initialBet", 100000)
    bet_increment = cfg.get("client", {}).get("betIncrement", 100000)

    print(
        f"Flutter config: clockBroadcastInterval={clock_broadcast_interval}us, "
        f"initialBet={base_bet_offset}us, betIncrement={bet_increment}us"
    )

    # Get all unique IPs for Flutter (replicas + clients)
    all_ips = set(replicas) | set(clients)
    group = ThreadingGroup(*all_ips)

    # Kill previous runs
    group.run(
        "killall flutter_replica flutter_client",
        warn=True,
        hide="both",
    )

    # Give machines the config file
    group.put(config_file)
    group.put("filter_logs.py")

    remote_config_file = os.path.basename(config_file)

    client_handles = []
    replica_handles = []

    c.run("mkdir -p ../logs")

    # Run a dummy command with pty to log a session so that the machine doesn't shutdown from being inactive
    group.run("echo ''", pty=True)

    print("Starting Flutter replicas")
    for id, ip in enumerate(replicas):
        arun = arun_on(
            ip, f"flutter_replica{id}.log", timeout=10 + runtime, profile=profile
        )
        hdl = arun(
            f"{replica_path} -v {v} -config {remote_config_file} -replicaId {id} "
            f"-clockBroadcastInterval {clock_broadcast_interval}"
        )
        replica_handles.append(hdl)

    time.sleep(3)

    print("Starting Flutter clients")
    for id, ip in enumerate(clients):
        arun = arun_on(
            ip, f"flutter_client{id}.log", timeout=10 + runtime, profile=profile
        )

        if filter_client_logs:
            suffix = " 2>&1 | python3 -u filter_logs.py "
        else:
            suffix = " "

        hdl = arun(
            f"{client_path} -v {v} -config {remote_config_file} -clientId {id} "
            f"-baseBetOffset {base_bet_offset} -betIncrement {bet_increment}{suffix}"
        )
        client_handles.append(hdl)

    try:
        # Join on the client processes, which should end
        for hdl in client_handles:
            hdl.join()

    finally:
        print("Clients done, waiting for replicas to finish...")

        # Kill replicas and then join
        group.run(
            "killall -SIGINT flutter_replica flutter_client",
            warn=True,
            hide="both",
        )

        for hdl in replica_handles:
            try:
                hdl.join()
            except invoke.exceptions.CommandTimedOut as e:
                print(f"{e}")

        c.run("rm -f ../logs/*", warn=True)

        get_logs(c, replicas, "flutter_replica")
        get_logs(c, clients, "flutter_client")

        c.run("gzip -d ../logs/*", warn=True)


# =================================================
#             Multiple experiment tasks
# =================================================
@task
def run_rates(
    c,
    config_file="../configs/flutter-remote.yaml",
    resolve=lambda x: x,
    v=1,
    prot="dombft",
    batch_size=1,
):
    try:
        with open(config_file, "r") as cfg_file:
            original_contents = cfg_file.read()
            cfg = yaml.load(original_contents, Loader=yaml.Loader)

        # Fast path short
        cfg["client"]["sendMode"] = "sendRate"
        nClients = len(cfg["client"]["ips"])

        for send_rate in [200, 400, 500, 600, 700, 800]:
            cfg["client"]["sendRate"] = send_rate
            cfg["client"]["maxInFlight"] = int(send_rate * 0.2)

            yaml.dump(cfg, open(config_file, "w"))
            flutter(
                c,
                config_file=config_file,
                resolve=resolve,
                v=v,
            )
            c.run(
                f"cat ../logs/flutter_replica*.log ../logs/flutter_client*.log | grep PERF >flutter_sr{nClients * send_rate}.out"
            )

    finally:
        with open(config_file, "w") as cfg_file:
            cfg_file.write(original_contents)


# =================================================
#             Other tasks
# =================================================


@task
def copy_keys(c, config_file="../configs/remote-prod.yaml", resolve=lambda x: x):
    group = ThreadingGroup(*get_all_ips(config_file, resolve))
    group.run("rm -rf keys/*")

    print("Copying keys over...")
    for process in ["client", "replica", "proxy"]:
        group.run(f"mkdir -p keys/{process}")
        for filename in os.listdir(f"../keys/{process}"):
            group.put(os.path.join(f"../keys/{process}", filename), f"keys/{process}")


@task
def copy_flutter_bin(
    c,
    config_file="../configs/flutter_remote.yaml",
    upload_once=False,
    resolve=lambda x: x,
):
    """Copy Flutter binaries to remote machines"""
    replicas, clients = get_flutter_process_ips(config_file, resolve)
    all_ips = set(replicas) | set(clients)
    group = ThreadingGroup(*all_ips)

    group.run(
        "killall flutter_replica flutter_client",
        warn=True,
        hide="both",
    )

    if upload_once:
        # Upload to one machine, then use scp to distribute to others
        print(f"Copying binaries over to one machine {clients[0]}")
        start_time = time.time()
        conn = Connection(clients[0])

        conn.run("chmod +w flutter_*", warn=True)
        conn.put("../bazel-bin/processes/flutter/flutter_replica")
        conn.put("../bazel-bin/processes/flutter/flutter_client")
        conn.run("chmod +x flutter_*", warn=True)

        print(f"Copying took {time.time() - start_time:.0f}s")

        print("Copying to other machines")
        start_time = time.time()

        # Get internal IPs for scp
        replicas_internal, clients_internal = get_flutter_process_ips(
            config_file, lambda x: x
        )

        for ip in replicas_internal:
            print(f"Copying flutter_replica to {ip}")
            conn.run(
                f"scp -o StrictHostKeyChecking=no flutter_replica {ip}:", warn=True
            )

        for ip in set(clients_internal[1:]):  # Skip own
            print(f"Copying flutter_client to {ip}")
            conn.run(f"scp -o StrictHostKeyChecking=no flutter_client {ip}:", warn=True)

        print(f"Copying to other machines took {time.time() - start_time:.0f}s")

    else:
        # Use SerialGroup to copy to all machines individually
        replicas_group = SerialGroup(*replicas)
        clients_group = SerialGroup(*clients)

        group.run("rm -f flutter_*", warn=True)
        group.run("chmod +w flutter_*", warn=True)

        print("Copying Flutter binaries...")

        replicas_group.put("../bazel-bin/processes/flutter/flutter_replica")
        print("Copied flutter_replica")

        clients_group.put("../bazel-bin/processes/flutter/flutter_client")
        print("Copied flutter_client")

        group.run("chmod +x flutter_*", warn=True)


@task
def copy_bin(
    c, config_file="../configs/remote-prod.yaml", upload_once=False, resolve=lambda x: x
):
    replicas, proxies, clients = get_process_ips(config_file, resolve)
    group = ThreadingGroup(*get_all_ips(config_file, resolve))

    group.run(
        "killall dombft_proxy dombft_replica dombft_client",
        warn=True,
        hide="both",
    )
    if upload_once:
        # TODO try and check to see if binaries are stale
        print(f"Copying binaries over to one machine {clients[0]}")
        start_time = time.time()
        conn = Connection(clients[0])

        conn.run("chmod +w dombft_*", warn=True)
        conn.put("../bazel-bin/processes/replica/dombft_replica")
        conn.put("../bazel-bin/processes/proxy/dombft_proxy")
        conn.put("../bazel-bin/processes/client/dombft_client")
        conn.run("chmod +w dombft_*", warn=True)

        print(f"Copying took {time.time() - start_time:.0f}s")

        print(f"Copying to other machines")
        start_time = time.time()

        replicas, proxies, clients = get_process_ips(config_file, lambda x: x)

        for ip in replicas:
            print(f"Copying dombft_replica to {ip}")
            conn.run(f"scp -o StrictHostKeyChecking=no dombft_replica {ip}:", warn=True)

        for ip in proxies:
            print(f"Copying dombft_proxy to {ip}")
            conn.run(f"scp -o StrictHostKeyChecking=no dombft_proxy {ip}:", warn=True)

        for ip in set(clients[1:]):  # Skip own
            print(f"Copying dombft_client to {ip}")
            conn.run(f"scp -o StrictHostKeyChecking=no dombft_client {ip}:", warn=True)

        print(f"Copying to other machines took {time.time() - start_time:.0f}s")

    else:
        # Otherwise, just copy to all machines

        replicas = SerialGroup(*replicas)
        proxies = SerialGroup(*proxies)
        clients = SerialGroup(*clients)

        group.run("rm -f dombft_*", warn=True)
        group.run("chmod +w dombft_*", warn=True)

        print("Copying binaries over...")

        replicas.put("../bazel-bin/processes/replica/dombft_replica")
        print("Copied replica")

        proxies.put("../bazel-bin/processes/proxy/dombft_proxy")
        print("Copied proxy")

        clients.put("../bazel-bin/processes/client/dombft_client")
        print("Copied client")

        group.run("chmod +w dombft_*", warn=True)


@task
def build(
    c, config_file="../configs/remote-prod.yaml", resolve=lambda x: x, setup=False
):
    group = ThreadingGroup(*get_all_ips(config_file, resolve))
    group.put(config_file)

    if setup:
        group.put("setup.sh")
        group.run("chmod +x ./setup.sh && sudo ./setup.sh")

    print("Cloning/building repo...")

    group.run("git clone https://github.com/dqian3/DOM-BFT", warn=True)
    group.run("cd DOM-BFT && git checkout main && bazel build //processes/...")

    group.run("rm ~/dombft_*", warn=True)
    group.run("cp ./DOM-BFT/bazel-bin/processes/replica/dombft_replica ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/proxy/dombft_proxy ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/client/dombft_client ~")


@task
def cmd(c, cmd, config_file="../configs/remote-prod.yaml", resolve=lambda x: x):
    ips = get_all_ips(config_file, resolve)
    print(ips)

    group = ThreadingGroup(*ips)
    group.run(cmd)


@task
def copy(
    c,
    file,
    remote_path=None,
    config_file="../configs/remote-prod.yaml",
    resolve=lambda x: x,
):
    group = ThreadingGroup(*get_all_ips(config_file, resolve))
    group.put(file, remote_path)


@task
def setup_clockwork(
    c, config_file="../configs/remote-prod.yaml", install=False, resolve=lambda x: x
):
    _, receivers, proxies, _ = get_process_ips(config_file, resolve)
    _, receivers_int, proxies_int, _ = get_process_ips(config_file, resolve=lambda x: x)

    addrs = set(receivers + proxies)
    addrs_int = set(receivers_int + proxies_int)

    # Only need to do this on proxies and receivers
    group = ThreadingGroup(*addrs)

    if install:
        group.put("../ttcs-agent_1.3.0_amd64.deb")
        group.run("sudo dpkg -i ttcs-agent_1.3.0_amd64.deb")

    with open("../ttcs-agent.cfg") as ttcs_file:
        ttcs_template = ttcs_file.read()

    ip = addrs[0]
    ip_int = addrs_int[0]
    ttcs_config = ttcs_template.format(ip_int, ip_int, 10, "false")
    Connection(ip).run(f"echo '{ttcs_config}' | sudo tee /etc/opt/ttcs/ttcs-agent.cfg")

    for ip, ip_int in zip(addrs[1:], addrs_int[1:]):
        ttcs_config = ttcs_template.format(ip_int, ip_int, 1, "true")
        Connection(ip).run(
            f"echo '{ttcs_config}'| sudo tee /etc/opt/ttcs/ttcs-agent.cfg"
        )

    group.run("sudo systemctl stop ntp", warn=True)
    group.run("sudo systemctl disable ntp", warn=True)
    group.run("sudo systemctl stop systemd-timesyncd", warn=True)
    group.run("sudo systemctl disable systemd-timesyncd", warn=True)

    group.run("sudo systemctl enable ttcs-agent", warn=True)

    if install:
        group.run("sudo systemctl start ttcs-agent")
    else:
        group.run("sudo systemctl restart ttcs-agent")
