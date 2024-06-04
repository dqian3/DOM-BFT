
from fabric import Connection, ThreadingGroup, SerialGroup
from invoke import task
import yaml
import os
import time

# TODO we can process output of these here instead of in the terminal

@task
def local(c, config_file):
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

    # TODO verbosity
    with c.cd(".."):
        c.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True)
        c.run("mkdir -p logs")
        for id in range(n_replicas):
            hdl = arun(f"./bazel-bin/processes/replica/dombft_replica -v {5} -config {config_file} -replicaId {id} &>logs/replica{id}.log")
            other_handles.append(hdl)
            
        for id in range(n_receivers):
            hdl = arun(f"./bazel-bin/processes/receiver/dombft_receiver -v {5} -config {config_file} -receiverId {id} &>logs/receiver{id}.log")
            other_handles.append(hdl)

        for id in range(n_proxies):
            hdl = arun(f"./bazel-bin/processes/proxy/dombft_proxy -v {5} -config {config_file} -proxyId {id} &>logs/proxy{id}.log")
            other_handles.append(hdl)

        for id in range(n_clients):
            hdl = arun(f"./bazel-bin/processes/client/dombft_client -v {5} -config {config_file} -clientId {id} &>logs/client{id}.log")
            client_handles.append(hdl)

    try:
        # join on the client processes, which should end
        for hdl in client_handles:
            hdl.join()

    finally:
        # kill these processes and then join
        for hdl in other_handles:
            hdl.runner.kill()
            hdl.join()


@task 
def gcloud(c, config_file, compile=False, copy_bin=False, copy_keys=False):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    # ips of each process 
    replicas = config["replica"]["ips"]
    receivers = config["receiver"]["ips"]
    proxies = config["proxy"]["ips"]
    clients = config["client"]["ips"]

    # parse gcloud CLI to get internalIP -> externalIP mapping
    gcloud_output = c.run("gcloud compute instances list").stdout[1:].splitlines()
    gcloud_output = map(lambda s : s.split(), gcloud_output)
    ext_ips = {
        # internal ip and external ip are last 2 tokens in each line
        line[-3] : line[-2]
        for line in gcloud_output
    }


    # TODO transfer keys and configs
    ips = []
    for ip in clients + replicas + proxies: # TODO non local receivers?
        ips.append(ext_ips[ip])
    
    group = ThreadingGroup(
        *ips
    )


    group.put(config_file)
    
    # Copy keys over

    if copy_keys:
        print("Copying keys over...")
        for process in ["client", "replica", "receiver", "proxy"]:
            group.run(f"mkdir -p keys/{process}")
            for filename in os.listdir(f"../keys/{process}"):
                group.put(os.path.join(f"../keys/{process}", filename), f"keys/{process}")


    if compile:
        print("Cloning/building repo...")
        group.run("git clone https://github.com/dqian3/DOM-BFT", warn=True)
        group.run("cd DOM-BFT && git pull && bazel build //processes/...")

        group.run("cp ./DOM-BFT/bazel-bin/processes/replica/dombft_replica ~") 
        group.run("cp ./DOM-BFT/bazel-bin/processes/receiver/dombft_receiver ~") 
        group.run("cp ./DOM-BFT/bazel-bin/processes/proxy/dombft_proxy ~")
        group.run("cp ./DOM-BFT/bazel-bin/processes/client/dombft_client ~") 


    if copy_bin:        
        print("Copying binaries over...")
        
        group.run("rm dombft_*")
        group.put("../bazel-bin/processes/replica/dombft_replica")
        group.put("../bazel-bin/processes/receiver/dombft_receiver")
        group.put("../bazel-bin/processes/proxy/dombft_proxy")
        group.put("../bazel-bin/processes/client/dombft_client")

    replica_path = "./dombft_replica" 
    receiver_path = "./dombft_receiver" 
    proxy_path = "./dombft_proxy" 
    client_path = "./dombft_client" 

    clients = [ext_ips[ip] for ip in clients]
    replicas = [ext_ips[ip] for ip in replicas]
    receivers = [ext_ips[ip] for ip in receivers]
    proxies = [ext_ips[ip] for ip in proxies]

    remote_config_file = os.path.basename(config_file)

    client_handles = []
    other_handles = []

    group.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True, hide="both")


    def local_log_arun(logfile, ip):
        def arun(*args, **kwargs):
            log = open(logfile, "w")
            conn = Connection(ip)

            # print(f"Running {args}")
            print(f"Running {args} on {ip}" )
            return conn.run(*args, **kwargs, asynchronous=True, warn=True, out_stream=log)
            
        return arun

    c.run("mkdir -p logs")
    print("Starting replicas")
    for id, ip in enumerate(replicas):
        arun = local_log_arun(f"logs/replica{id}.log", ip)
        hdl = arun(f"{replica_path} -v {5} -config {remote_config_file} -replicaId {id} 2>&1")
        other_handles.append(hdl)
            
    print("Starting receivers")
    for id, ip in enumerate(receivers):
        arun = local_log_arun(f"logs/receiver{id}.log", ip)
        hdl = arun(f"{receiver_path} -v {5} -config {remote_config_file} -receiverId {id} 2>&1")
        other_handles.append(hdl)

    print("Starting proxies")
    for id, ip in enumerate(proxies):
        arun = local_log_arun(f"logs/proxy{id}.log", ip)
        hdl = arun(f"{proxy_path} -v {5} -config {remote_config_file} -proxyId {id} 2>&1")
        other_handles.append(hdl)

    time.sleep(5)

    print("Starting clients")
    for id, ip in enumerate(clients):
        arun = local_log_arun(f"logs/client{id}.log", ip)
        hdl = arun(f"{client_path} -v {5} -config {remote_config_file} -clientId {id} 2>&1")
        client_handles.append(hdl)

    try:
        # join on the client processes, which should end
        for hdl in client_handles:
            hdl.join()

    finally:
        # kill these processes and then join
        for hdl in other_handles:
            hdl.runner.kill()
            hdl.join()
