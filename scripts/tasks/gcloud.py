import os
import time

import yaml
from invoke import task
from fabric import Connection, ThreadingGroup

import copy

from . import remote


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


def get_address_resolver(context):
    ext_ip_map = get_gcloud_ext_ips(context)
    return lambda ip: ext_ip_map[ip]

@task
def vm(c, config_file="../configs/remote-prod.yaml", stop=False):
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

    if not stop:
        print("Sleeping 15 seconds...")
        time.sleep(15) # Give time for ssh daemons to start for other tasks



@task
def cmd(c, cmd, config_file="../configs/remote-prod.yaml"):
    remote.cmd(c, cmd, config_file=config_file, resolve=get_address_resolver(c))


@task
def setup_clockwork(c, config_file="../configs/remote-prod.yaml", install=False):
    config_file = os.path.abspath(config_file)
    resolve = get_address_resolver(c)
    remote.setup_clockwork(c, config_file=config_file, resolve=resolve, install=install)


@task
def build(c, config_file="../configs/remote-prod.yaml", setup=False):
    config_file = os.path.abspath(config_file)
    resolve = get_address_resolver(c)
    remote.build(c, config_file=config_file, resolve=resolve, setup=setup)


@task
def copy_keys(c, config_file="../configs/remote-prod.yaml"):
    resolve = get_address_resolver(c)
    remote.copy_keys(c, config_file, resolve=resolve)

@task
def copy_bin(c, config_file="../configs/remote-prod.yaml", upload_once=False):
    resolve = get_address_resolver(c)
    remote.copy_bin(c, config_file, upload_once=upload_once, resolve=resolve)


def get_gcloud_process_ips(c, filter):
    gcloud_output = c.run(f"gcloud compute instances list | grep {filter}").stdout.splitlines()
    gcloud_output = map(lambda s : s.split(), gcloud_output)

    return [
        # internal ip is 3rd last token in line
        line[-3]
        for line in gcloud_output
    ]


@task
def create_prod(c, config_template="../configs/remote-prod.yaml"):
    # This is probably better, but can't be across zones:
    # https://cloud.google.com/compute/docs/instances/multiple/create-in-bulk

    create_vm_template = """
gcloud compute instances create {} \
    --project=mythic-veld-419517 \
    --zone={} \
    --machine-type=t2d-standard-16 \
    --network-interface=network-tier=PREMIUM,stack-type=IPV4_ONLY,subnet=default \
    --create-disk=auto-delete=yes,boot=yes,device-name=prod-replica1,image=projects/debian-cloud/global/images/debian-12-bookworm-v20240709,mode=rw,size=10,type=pd-balanced 
"""

    zones = ["us-west1-c", "us-west4-c", "us-east1-c", "us-east4-c"]

    config_file = os.path.abspath(config_template)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)


    n = len(config["replica"]["ips"])
    n_clients = len(config["client"]["ips"])

    for i in range(n):
        zone = zones[i % len(zones)]
        c.run(create_vm_template.format(f"replica{i}", zone))
    

    for i in range(n_clients):
        zone = zones[i % len(zones)]
        c.run(create_vm_template.format(f"client{i}", zone))


    config["replica"]["ips"] = get_gcloud_process_ips(c, "replica")
    config["receiver"]["ips"] = get_gcloud_process_ips(c, "replica")
    config["proxy"]["ips"] = get_gcloud_process_ips(c, "client")
    config["client"]["ips"] = get_gcloud_process_ips(c, "client")

    filename, ext = os.path.splitext(config_template)
    yaml.dump(config, open(filename + "-prod" + ext, "w"))





@task
def gcloud_run_largen(c, config_file="../configs/remote-large-n.yaml",
               v=5,
               prot="dombft",
):
    # Leaving this as gcloud only, because we can't really do this on a static set of ips
    # Could obv be ported to other platforms if needed
    try:
        with open(config_file, "r") as cfg_file:
            original_contents = cfg_file.read()
            original_cfg = yaml.load(original_contents, Loader=yaml.Loader)
          

        for n_replicas in [7, 10, 13, 16]:
            vm(c, config_file=config_file) # This should only start the vms that are needed, not all
            time.sleep(10)

            
            cfg = copy.deepcopy(original_cfg)

            cfg["client"]["maxInFlight"] = 200
            cfg["client"]["sendMode"] = "sendRate"

            cfg["client"]["ips"] = cfg["client"]["ips"][:n_replicas]

            cfg["replica"]["ips"] = cfg["replica"]["ips"][:n_replicas]
            cfg["receiver"]["ips"] = cfg["receiver"]["ips"][:n_replicas]


            for total_send_rate in [12000, 16000, 20000]:
                send_rate = total_send_rate // n_replicas

                cfg["client"]["sendRate"] = send_rate

                yaml.dump(cfg, open(config_file, "w"))
                run(c, config_file=config_file, v=v, prot=prot)
                c.run(f"cat ../logs/replica*.log ../logs/client*.log | grep PERF >{prot}_n{n_replicas}_sr{send_rate}.out")

            vm(c, config_file=config_file, stop=True)

    finally:
        with open(config_file, "w") as cfg_file:
            cfg_file.write(original_contents)

        vm(c, config_file=config_file, stop=True)



@task
def run_rates(c, config_file="../configs/remote-prod.yaml",
               v=5,
               prot="dombft",
               batch_size=1,
               use_in_flight=False,
):
    resolve = get_address_resolver(c)
    remote.run_rates(c, config_file=config_file, resolve=resolve, v=v, prot=prot, batch_size=batch_size, use_in_flight=use_in_flight)
    vm(c, config_file=config_file, stop=True)

# local_log_file is good for debugging, but will slow the system down at high throughputs
@task
def run(
    c, 
    
    config_file="../configs/remote-prod.yaml",
    prot="dombft",
    
    v=5,
    dom_logs=False,
    profile=False,
    filter_client_logs=False,

    batch_size=0,
    slow_path_freq=0,
    normal_path_freq=0,
    view_change_freq=0,
    drop_checkpoint_freq=0,
    commit_local_in_view_change=False,
    max_view_change = 0,
):
    # Wrapper around remote run to convert ips
    resolve = get_address_resolver(c)
    remote.run(c, config_file, resolve=resolve, prot=prot, v=v, dom_logs=dom_logs, profile=profile,
               batch_size=batch_size, filter_client_logs=filter_client_logs,
               slow_path_freq=slow_path_freq, normal_path_freq=normal_path_freq, view_change_freq=view_change_freq,
               drop_checkpoint_freq=drop_checkpoint_freq, commit_local_in_view_change=commit_local_in_view_change,
               max_view_change=max_view_change)


@task
def logs(c,  config_file="../configs/remote-prod.yaml", resolve=lambda x: x,):
    # ips of each process 
    resolve = get_address_resolver(c)
    remote.logs(c, config_file=config_file, resolve=resolve)




@task
def gcloud_reorder_exp(c, config_file="../configs/remote-prod.yaml", 
                    poisson=False, ignore_deadlines=False, duration=20, rate=100,
                    local_log=False):
    resolve = get_address_resolver(c)
    remote.reorder_exp(c, config_file, resolve=resolve, poisson=poisson, ignore_deadlines=ignore_deadlines, duration=duration, rate=rate, local_log=local_log)
