import argparse
import subprocess
import yaml
import os

parser = argparse.ArgumentParser(prog="genkeys.py", description="Generate keys for DOMBFT")

parser.add_argument("config")
parser.add_argument("-a", "--algorithm", default="RSA", choices=["RSA", "ED25519"])
parser.add_argument("-k", "--keysize", default=2048)

args = parser.parse_args()

# Parse config to get dirs and number of processes for each
with open(args.config) as config_file:
    config = yaml.load(config_file, Loader=yaml.Loader)

# number of keys we need to generate
num_processes = {}
# dir that keys should be put in for each process
dirs = {}

for process in config:
    pconfig = config[process]
    num_processes[process] = len(pconfig["ips"])
    dirs[process] = pconfig["keysDir"]

for process in dirs:
    key_dir = dirs[process]
    nkeys = num_processes[process]

    subprocess.run(["mkdir", "-p", key_dir])

    print(f"Generating {nkeys} keys for {process}")
    for i in range(nkeys):
        key_path = os.path.join(key_dir, process + f"{i}")
        print(key_path)

        if args.algorithm == "RSA":
            subprocess.run(["openssl", "genrsa", "-out", key_path + ".pem", str(args.keysize)])
        elif args.algorithm == "ED25519":
            subprocess.run(["openssl",  "genpkey",  "-algorithm",  "ed25519", "-out", key_path + ".pem"])
        subprocess.run(["openssl", "pkey", "-in", key_path + ".pem", "-pubout", "-out", key_path + ".pub"])
