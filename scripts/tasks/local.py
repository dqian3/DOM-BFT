import os
import time

import yaml
from invoke import task


@task
def genkeys(c, config_file, algorithm="ED25519", keysize=2048):
    if algorithm not in ["ED25519", "RSA"]:
        raise ValueError(f"Invalid algorithm {algorithm}")

    # Parse config to get dirs and number of processes for each
    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    # number of keys we need to generate
    num_processes = {}
    # dir that keys should be put in for each process
    dirs = {}

    for p in config:
        if "ips" not in config[p]:
            continue

        pconfig = config[p]
        num_processes[p] = len(pconfig["ips"])
        dirs[p] = pconfig["keysDir"]

    with c.cd(".."):
        for process in dirs:
            key_dir = dirs[process]
            nkeys = num_processes[process]

            c.run("mkdir -p " + key_dir)

            print(f"Generating {nkeys} keys for {process}")
            for i in range(nkeys):
                key_path = os.path.join(key_dir, process + f"{i}")
                print(key_path)

                if algorithm == "RSA":
                    c.run(
                        f"openssl genrsa -outform der -out {key_path}.der {str(keysize)}"
                    )
                elif algorithm == "ED25519":
                    c.run(
                        f"openssl genpkey -outform der -algorithm ed25519 -out {key_path}.der"
                    )
                c.run(
                    f"openssl pkey -outform der -in {key_path}.der -pubout -out {key_path}.pub"
                )


@task
def run(
    c,
    config_file="../configs/local.yaml",
    v=5,
    prot="dombft",
    batch_size=5,
    analyze_client_logs=False,
    num_crashed=0,
    slow_path_freq=0,
    normal_path_freq=0,
    view_change_freq=0,
    commit_local_in_view_change=False,
):
    def arun(*args, **kwargs):
        return c.run(*args, **kwargs, asynchronous=True, warn=True)

    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    # number of replicas (unified process handles both replica and receiver)
    n_replicas = len(config["replica"]["ips"])
    n_clients = len(config["client"]["ips"])
    n_proxies = len(config["proxy"]["ips"])
    client_handles = []
    other_handles = []

    # TODO verbosity
    with c.cd(".."):
        c.run("rm logs/*", warn=True)

        c.run(
            "killall dombft_proxy dombft_client dombft_replica",
            warn=True,
        )
        c.run("mkdir -p logs")
        for id in range(n_replicas):
            swap_arg = ""
            if normal_path_freq != 0 and id < 1:
                swap_arg = f"-swapFreq {normal_path_freq}"
            if slow_path_freq != 0 and (id % 2) == 0:
                swap_arg = f"-swapFreq {slow_path_freq}"
            view_change_arg = ""
            if (id % 2) == 0:
                if view_change_freq != 0:
                    view_change_arg = f"-viewChangeFreq {view_change_freq}"
                if commit_local_in_view_change and view_change_freq == 0:
                    view_change_arg += " -commitLocalInViewChange"

            if id < num_crashed:
                crashed_arg = "-crashed"
            else:
                crashed_arg = ""

            cmd = f"./bazel-bin/processes/replica/dombft_replica -prot {prot} -v {v} -config {config_file} -replicaId {id} {crashed_arg} {swap_arg} {view_change_arg} --batchSize {batch_size} &>logs/replica{id}.log"
            hdl = arun(cmd)
            print(cmd)
            other_handles.append(hdl)

        for id in range(n_proxies):
            cmd = f"./bazel-bin/processes/proxy/dombft_proxy -v {v} -config {config_file} -proxyId {id} &>logs/proxy{id}.log"
            hdl = arun(cmd)
            print(cmd)

            other_handles.append(hdl)
        time.sleep(3)

        for id in range(n_clients):
            cmd = f"./bazel-bin/processes/client/dombft_client -v {v} -config {config_file} -clientId {id} &>logs/client{id}.log"
            hdl = arun(cmd)
            print(cmd)

            client_handles.append(hdl)

    try:
        # join on the client processes, which should end
        for hdl in client_handles:
            hdl.join()

    finally:
        print("Clients done, waiting for other processes to finish...")
        c.run(
            "killall -SIGINT dombft_client dombft_proxy dombft_replica",
            warn=True,
        )

        #  stop other processes and then join
        for hdl in other_handles:
            hdl.join()

        if analyze_client_logs:
            print("Analyzing client logs...")
            # Run analyze_client.py on each client log
            for id in range(n_clients):
                c.run(
                    f"python3 scripts/analysis/analyze_client.py logs/client{id}.log -o logs/client{id}.json",
                    warn=True
                )

            # Run aggregate_results.py to combine all client analyses
            c.run(
                "python3 scripts/analysis/aggregate_results.py logs/aggregate.json logs/",
                warn=True
            )
            print("Client log analysis complete. Results in logs/aggregate.json")
