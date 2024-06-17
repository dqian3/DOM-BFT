
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
        c.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True)

        # kill these processes and then join
        for hdl in other_handles:
            hdl.runner.kill()
            hdl.join()


def get_gcloud_ext_ips(c):
    # parse gcloud CLI to get internalIP -> externalIP mapping
    gcloud_output = c.run("gcloud compute instances list").stdout[1:].splitlines()
    gcloud_output = map(lambda s : s.split(), gcloud_output)
    ext_ips = {
        # internal ip and external ip are last 2 tokens in each line
        line[-3] : line[-2]
        for line in gcloud_output
    }

    return ext_ips


def get_gcloud_process_group(config, ext_ips):
    int_ips = set()
    for process in config:
        if process == "transport": continue
        int_ips |= set([ip for ip in config[process]["ips"]])

    # TODO transfer keys and configs
    ips = []
    for ip in int_ips: # TODO non local receivers?
        ips.append(ext_ips[ip])
    
    group = ThreadingGroup(
        *ips
    )
    return group


@task 
def gcloud_clockwork(c, config_file="../configs/remote.yaml", install=False):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    int_ips = config["proxy"]["ips"] + config["receiver"]["ips"] 

    # Only need to do this on proxies and receivers
    group = ThreadingGroup(
        *(ext_ips[ip] for ip in int_ips)
    )

    if install:
        group.put("../ttcs-agent_1.3.0_amd64.deb")
        group.run("sudo dpkg -i ttcs-agent_1.3.0_amd64.deb")

    with open("../ttcs-agent.cfg") as ttcs_file:
        ttcs_template = ttcs_file.read()

    ip = int_ips[0]
    ttcs_config = ttcs_template.format(ip, ip, 10, "false")
    Connection(ext_ips[ip]).run(f"echo '{ttcs_config}' | sudo tee /etc/opt/ttcs/ttcs-agent.cfg")

    for ip in int_ips[1:]:
        ttcs_config = ttcs_template.format(ip, ip, 1, "true")
        Connection(ext_ips[ip]).run(f"echo '{ttcs_config}'| sudo tee /etc/opt/ttcs/ttcs-agent.cfg")


    group.run("sudo systemctl stop ntp", warn=True)
    group.run("sudo systemctl disable ntp", warn=True)
    group.run("sudo systemctl stop systemd-timesyncd", warn=True)
    group.run("sudo systemctl disable systemd-timesyncd", warn=True)

    group.run("sudo systemctl enable ttcs-agent", warn=True)

    if install:
        group.run("sudo systemctl start ttcs-agent")
    else:
        group.run("sudo systemctl restart ttcs-agent")


@task
def gcloud_build(c, config_file="../configs/remote.yaml"):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = get_gcloud_process_group(config, ext_ips)
    group.put(config_file)

    print("Cloning/building repo...")
    group.run("git clone https://github.com/dqian3/DOM-BFT", warn=True)
    group.run("cd DOM-BFT && git pull && bazel build //processes/...")

    group.run("cp ./DOM-BFT/bazel-bin/processes/replica/dombft_replica ~") 
    group.run("cp ./DOM-BFT/bazel-bin/processes/receiver/dombft_receiver ~") 
    group.run("cp ./DOM-BFT/bazel-bin/processes/proxy/dombft_proxy ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/client/dombft_client ~") 


@task 
def gcloud_copy_keys(c, config_file="../configs/remote.yaml"):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)


    ext_ips = get_gcloud_ext_ips(c)
    group = get_gcloud_process_group(config, ext_ips)
    group.put(config_file)
    
    print("Copying keys over...")
    for process in ["client", "replica", "receiver", "proxy"]:
        group.run(f"mkdir -p keys/{process}")
        for filename in os.listdir(f"../keys/{process}"):
            group.put(os.path.join(f"../keys/{process}", filename), f"keys/{process}")


@task 
def gcloud_copy_bin(c, config_file="../configs/remote.yaml"):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = get_gcloud_process_group(config, ext_ips)

    replicas = config["replica"]["ips"]
    receivers = config["receiver"]["ips"]
    proxies = config["proxy"]["ips"]
    clients = config["client"]["ips"]

    replicas = SerialGroup(*[ext_ips[ip] for ip in replicas])
    receivers = SerialGroup(*[ext_ips[ip] for ip in receivers])
    proxies = SerialGroup(*[ext_ips[ip] for ip in proxies])
    clients = SerialGroup(*[ext_ips[ip] for ip in clients])


    print("Copying binaries over...")
    group.run("rm dombft_*", warn=True)
    
    replicas.put("../bazel-bin/processes/replica/dombft_replica")
    print("Copied replica")

    receivers.put("../bazel-bin/processes/receiver/dombft_receiver")
    print("Copied receiver")

    proxies.put("../bazel-bin/processes/proxy/dombft_proxy")
    print("Copied proxy")

    clients.put("../bazel-bin/processes/client/dombft_client")
    print("Copied client")


@task 
def gcloud_run(c, config_file="../configs/remote.yaml"):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)


    ext_ips = get_gcloud_ext_ips(c)
    group = get_gcloud_process_group(config, ext_ips)
    group.put(config_file)
    group.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True, hide="both")

    # ips of each process 
    replicas = config["replica"]["ips"]
    receivers = config["receiver"]["ips"]
    proxies = config["proxy"]["ips"]
    clients = config["client"]["ips"]

    replica_path = "./dombft_replica" 
    receiver_path = "./dombft_receiver" 
    proxy_path = "./dombft_proxy" 
    client_path = "./dombft_client" 

    replicas = [ext_ips[ip] for ip in replicas]
    receivers = [ext_ips[ip] for ip in receivers]
    proxies = [ext_ips[ip] for ip in proxies]
    clients = [ext_ips[ip] for ip in clients]

    remote_config_file = os.path.basename(config_file)

    client_handles = []
    other_handles = []

    def local_log_arun(logfile, ip):
        def arun(*args, **kwargs):
            log = open(logfile, "w")
            conn = Connection(ip)

            # print(f"Running {args}")
            print(f"Running {args} on {ip}" )
            return conn.run(*args, **kwargs, asynchronous=True, warn=True, out_stream=log)
            
        return arun

    c.run("mkdir -p ../logs")
    print("Starting replicas")
    for id, ip in enumerate(replicas):
        arun = local_log_arun(f"../logs/replica{id}.log", ip)
        hdl = arun(f"{replica_path} -v {5} -config {remote_config_file} -replicaId {id} 2>&1")
        other_handles.append(hdl)
            
    print("Starting receivers")
    for id, ip in enumerate(receivers):
        arun = local_log_arun(f"../logs/receiver{id}.log", ip)
        hdl = arun(f"{receiver_path} -v {5} -config {remote_config_file} -receiverId {id} 2>&1")
        other_handles.append(hdl)

    print("Starting proxies")
    for id, ip in enumerate(proxies):
        arun = local_log_arun(f"../logs/proxy{id}.log", ip)
        hdl = arun(f"{proxy_path} -v {5} -config {remote_config_file} -proxyId {id} 2>&1")
        other_handles.append(hdl)

    time.sleep(5)

    print("Starting clients")
    for id, ip in enumerate(clients):
        arun = local_log_arun(f"../logs/client{id}.log", ip)
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
