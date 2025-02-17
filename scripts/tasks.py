import os
import time

import invoke
import yaml
from fabric import Connection, SerialGroup, ThreadingGroup
from invoke import task
import copy


# TODO we can process output of these here instead of in the terminal
@task
def local(c, config_file="../configs/local.yaml", v=5, prot="dombft",
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

    # TODO verbosity
    with c.cd(".."):
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
            cmd = f"./bazel-bin/processes/replica/dombft_replica -prot {prot} -v {v} -config {config_file} -replicaId {id} {swap_arg} {view_change_arg} &>logs/replica{id}.log"
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
        # make sure clients would not fallback before any requests are commited, corner case not reolved. 
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
    gcloud_output = c.run("gcloud compute instances list").stdout.splitlines()[1:]
    gcloud_output = map(lambda s: s.split(), gcloud_output)
    ext_ips = {
        # internal ip and external ip are last 2 tokens in each line
        line[-3]: line[-2]
        for line in gcloud_output
    }
    return ext_ips


def get_all_int_ips(config):
    int_ips = set()
    for process in config:
        if process == "transport" or process == "app": continue
        int_ips |= set([ip for ip in config[process]["ips"]])
    
    return int_ips


def get_all_ext_ips(config, ext_ip_map):
    ips = []
    for ip in get_all_int_ips(config):  # TODO non local receivers?
        if (ip not in ext_ip_map): continue
        ips.append(ext_ip_map[ip])

    return ips 


@task
def gcloud_vm(c, config_file="../configs/remote-prod.yaml", stop=False):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    int_ips = get_all_int_ips(config)

    gcloud_output = c.run("gcloud compute instances list").stdout.splitlines()[1:]
    gcloud_output = map(lambda s : s.split(), gcloud_output)

    vm_info = {
        # name, zone, type, internal ip
        line[3] : (line[0], line[1])
        for line in gcloud_output
    }

    hdls = []
    for ip in int_ips: 
        name, zone = vm_info[ip]
        h = c.run(f"gcloud compute instances {'stop' if stop else 'start'} {name} --zone {zone}", asynchronous=True)
        hdls.append(h)

    for h in hdls:
        h.join()
    
    print(f"{'Stopped' if stop else 'Started'} all instances!")
    time.sleep(3) # Give time for ssh daemons to start for other tasks


@task
def gcloud_clockwork(c, config_file="../configs/remote-prod.yaml", install=False):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    int_ips =  config["receiver"]["ips"] + config["proxy"]["ips"]

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
def gcloud_cmd(c, cmd, config_file="../configs/remote-prod.yaml"):
    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = ThreadingGroup(*get_all_ext_ips(config, ext_ips))
    group.run(cmd)


@task
def gcloud_copy(c, file, config_file="../configs/remote-prod.yaml"):
    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = ThreadingGroup(*get_all_ext_ips(config, ext_ips))
    group.put(file)

    
@task
def gcloud_build(c, config_file="../configs/remote-prod.yaml", setup=False):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = ThreadingGroup(*get_all_ext_ips(config, ext_ips))
    group.put(config_file)

    if setup:
        group.put("setup.sh")
        group.run("chmod +x ./setup.sh && sudo ./setup.sh")

    print("Cloning/building repo...")

    group.run("git clone https://github.com/dqian3/DOM-BFT", warn=True)
    group.run("cd DOM-BFT && git pull --rebase && git checkout kv_store_complete && bazel build //processes/...")

    group.run("rm ~/dombft_*", warn=True)
    group.run("cp ./DOM-BFT/bazel-bin/processes/replica/dombft_replica ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/receiver/dombft_receiver ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/proxy/dombft_proxy ~")
    group.run("cp ./DOM-BFT/bazel-bin/processes/client/dombft_client ~")


@task
def gcloud_copy_keys(c, config_file="../configs/remote-prod.yaml"):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = ThreadingGroup(*get_all_ext_ips(config, ext_ips))
    group.put(config_file)

    group.run("rm -rf keys/*")

    
    print("Copying keys over...")
    for process in ["client", "replica", "receiver", "proxy"]:
        group.run(f"mkdir -p keys/{process}")
        for filename in os.listdir(f"../keys/{process}"):
            group.put(os.path.join(f"../keys/{process}", filename), f"keys/{process}")


@task
def gcloud_copy_bin(c, config_file="../configs/remote-prod.yaml"):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)

    replicas = config["replica"]["ips"]
    receivers = config["receiver"]["ips"]
    proxies = config["proxy"]["ips"]
    clients = config["client"]["ips"]

    # TODO try and check to see if binaries are stale
    print(f"Copying binaries over to one machine {ext_ips[clients[0]]} ({clients[0]})")
    start_time = time.time()
    conn = Connection(ext_ips[clients[0]])

    conn.run("chmod +w dombft_*", warn=True)
    conn.put("../bazel-bin/processes/replica/dombft_replica")
    conn.put("../bazel-bin/processes/receiver/dombft_receiver")
    conn.put("../bazel-bin/processes/proxy/dombft_proxy")
    conn.put("../bazel-bin/processes/client/dombft_client")
    conn.run("chmod +w dombft_*", warn=True)

    print(f"Copying took {time.time() - start_time:.0f}s")


    print(f"Copying to other machines")
    start_time = time.time()

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


def get_gcloud_process_ips(c, filter):
    gcloud_output = c.run(f"gcloud compute instances list | grep {filter}").stdout.splitlines()
    gcloud_output = map(lambda s : s.split(), gcloud_output)

    return [
        # internal ip is 3rd last token in line
        line[-3]
        for line in gcloud_output
    ]


@task
def gcloud_create_prod(c, config_template="../configs/remote-prod.yaml"):
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

    zones = ["us-west1-c", "us-west4-c", "us-east1-c", "us-east4-c"]

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



def arun_on(ip, logfile, local_log=False, profile=False):
    def perf_prefix(prof_file):
        return f"env LD_PRELOAD='/home/dqian/libprofiler.so' CPUPROFILE={prof_file} CPUPROFILE_FREQUENCY={10} "

    if local_log:
        logfile = os.path.join("../logs/", logfile)
        def arun_local_log(command, **kwargs):
            log = open(logfile, "w")
            conn = Connection(ip)

            if (profile):
                command = perf_prefix(os.path.splitext(logfile)[0] + '.prof') + command

            print(f"Running {command} on {ip}, logging to local {logfile}")
            return conn.run(command + " 2>&1", **kwargs, asynchronous=True, warn=True, out_stream=log)

        return arun_local_log

    else:
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



@task
def gcloud_logs(c, config_file="../configs/remote-prod.yaml"):

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
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


@task
def gcloud_run_largen(c, config_file="../configs/remote-large-n.yaml",
               v=5,
               prot="dombft",
):
    try:
        with open(config_file, "r") as cfg_file:
            original_contents = cfg_file.read()
            original_cfg = yaml.load(original_contents, Loader=yaml.Loader)
          
        in_flight = original_cfg["client"]["maxInFlight"]

        for n_replicas in [7, 10, 13, 16]:
            gcloud_vm(c, config_file=config_file)
            time.sleep(20)


            cfg = copy.deepcopy(original_cfg)

            cfg["replica"]["ips"] = cfg["replica"]["ips"][:n_replicas]
            cfg["receiver"]["ips"] = cfg["receiver"]["ips"][:n_replicas]

            yaml.dump(cfg, open(config_file, "w"))
            gcloud_run(c, config_file=config_file, v=v, prot=prot)
            c.run(f"cat ../logs/replica*.log ../logs/client*.log | grep PERF >{prot}_n{n_replicas}_if{in_flight}.out")

            gcloud_vm(c, config_file=config_file, stop=True)

    finally:
        with open(config_file, "w") as cfg_file:
            cfg_file.write(original_contents)

        gcloud_vm(c, config_file=config_file, stop=True)



@task
def gcloud_run_rates(c, config_file="../configs/remote-prod.yaml",
               v=5,
               prot="dombft",
):
    # gcloud_vm(c, config_file=config_file)
    # time.sleep(5)

    try:

        with open(config_file, "r") as cfg_file:
            original_contents = cfg_file.read()
            cfg = yaml.load(original_contents, Loader=yaml.Loader)
            

        cfg["client"]["sendMode"] = "maxInFlight"

        for inFlight in [25, 50, 75, 100, 150, 200]:
            cfg["client"]["maxInFlight"] = inFlight
            yaml.dump(cfg, open(config_file, "w"))
            gcloud_run(c, config_file=config_file, v=v, prot=prot)
            c.run(f"cat ../logs/replica*.log ../logs/client*.log | grep PERF >{prot}_if{inFlight}.out")

    finally:
        with open(config_file, "w") as cfg_file:
            cfg_file.write(original_contents)

        gcloud_vm(c, config_file=config_file, stop=True)

# local_log_file is good for debugging, but will slow the system down at high throughputs
@task
def gcloud_run(c, config_file="../configs/remote-prod.yaml",
               local_log=False,
               dom_logs=False,
               slow_path_freq=0,
               normal_path_freq=0,
               view_change_freq=0,
               drop_checkpoint_freq=0,
               commit_local_in_view_change=False,
               profile=False,
               v=5,
               prot="dombft",
               max_view_change = 0,
):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = ThreadingGroup(*get_all_ext_ips(config, ext_ips))
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

    n = len(replicas)
    f = len(replicas) // 3

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
            
        arun = arun_on(ip, f"replica{id}.log", local_log=local_log, profile=profile)
        hdl = arun(f"{replica_path} -prot {prot} -v {v} -config {remote_config_file} -replicaId {id} {swap_arg} {view_change_arg} {drop_checkpoint_arg}")
        other_handles.append(hdl)

    print("Starting receivers")
    for id, ip in enumerate(receivers):
        arun = arun_on(ip, f"receiver{id}.log", local_log=local_log, profile=profile)
        hdl = arun(f"{receiver_path} -v {v} -config {remote_config_file} -receiverId {id}")
        other_handles.append(hdl)

    print("Starting proxies")
    for id, ip in enumerate(proxies):
        arun = arun_on(ip, f"proxy{id}.log", local_log=local_log, profile=profile )
        hdl = arun(f"{proxy_path} -v {v} -config {remote_config_file} -proxyId {id}")
        other_handles.append(hdl)

    time.sleep(2)

    print("Starting clients")
    for id, ip in enumerate(clients):
        arun = arun_on(ip, f"client{id}.log", local_log=local_log, profile=profile)
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

        if not local_log:
            get_logs(c, clients, "client")
            get_logs(c, replicas, "replica")

            if dom_logs:
                get_logs(c, receivers, "receiver")
                get_logs(c, proxies, "proxy")



@task
def gcloud_reorder_exp(c, config_file="../configs/remote-prod.yaml", 
                    poisson=False, ignore_deadlines=False, duration=20, rate=100,
                    local_log=False):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    ext_ips = get_gcloud_ext_ips(c)
    group = ThreadingGroup(*get_all_ext_ips(config, ext_ips))
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

        try:
            for hdl in other_handles:
                hdl.runner.send_interrupt(KeyboardInterrupt())
                hdl.join()
        finally:
            get_logs(c, receivers, "receiver")
            get_logs(c, proxies, "proxy")

