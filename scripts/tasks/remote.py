
import os
import time

import yaml
from fabric import Connection, ThreadingGroup, SerialGroup
from invoke import task

#=================================================
#               Helper functions
#=================================================

def resolve(c, ip, platform):
    if platform == "gcloud":
        return f"ubuntu@{ip}"
    elif platform == "cloudlab":
        return f"root@{ip}"
    else:
        raise ValueError(f"Unknown platform {platform}")


def arun_on(ip, logfile, profile=False):
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
            command = perf_prefix(os.path.splitext(logfile)[0] + '.prof') + command

        print(f"Running {command} on {ip}, logging on remote machine {logfile}" )
        return conn.run(command + f" &>{logfile}", **kwargs, asynchronous=True, warn=True)
    
    return arun


def get_logs(c, ips, log_prefix):
    for id, ip in enumerate(ips):
        conn = Connection(ip)
        print(f"Getting {log_prefix}{id}.log")
        conn.get(f"{log_prefix}{id}.log", "../logs/")


def get_process_ips(config_file, resolve):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    replicas = [resolve(ip) for ip in config["replica"]["ips"]]
    receivers = [resolve(ip) for ip in config["receiver"]["ips"]]
    proxies = [resolve(ip) for ip in config["proxy"]["ips"]]
    clients = [resolve(ip) for ip in config["client"]["ips"]]
    return replicas, receivers, proxies, clients



def get_all_ips(config_file, resolve):
    return [
        ip 
        for ip_list in get_process_ips(config_file, resolve)
        for ip in ip_list 
    ]

#=================================================
#             Main experiment tasks
#=================================================


@task
def logs(c,  config_file="../configs/remote-prod.yaml", resolve=lambda x: x,):
    # ips of each process 
    replicas, receivers, proxies, clients = get_process_ips(config_file, resolve)

    get_logs(c, replicas, "replica")
    get_logs(c, receivers, "receiver")
    get_logs(c, proxies, "proxy")
    get_logs(c, clients, "client")


@task
def run(       
    # Invoke context
    c,


    # Necessary args to run dombft 
    config_file="../config/remote-prod.yaml",            # Path to the config file on the local machine

    # function to resolve addresses in the config file to accesible addresses
    resolve=lambda x: x,
    prot="dombft",

    # Options for logging/output to fetch
    v=5,

    dom_logs=False,
    profile=False,

    # Optional args to modify the dombft experiments
    num_crashed=0,
    slow_path_freq=0,
    normal_path_freq=0,
    view_change_freq=0,
    drop_checkpoint_freq=0,
    commit_local_in_view_change=False,
    max_view_change = 0,
):
    config_file = os.path.abspath(config_file)

    replicas, receivers, proxies, clients = get_process_ips(config_file, resolve)

    replica_path = "./dombft_replica"
    receiver_path = "./dombft_receiver"
    proxy_path = "./dombft_proxy"
    client_path = "./dombft_client"

    f = len(replicas) // 3

    group = ThreadingGroup(*replicas, *receivers, *proxies, *clients)

    # Kill previous runs
    group.run("killall dombft_proxy dombft_receiver dombft_client", warn=True, hide="both")

    # Give replicas the config file
    group.put(config_file)
    remote_config_file = os.path.basename(config_file)

    client_handles = []
    other_handles = []

    c.run("mkdir -p ../logs")
    print("Starting replicas")
    for id, ip in enumerate(replicas):
        swap_arg = ''
        if normal_path_freq != 0 and id < f:
            swap_arg = f'-swapFreq {normal_path_freq}'
        if slow_path_freq != 0 and (id % 2) == 0:
            swap_arg = f'-swapFreq {slow_path_freq}'

        drop_checkpoint_arg = ''
        if id ==0 and drop_checkpoint_freq != 0:
            drop_checkpoint_arg = f'-checkpointDropFreq {drop_checkpoint_freq}'
        view_change_arg = ''

        if (id % 2) == 0:
            if view_change_freq != 0:
                view_change_arg = f'-viewChangeFreq {view_change_freq}'
            if commit_local_in_view_change and id==0:
                view_change_arg += ' -commitLocalInViewChange'
            if max_view_change != 0:
                view_change_arg += f' -viewChangeNum {max_view_change}'

        if (id < num_crashed):
            crashed_arg = '-crashed'
        else:
            crashed_arg = ''


        arun = arun_on(ip, f"replica{id}.log", profile=profile)
        hdl = arun(f"{replica_path} -prot {prot} -v {v} -config {remote_config_file} -replicaId {id} {crashed_arg} {swap_arg} {view_change_arg} {drop_checkpoint_arg}")
        other_handles.append(hdl)

    print("Starting receivers")
    for id, ip in enumerate(receivers):
        arun = arun_on(ip, f"receiver{id}.log", profile=profile)
        hdl = arun(f"{receiver_path} -v {v} -config {remote_config_file} -receiverId {id}")
        other_handles.append(hdl)

    print("Starting proxies")
    for id, ip in enumerate(proxies):
        arun = arun_on(ip, f"proxy{id}.log", profile=profile )
        hdl = arun(f"{proxy_path} -v {v} -config {remote_config_file} -proxyId {id}")
        other_handles.append(hdl)

    time.sleep(2)

    print("Starting clients")
    for id, ip in enumerate(clients):
        arun = arun_on(ip, f"client{id}.log", profile=profile)
        hdl = arun(f"{client_path} -v {v} -config {remote_config_file} -clientId {id}")
        client_handles.append(hdl)

    try:
        # join on the client processes, which should end
        for hdl in client_handles:
            hdl.join()

    finally:
        print("Clients done, waiting for other processes to finish...")

        # kill these processes and then join
        group.run("killall -SIGINT dombft_replica dombft_proxy dombft_receiver", warn=True, hide="both")

        for hdl in other_handles:
            hdl.join()

        c.run("rm -f ../logs/*.log")

        get_logs(c, clients, "client")
        get_logs(c, replicas, "replica")

        if dom_logs:
            get_logs(c, receivers, "receiver")
            get_logs(c, proxies, "proxy")


@task
def reorder_exp(c, config_file="../configs/remote-prod.yaml", resolve=lambda x: x,
                    poisson=False, ignore_deadlines=False, duration=20, rate=100,
                    local_log=False):
    
    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    group = ThreadingGroup(*get_all_ips(config_file, resolve))
    group.put(config_file)
    group.run("killall dombft_replica dombft_proxy dombft_receiver dombft_client", warn=True, hide="both")

    # ips of each process 
    receivers = config["receiver"]["ips"]
    proxies = config["proxy"]["ips"]

    receiver_path = "./dombft_receiver"
    proxy_path = "./dombft_proxy"

    _, receivers, proxies, _ = get_process_ips(config_file, resolve)
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

        try:
            for hdl in other_handles:
                hdl.runner.send_interrupt(KeyboardInterrupt())
                hdl.join()
        finally:
            get_logs(c, receivers, "receiver")
            get_logs(c, proxies, "proxy")



#=================================================
#             Multiple experiment tasks
#=================================================
@task
def run_rates(c, config_file="../configs/remote-prod.yaml",
            resolve=lambda x: x,
            v=5,
            use_in_flight=False,
            prot="dombft",
):
    # gcloud_vm(c, config_file=config_file)
    # time.sleep(5)

    try:

        with open(config_file, "r") as cfg_file:
            original_contents = cfg_file.read()
            cfg = yaml.load(original_contents, Loader=yaml.Loader)
            

        if use_in_flight:
            cfg["client"]["sendMode"] = "maxInFlight"

            for num_in_flight in [25, 50, 75, 100, 150, 200]:
                cfg["client"]["maxInFlight"] = num_in_flight
                yaml.dump(cfg, open(config_file, "w"))
                run(c, config_file=config_file, resolve=resolve, v=v, prot=prot)
                c.run(f"cat ../logs/replica*.log ../logs/client*.log | grep PERF >{prot}_if{num_in_flight}.out")
        else:
            cfg["client"]["sendMode"] = "sendRate"
            cfg["client"]["maxInFlight"] = 200

            for send_rate in [250, 500, 750, 1000, 1500, 2000]:
                cfg["client"]["sendRate"] = send_rate
                yaml.dump(cfg, open(config_file, "w"))
                run(c, config_file=config_file, resolve=resolve, v=v, prot=prot)
                c.run(f"cat ../logs/replica*.log ../logs/client*.log | grep PERF >{prot}_sr{send_rate}.out")


    finally:
        with open(config_file, "w") as cfg_file:
            cfg_file.write(original_contents)



#=================================================
#             Other tasks
#=================================================

@task
def copy_keys(c, config_file="../configs/remote-prod.yaml", resolve=lambda x: x):
    group = ThreadingGroup(*get_all_ips(config_file, resolve))
    group.run("rm -rf keys/*")
    
    print("Copying keys over...")
    for process in ["client", "replica", "receiver", "proxy"]:
        group.run(f"mkdir -p keys/{process}")
        for filename in os.listdir(f"../keys/{process}"):
            group.put(os.path.join(f"../keys/{process}", filename), f"keys/{process}")


@task
def copy_bin(c, config_file="../configs/remote-prod.yaml", upload_once=False, resolve=lambda x: x):
    replicas, receivers, proxies, clients = get_process_ips(config_file, resolve)
    group = ThreadingGroup(*replicas, *receivers, *proxies, *clients)    

    if upload_once:
    

        # TODO try and check to see if binaries are stale
        print(f"Copying binaries over to one machine {clients[0]}")
        start_time = time.time()
        conn = Connection(clients[0])

        conn.run("chmod +w dombft_*", warn=True)
        conn.put("../bazel-bin/processes/replica/dombft_replica")
        conn.put("../bazel-bin/processes/receiver/dombft_receiver")
        conn.put("../bazel-bin/processes/proxy/dombft_proxy")
        conn.put("../bazel-bin/processes/client/dombft_client")
        conn.run("chmod +w dombft_*", warn=True)

        print(f"Copying took {time.time() - start_time:.0f}s")


        print(f"Copying to other machines")
        start_time = time.time()

        replicas, receivers, proxies, clients = get_process_ips(config_file, lambda x: x)

        for ip in replicas:
            print(f"Copying dombft_replica to {ip}")
            conn.run(f"scp dombft_replica {ip}:", warn=True)

        for ip in receivers:
            print(f"Copying dombft_receiver to {ip}")
            conn.run(f"scp dombft_receiver {ip}:", warn=True)

        for ip in proxies:
            print(f"Copying dombft_proxy to {ip}")
            conn.run(f"scp dombft_proxy {ip}:", warn=True)

        for ip in set(clients[1:]): # Skip own
            print(f"Copying dombft_client to {ip}")
            conn.run(f"scp dombft_client {ip}:", warn=True)

        print(f"Copying to other machines took {time.time() - start_time:.0f}s")

    else:
        # Otherwise, just copy to all machines

        replicas = SerialGroup(*replicas)
        receivers = SerialGroup(*receivers)
        proxies = SerialGroup(*proxies)
        clients = SerialGroup(*clients)

        group.run("chmod +w dombft_*", warn=True)

        print("Copying binaries over...")

        replicas.put("../bazel-bin/processes/replica/dombft_replica")
        print("Copied replica")

        receivers.put("../bazel-bin/processes/receiver/dombft_receiver")
        print("Copied receiver")

        proxies.put("../bazel-bin/processes/proxy/dombft_proxy")
        print("Copied proxy")

        clients.put("../bazel-bin/processes/client/dombft_client")
        print("Copied client")

        group.run("chmod +w dombft_*", warn=True)





@task
def build(c, config_file="../configs/remote-prod.yaml", resolve=lambda x: x, setup=False):
    group = ThreadingGroup(*set(get_all_ips(config_file, resolve)))
    group.put(config_file)

    if setup:
        group.put("setup.sh")
        group.run("chmod +x ./setup.sh && sudo ./setup.sh")

    print("Cloning/building repo...")

    group.run("git clone https://github.com/dqian3/DOM-BFT", warn=True)
    group.run("cd DOM-BFT && git checkout kvstore_snapshot2 && bazel build //processes/...")

    group.run("rm ~/dombft_*", warn=True)
    group.run("cp ./DOM-BFT/bazel-bin/processes/replica/dombft_replica ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/receiver/dombft_receiver ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/proxy/dombft_proxy ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/client/dombft_client ~")


@task
def cmd(c, cmd, config_file="../configs/remote-prod.yaml", resolve=lambda x: x):
    
    ips = set(get_all_ips(config_file, resolve))
    print(ips)

    group = ThreadingGroup(*ips)
    group.run(cmd)


@task
def copy(c, file, config_file="../configs/remote-prod.yaml"):
    group = ThreadingGroup(*get_all_ips(config_file, resolve))
    group.put(file)


@task
def setup_clockwork(c, config_file="../configs/remote-prod.yaml", install=False, resolve=lambda x: x):
    _, receivers, proxies, _ = get_process_ips(config_file, resolve)
    _, receivers_int, proxies_int, _ = get_process_ips(config_file, resolve=lambda x: x)

    addrs = receivers + proxies
    addrs_int = receivers_int + proxies_int

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
        Connection(ip).run(f"echo '{ttcs_config}'| sudo tee /etc/opt/ttcs/ttcs-agent.cfg")

    group.run("sudo systemctl stop ntp", warn=True)
    group.run("sudo systemctl disable ntp", warn=True)
    group.run("sudo systemctl stop systemd-timesyncd", warn=True)
    group.run("sudo systemctl disable systemd-timesyncd", warn=True)

    group.run("sudo systemctl enable ttcs-agent", warn=True)

    if install:
        group.run("sudo systemctl start ttcs-agent")
    else:
        group.run("sudo systemctl restart ttcs-agent")