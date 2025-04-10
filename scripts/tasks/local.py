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

    for process in dirs:
        key_dir = dirs[process]
        nkeys = num_processes[process]

        c.run("mkdir -p " + key_dir)

        print(f"Generating {nkeys} keys for {process}")
        for i in range(nkeys):
            key_path = os.path.join(key_dir, process + f"{i}")
            print(key_path)

            if algorithm == "RSA":
                c.run(f"openssl genrsa -outform der -out {key_path}.der {str(keysize)}")
            elif algorithm == "ED25519":
                c.run(f"openssl genpkey -outform der -algorithm ed25519 -out {key_path}.der")
            c.run(f"openssl pkey -outform der -in {key_path}.der -pubout -out {key_path}.pub")


@task
def run(c, config_file="../configs/local.yaml", v=5, prot="dombft",
            batch_size=5,
            num_crashed=0,
            slow_path_freq=0,
            normal_path_freq=0,
            view_change_freq=0,
            commit_local_in_view_change = False):
    def arun(*args, **kwargs):
        return c.run(*args, **kwargs, asynchronous=True, warn=True)

    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    # number of replicas
    n_replicas = len(config["replica"]["ips"])
    n_clients = len(config["client"]["ips"])
    n_proxies = len(config["proxy"]["ips"])
    n_receivers = len(config["receiver"]["ips"])
    client_handles = []
    other_handles = []

    f = n_replicas // 3


    # TODO verbosity
    with c.cd(".."):
        c.run("rm logs/*", warn=True)

        c.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True)
        c.run("mkdir -p logs")
        for id in range(n_replicas):
            swap_arg = ''
            if normal_path_freq != 0 and id < f:
                swap_arg = f'-swapFreq {normal_path_freq}'
            if slow_path_freq != 0 and (id % 2) == 0:
                swap_arg = f'-swapFreq {slow_path_freq}'
            view_change_arg = ''
            if (id % 2) == 0:
                if view_change_freq != 0:
                    view_change_arg = f'-viewChangeFreq {view_change_freq}'
                if commit_local_in_view_change and view_change_freq == 0:
                    view_change_arg += ' -commitLocalInViewChange'
            
            if (id < num_crashed):
                crashed_arg = '-crashed'
            else:
                crashed_arg = ''

            
            cmd = f"./bazel-bin/processes/replica/dombft_replica -prot {prot} -v {v} -config {config_file} -replicaId {id} {crashed_arg} {swap_arg} {view_change_arg} --batchSize {batch_size} &>logs/replica{id}.log"
            hdl = arun(cmd)
            print(cmd)
            other_handles.append(hdl)

        for id in range(n_receivers):
            cmd = f"./bazel-bin/processes/receiver/dombft_receiver -v {v} -config {config_file} -receiverId {id} &>logs/receiver{id}.log"
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
        c.run("killall -SIGINT dombft_client dombft_replica dombft_proxy dombft_receiver", warn=True)

        #  stop other processes and then join
        for hdl in other_handles:
            hdl.join()


@task
def reorder_exp(c, config_file, poisson=False):
    def arun(*args, **kwargs):
        return c.run(*args, **kwargs, asynchronous=True, warn=True)

    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    n_proxies = len(config["proxy"]["ips"])
    n_receivers = len(config["receiver"]["ips"])
    proxy_handles = []
    other_handles = []

    with c.cd(".."):
        c.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True)
        c.run("mkdir -p logs")

        for id in range(n_receivers):
            cmd = (
                f"./bazel-bin/processes/receiver/dombft_receiver -v {5} -config {config_file}"
                + f" -receiverId {id} -skipForwarding  &>logs/receiver{id}.log"
            )
            hdl = arun(cmd)

            other_handles.append(hdl)

        for id in range(n_proxies):
            cmd = (
                f"./bazel-bin/processes/proxy/dombft_proxy -v {5} " +
                f"-config {config_file} -proxyId {id} -genRequests  -duration 10 " +
                f"{'-poisson' if poisson else ''} &>logs/proxy{id}.log"
            )

            hdl = arun(cmd)
            proxy_handles.append(hdl)

    try:
        # join on the proxy processes, which should end
        for hdl in proxy_handles:
            hdl.join()

        print("Proxies done, waiting 5 sec for receivers to finish...")
        time.sleep(5)

    finally:

        c.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True)

        # kill these processes and then join
        for hdl in other_handles:
            hdl.runner.kill()
            hdl.join()
