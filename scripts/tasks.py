
from fabric import Connection, ThreadingGroup, SerialGroup
from invoke import task
import yaml
import os

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
def gcloud(c, config_file):
    config_file = os.path.abspath(config_file)

    with open(config_file) as cfg_file:
        config = yaml.load(cfg_file, Loader=yaml.Loader)

    # number of replicas
    replicas = config["replica"]["ips"]
    clients = config["client"]["ips"]
    proxies = config["proxy"]["ips"]
    receivers = config["receiver"]["ips"]

    # parse gcloud CLI to get internalIP -> externalIP mapping
    gcloud_output = c.run("gcloud compute instances list").stdout[1:].splitlines()
    gcloud_output = map(lambda s : s.split(), gcloud_output)
    ext_ips = {
        # internal ip and external ip are last 2 tokens in each line
        line[-3] : line[-2]
        for line in gcloud_output
    }


    # TODO setup and compile if needed

    # TODO transfer keys and configs
    ips = []
    for ip in clients + replicas + proxies: # TODO non local receivers?
        ips.append(ext_ips[ip])
    
    group = ThreadingGroup(
        *ips
    )

    group.put(config_file)
    remote_config_file = os.path.basename(config_file)

    handles = group.run("sleep 5", asynchronous=True, warn=True)

    for hdl in handles:
        print(hdl)
        handles[hdl].runner.kill()
        handles[hdl].join()