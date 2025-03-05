## Scripts

The following directory contains various scripts for running experiments 
and setting up protocols.

These scripts are mostly based off of the [Invoke](https://www.pyinvoke.org/) and [Fabric](https://www.fabfile.org/) libraries.


## Generating Keys

As an example, to generate keys for a local test of the protocol, you can run `invoke local.genkeys ../configs/local.yaml`, which will generate ED25519 keys according to the provided config file

## Running

#### Local

Starting a run of the local version of the protocol can be done with
```
invoke local.run ../configs/local.yaml
```
The script will read from the local config, and start processes for each of the clients/replica/proxies/receivers specified in the config.
The logs of each process will be written to a file in the logs folder.

#### Remote

There are also various scripts for running on a remote setup. These scripts will read the ips
from the provided config file and execute the commands on the remote machines.

First you will need to copy the keys over with
```
invoke remote.keys
```

Then, either build the code on each machine with `invoke remote.build` or simply copy the binaries over (if compatible) with `invoke remote.copy-bin`.

Then, you can run
```
invoke remote.run
```
This is similar to the local run, it simply starts each process, but instead on the remote machines specified in the
config. The log files are also downloaded to your local machine afterwards.

A list of all remote tasks can be seen with
```
invoke -l remote
```

#### Google Cloud
We also provide some scripts for running experiments more conveniently on Google Cloud. Specifically, instead of
providing externally accessible ips in the config files, you can instead put the permanent internal ip of your
machines, and these scripts will automatically translate them. Note, this requires that you have the `gcloud` CLI tool installed.

These scripts wrap all the above `remote.*` scripts, while also providing a few others for managing VMs.