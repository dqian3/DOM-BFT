import os
import time

import invoke
import yaml
from fabric import Connection, SerialGroup, ThreadingGroup
from invoke import task

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
            cmd = f"./bazel-bin/processes/replica/dombft_replica -v {5} -config {config_file} -replicaId {id} &>logs/replica{id}.log;"
            hdl = arun(cmd)
            print(cmd)
            other_handles.append(hdl)

        for id in range(n_receivers):
            cmd = f"./bazel-bin/processes/receiver/dombft_receiver -v {5} -config {config_file} -receiverId {id} &>logs/receiver{id}.log"
            hdl = arun(cmd)
            print(cmd)

            other_handles.append(hdl)

        for id in range(n_proxies):
            cmd = f"./bazel-bin/processes/proxy/dombft_proxy -v {5} -config {config_file} -proxyId {id} &>logs/proxy{id}.log"
            hdl = arun(cmd)
            print(cmd)

            other_handles.append(hdl)

        for id in range(n_clients):
            cmd = f"./bazel-bin/processes/client/dombft_client -v {5} -config {config_file} -clientId {id} &>logs/client{id}.log"
            hdl = arun(cmd)
            print(cmd)

            client_handles.append(hdl)

    try:
        # join on the client processes, which should end
        for hdl in client_handles:
            hdl.join()

    finally:
        c.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True)

        # kill these processes and then join
        # TODO(Hao) there should be a graceful way to do it..
        for hdl in other_handles:
            hdl.runner.kill()
            hdl.join()


@task
def local_reorder_exp(c, config_file, poisson=False):
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


def get_gcloud_ext_ips(c):
    # parse gcloud CLI to get internalIP -> externalIP mapping    
    gcloud_output = c.run("gcloud compute instances list").stdout[1:].splitlines()
    gcloud_output = map(lambda s: s.split(), gcloud_output)
    ext_ips = {
        # internal ip and external ip are last 2 tokens in each line
        line[-3]: line[-2]
        for line in gcloud_output
    }
    return ext_ips

def get_gcloud_process_group(config, ext_ips):
    int_ips = set()
    for process in config:
        if process == "transport" or process == "app": continue
        int_ips |= set([ip for ip in config[process]["ips"]])

    ips = []
    for ip in int_ips:  # TODO non local receivers?
        if (ip not in ext_ips): continue
        ips.append(ext_ips[ip])

    group = ThreadingGroup(
        *ips, user="yy4108"
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
        *(ext_ips[ip] for ip in int_ips), user="yy4108"
    )

    if install:
        group.put("../ttcs-agent_1.3.0_amd64.deb")
        group.run("sudo dpkg -i ttcs-agent_1.3.0_amd64.deb")

    with open("../ttcs-agent.cfg") as ttcs_file:
        ttcs_template = ttcs_file.read()

    ip = int_ips[0]
    ttcs_config = ttcs_template.format(ip, ip, 10, "false")
    Connection(ext_ips[ip], user="yy4108").run(f"echo '{ttcs_config}' | sudo tee /etc/opt/ttcs/ttcs-agent.cfg")

    for ip in int_ips[1:]:
        ttcs_config = ttcs_template.format(ip, ip, 1, "true")
        Connection(ext_ips[ip], user="yy4108").run(f"echo '{ttcs_config}'| sudo tee /etc/opt/ttcs/ttcs-agent.cfg")

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
def gcloud_build(c, config_file="../configs/remote.yaml", setup=False):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = get_gcloud_process_group(config, ext_ips)
    group.put(config_file)

    if setup:
        group.put("setup.sh")
        group.run("chmod +x ./setup.sh && sudo ./setup.sh")

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

    replicas = SerialGroup(*[ext_ips[ip] for ip in replicas], user="yy4108")
    receivers = SerialGroup(*[ext_ips[ip] for ip in receivers], user="yy4108")
    proxies = SerialGroup(*[ext_ips[ip] for ip in proxies], user="yy4108")
    clients = SerialGroup(*[ext_ips[ip] for ip in clients], user="yy4108")

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


def get_gcloud_process_ips(c, filter):
    gcloud_output = c.run(f"gcloud compute instances list | grep {filter}").stdout.splitlines()
    gcloud_output = map(lambda s : s.split(), gcloud_output)

    return [
        # internal ip is 3rd last token in line
        line[-3]
        for line in gcloud_output
    ]


@task
def gcloud_create_prod(c, config_template="../configs/remote.yaml"):
    # This is probably better, but can't be across zones:
    # https://cloud.google.com/compute/docs/instances/multiple/create-in-bulk

    create_vm_template = """
gcloud compute instances create {} \
    --project=mythic-veld-419517 \
    --zone={} \
    --machine-type=t2d-standard-16 \
    --network-interface=network-tier=PREMIUM,stack-type=IPV4_ONLY,subnet=default \
    --create-disk=auto-delete=yes,boot=yes,device-name=prod-replica1,image=projects/debian-cloud/global/images/debian-12-bookworm-v20240709,mode=rw,size=20,type=projects/mythic-veld-419517/zones/us-west1-c/diskTypes/pd-ssd 
"""

    zones = ["us-west1-c", "us-west4-c", "us-east1-c", "us-east5-c"]

    config_file = os.path.abspath(config_template)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)


    n = len(config["replica"]["ips"])
    n_proxies = len(config["proxy"]["ips"])
    n_clients = len(config["client"]["ips"])

    for i in range(n):
        zone = zones[i % len(zones)]
        c.run(create_vm_template.format(f"prod-replica{i}", zone))

    for i in range(n_proxies):
        zone = zones[i % len(zones)]
        c.run(create_vm_template.format(f"prod-proxy{i}", zone))
    

    for i in range(n_clients):
        zone = zones[i % len(zones)]
        c.run(create_vm_template.format(f"prod-client{i}", zone))


    config["replica"]["ips"] = get_gcloud_process_ips(c, "prod-replica")
    config["receiver"]["ips"] = get_gcloud_process_ips(c, "prod-replica")
    config["proxy"]["ips"] = get_gcloud_process_ips(c, "prod-proxy")
    config["client"]["ips"] = get_gcloud_process_ips(c, "prod-client")

    filename, ext = os.path.splitext(config_template)
    yaml.dump(config, open(filename + "-prod" + ext, "w"))



def arun_on(ip, logfile, local_log=False):

    if local_log:
        logfile = os.path.join("../logs/", logfile)
        def arun_local_log(command, **kwargs):
            log = open(logfile, "w")
            conn = Connection(ip, user="yy4108")

            print(f"Running {args} on {ip}, logging to local {logfile}")
            return conn.run(command + " 2>&1", **kwargs, asynchronous=True, warn=True, out_stream=log)

        return arun_local_log

    else:
        def arun(command, **kwargs):
            conn = Connection(ip, user="yy4108")

            print(f"Running {command} on {ip}, logging on remote machine {logfile}" )
            return conn.run(command + f" &>{logfile}", **kwargs, asynchronous=True, warn=True)
        return arun


def get_logs(c, ips, log_prefix):
    for id, ip in enumerate(ips):
        conn = Connection(ip, user="yy4108")
        print(f"Getting {log_prefix}{id}.log")
        conn.get(f"{log_prefix}{id}.log", "../logs/")
 

@task
def gcloud_logs(c, config_file="../configs/remote.yaml"):

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = get_gcloud_process_group(config, ext_ips)

    # ips of each process 
    replicas = config["replica"]["ips"]
    receivers = config["receiver"]["ips"]
    proxies = config["proxy"]["ips"]
    clients = config["client"]["ips"]

    replicas = [ext_ips[ip] for ip in replicas]
    receivers = [ext_ips[ip] for ip in receivers]
    proxies = [ext_ips[ip] for ip in proxies]
    clients = [ext_ips[ip] for ip in clients]

    get_logs(c, replicas, "replica")
    get_logs(c, receivers, "receiver")
    get_logs(c, proxies, "proxy")
    get_logs(c, clients, "client")


# local_log_file is good for debugging, but will slow the system down at high throughputs
@task
def gcloud_run(c, config_file="../configs/remote.yaml",
               local_log=False):
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


    c.run("mkdir -p ../logs")
    print("Starting replicas")
    for id, ip in enumerate(replicas):
        arun = arun_on(ip, f"replica{id}.log", local_log=local_log)
        hdl = arun(f"{replica_path} -v {5} -config {remote_config_file} -replicaId {id}")
        other_handles.append(hdl)

    print("Starting receivers")
    for id, ip in enumerate(receivers):
        arun = arun_on(ip, f"receiver{id}.log", local_log=local_log)
        hdl = arun(f"{receiver_path} -v {5} -config {remote_config_file} -receiverId {id}")
        other_handles.append(hdl)

    print("Starting proxies")
    for id, ip in enumerate(proxies):
        arun = arun_on(ip, f"proxy{id}.log", local_log=local_log)
        hdl = arun(f"{proxy_path} -v {5} -config {remote_config_file} -proxyId {id}")
        other_handles.append(hdl)

    time.sleep(5)

    print("Starting clients")
    for id, ip in enumerate(clients):
        arun = arun_on(ip, f"client{id}.log", local_log=local_log)
        hdl = arun(f"{client_path} -v {5} -config {remote_config_file} -clientId {id}")
        client_handles.append(hdl)

    try:
        # join on the client processes, which should end
        for hdl in client_handles:
            hdl.join()

    finally:
        # kill these processes and then join
        for hdl in other_handles:
            hdl.runner.kill()

        for hdl in other_handles:
            hdl.join()


        print("Clients done, waiting 5 sec for other processes to finish...")
        time.sleep(5)


        if not local_log:
            get_logs(c, clients, "client")
            get_logs(c, replicas, "replica")
            get_logs(c, receivers, "receiver")
            get_logs(c, proxies, "proxy")



@task
def gcloud_reorder_exp(c, config_file="../configs/remote.yaml", 
                    poisson=False, ignore_deadlines=False, duration=20, rate=100,
                    local_log=False):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = get_gcloud_process_group(config, ext_ips)
    group.put(config_file)
    group.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True, hide="both")

    # ips of each process 
    receivers = config["receiver"]["ips"]
    proxies = config["proxy"]["ips"]

    receiver_path = "./dombft_receiver"
    proxy_path = "./dombft_proxy"

    receivers = [ext_ips[ip] for ip in receivers]
    proxies = [ext_ips[ip] for ip in proxies]

    remote_config_file = os.path.basename(config_file)

    proxy_handles = []
    other_handles = []

    print("Starting receivers")
    for id, ip in enumerate(receivers):
        arun = arun_on(ip, f"receiver{id}.log", local_log=local_log)
        hdl = arun(
            f"{receiver_path}  -v {1} -receiverId {id} -config {remote_config_file}" 
            + f" -skipForwarding {'-ignoreDeadlines' if ignore_deadlines else ''}"
        )

        other_handles.append(hdl)

    time.sleep(5)

    print("Starting proxies")
    for id, ip in enumerate(proxies):
        arun = arun_on(ip, f"proxy{id}.log", local_log=local_log)
        hdl = arun(f"{proxy_path} -v {5} -config {remote_config_file} -proxyId {id} -genRequests " +
                f"{'-poisson' if poisson else ''} -duration {duration} -rate {rate}")
        
        proxy_handles.append(hdl)

    try:
        # join on the client processes, which should end
        for hdl in proxy_handles:
            hdl.join()
            
        print("Proxies done, waiting 5 sec for receivers to finish...")
        time.sleep(5)

    finally:
        # kill these processes and then join
        for hdl in other_handles:
            hdl.runner.kill()
            hdl.join()

        if not local_log:
            get_logs(c, receivers, "receiver")
            get_logs(c, proxies, "proxy")

