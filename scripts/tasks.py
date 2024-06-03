
from fabric import Connection
from invoke import task
import yaml
import os

# TODO we can process output of these here instead of in the terminal

@task
def replica(c, config_file, id, verbosity=5):
    c.run(f"./bazel-bin/processes/replica/dombft_replica -v {verbosity} -config {config_file} -replicaId {id}")


@task
def client(c, config_file, id, verbosity=5):
    with c.cd(".."):
        c.run(f"./bazel-bin/processes/client/dombft_client -v {verbosity} -config {config_file} -clientId {id}")

@task
def proxy(c, config_file, id, verbosity=5):
    with c.cd(".."):
        c.run(f"./bazel-bin/processes/proxy/dombft_proxy -v {verbosity} -config {config_file} -proxyId {id}")


@task
def receiver(c, config_file, id, verbosity=5):
    with c.cd(".."):
        c.run(f"./bazel-bin/processes/proxy/dombft_proxy -v {verbosity} -config {config_file} -proxyId {id}")


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

    