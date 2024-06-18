## Scripts

The following directory contains various scripts for running experiments 
and setting up protocols.

These scripts are mostly based off of the [Invoke](https://www.pyinvoke.org/) and [Fabric](https://www.fabfile.org/) libraries.

The scripts also largely rely on

## Generating Keys

As an example, to generate keys for a local test of the protocol, you can run `python gen_keys.py ../configs/local.yaml`.
Note that the other scripts expect the `keys` dir to be in the top level directory, so you should probably also run
`mv keys/ ..`.

## Running

#### Local

Starting a run of the local version of the protocol can be done with
```
invoke local ../configs/local.yaml
```
The script will read from the local config, and start processes for each of the clients/replica/proxies/receivers specified in the config.
The logs of each process will be written to a file in the logs folder.

#### Cloud

There are also various scripts for running on a gcloud setup. These require that you have the `gcloud` CLI tool installed, since our
script relies on it to get the external IPS of the machines to make connnections. By default these scripts will use `../configs/remote.yaml`,
you can change it with a command line argument to another config file if needed.

First you will need to copy the keys over with
```
invoke gcloud-copy-keys
```

Then, either build the code on each machine with `invoke gcloud-build` or simply copy the binaries over (if compatible) with `invoke gcloud-copy-bin`.

Then, you can run
```
invoke gcloud-run
```
This is similar to the local run, it simply starts each process, but instead on the google cloud machines specified in the config.
The logs are still recorded locally, however.
