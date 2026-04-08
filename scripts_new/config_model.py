"""Configuration model for OooBFT benchmark automation.

Handles cluster configs (VM names, bench params) and generates OooBFT YAML
configs with resolved IPs for local and remote runs.
"""
from __future__ import annotations

from dataclasses import dataclass, field, replace
from typing import Any

import yaml


DOMBFT_PROTOCOLS = {"dombft", "flutter"}
SUPPORTED_PROTOCOLS = DOMBFT_PROTOCOLS

TOP_LEVEL_KEYS = {
    "platform",
    "replica",
    "client",
    "proxy",
    "bench",
    "zone",
    "project",
    "user",
    "key_file",
}
ROLE_KEYS = {"vms", "port"}
BENCH_KEYS = {
    "transport",
    "f",
    "e",
    "send_rate",
    "max_in_flight",
    "runtime_secs",
    "request_size",
    "batch_size",
    "use_hmac",
    "use_proxy",
    "num_send_threads",
    "num_verify_threads",
    # Flutter-specific
    "clock_broadcast_interval",
    "initial_bet",
    "bet_increment",
    # Proxy-specific
    "offset_coefficient",
    "proxy_batch_enabled",
    "proxy_batch_max_count",
    "proxy_batch_max_delay",
}


class ConfigError(ValueError):
    pass


def _reject_unknown(section: str, data: dict[str, Any], allowed: set[str]) -> None:
    unknown = sorted(set(data) - allowed)
    if unknown:
        raise ConfigError(f"unknown keys in {section}: {', '.join(unknown)}")


def _as_int(name: str, value: Any, default: int | None = None) -> int:
    if value is None:
        if default is None:
            raise ConfigError(f"missing integer field: {name}")
        return default
    if isinstance(value, bool) or not isinstance(value, int):
        raise ConfigError(f"{name} must be an integer")
    return value


def _as_float(name: str, value: Any, default: float) -> float:
    if value is None:
        return default
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(f"{name} must be a number")
    return float(value)


def _as_bool(name: str, value: Any, default: bool) -> bool:
    if value is None:
        return default
    if not isinstance(value, bool):
        raise ConfigError(f"{name} must be a boolean")
    return value


def _as_str_list(name: str, value: Any) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or any(not isinstance(v, str) for v in value):
        raise ConfigError(f"{name} must be a list of strings")
    return list(value)


@dataclass(frozen=True)
class NodeGroup:
    vms: list[str]
    port: int


@dataclass(frozen=True)
class BenchConfig:
    provided_keys: frozenset[str] = field(default_factory=frozenset)
    transport: str = "tcp"
    f: int = 1
    e: int = 0
    send_rate: int = 500
    max_in_flight: int = 1000
    runtime_secs: int = 40
    request_size: int = 512
    batch_size: int = 5
    use_hmac: bool = True
    use_proxy: bool = True
    num_send_threads: int = 4
    num_verify_threads: int = 4
    # Flutter
    clock_broadcast_interval: int = 50000
    initial_bet: int = 100000
    bet_increment: int = 100000
    # Proxy
    offset_coefficient: float = 1.1
    proxy_batch_enabled: bool = True
    proxy_batch_max_count: int = 50
    proxy_batch_max_delay: int = 2000

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "BenchConfig":
        _reject_unknown("bench", data, BENCH_KEYS)
        return cls(
            provided_keys=frozenset(data.keys()),
            transport=data.get("transport", "tcp"),
            f=_as_int("f", data.get("f"), 1),
            e=_as_int("e", data.get("e"), 0),
            send_rate=_as_int("send_rate", data.get("send_rate"), 500),
            max_in_flight=_as_int("max_in_flight", data.get("max_in_flight"), 1000),
            runtime_secs=_as_int("runtime_secs", data.get("runtime_secs"), 40),
            request_size=_as_int("request_size", data.get("request_size"), 512),
            batch_size=_as_int("batch_size", data.get("batch_size"), 5),
            use_hmac=_as_bool("use_hmac", data.get("use_hmac"), True),
            use_proxy=_as_bool("use_proxy", data.get("use_proxy"), True),
            num_send_threads=_as_int("num_send_threads", data.get("num_send_threads"), 4),
            num_verify_threads=_as_int("num_verify_threads", data.get("num_verify_threads"), 4),
            clock_broadcast_interval=_as_int("clock_broadcast_interval", data.get("clock_broadcast_interval"), 50000),
            initial_bet=_as_int("initial_bet", data.get("initial_bet"), 100000),
            bet_increment=_as_int("bet_increment", data.get("bet_increment"), 100000),
            offset_coefficient=_as_float("offset_coefficient", data.get("offset_coefficient"), 1.1),
            proxy_batch_enabled=_as_bool("proxy_batch_enabled", data.get("proxy_batch_enabled"), True),
            proxy_batch_max_count=_as_int("proxy_batch_max_count", data.get("proxy_batch_max_count"), 50),
            proxy_batch_max_delay=_as_int("proxy_batch_max_delay", data.get("proxy_batch_max_delay"), 2000),
        )


@dataclass(frozen=True)
class ClusterConfig:
    platform: str
    replica: NodeGroup
    client: NodeGroup
    proxy: NodeGroup | None
    bench: BenchConfig
    zone: str | None = None
    project: str | None = None
    user: str | None = None
    key_file: str | None = None


@dataclass(frozen=True)
class ResolvedNode:
    id: int
    name: str
    host: str
    port: int

    @property
    def address(self) -> str:
        return f"{self.host}:{self.port}"


@dataclass(frozen=True)
class ResolvedCluster:
    platform: str
    replicas: list[ResolvedNode]
    clients: list[ResolvedNode]
    proxies: list[ResolvedNode]
    bench: BenchConfig


def load_cluster_config(path: str) -> ClusterConfig:
    with open(path) as f:
        raw = yaml.safe_load(f)
    if not isinstance(raw, dict):
        raise ConfigError("config must be a YAML mapping")

    platform = raw.get("platform", "local")

    replica_raw = raw.get("replica", {})
    replica = NodeGroup(
        vms=_as_str_list("replica.vms", replica_raw.get("vms")),
        port=_as_int("replica.port", replica_raw.get("port"), 34000),
    )

    client_raw = raw.get("client", {})
    client = NodeGroup(
        vms=_as_str_list("client.vms", client_raw.get("vms")),
        port=_as_int("client.port", client_raw.get("port"), 33000),
    )

    proxy_raw = raw.get("proxy", {})
    proxy = None
    if proxy_raw and proxy_raw.get("vms"):
        proxy = NodeGroup(
            vms=_as_str_list("proxy.vms", proxy_raw.get("vms")),
            port=_as_int("proxy.port", proxy_raw.get("port"), 31000),
        )

    bench_raw = raw.get("bench", {})
    bench = BenchConfig.from_dict(bench_raw)

    return ClusterConfig(
        platform=platform, replica=replica, client=client, proxy=proxy,
        bench=bench, zone=raw.get("zone"), project=raw.get("project"),
        user=raw.get("user"), key_file=raw.get("key_file"),
    )


def apply_bench_overrides(config: ClusterConfig, **overrides: Any) -> ClusterConfig:
    bench = config.bench
    provided = set(bench.provided_keys)
    for key, value in overrides.items():
        if value is None:
            continue
        if not hasattr(bench, key):
            raise ConfigError(f"unknown bench override: {key}")
        provided.add(key)
        bench = replace(bench, **{key: value})
    bench = replace(bench, provided_keys=frozenset(provided))
    return replace(config, bench=bench)


def resolve_local_cluster(config: ClusterConfig, protocol: str) -> ResolvedCluster:
    """Resolve a cluster config for local execution (localhost IPs)."""
    b = config.bench
    if protocol == "flutter":
        n_replicas = 5 * b.f + 1
    else:
        n_replicas = 3 * b.f + 2 * b.e + 1

    n_clients = len(config.client.vms) or 2
    n_proxies = len(config.proxy.vms) if config.proxy else n_clients

    replicas = [
        ResolvedNode(id=i, name=f"replica{i}", host=f"127.0.2.{i}", port=config.replica.port)
        for i in range(n_replicas)
    ]
    clients = [
        ResolvedNode(id=i, name=f"client{i}", host=f"127.0.0.{i}", port=config.client.port)
        for i in range(n_clients)
    ]
    proxy_port = config.proxy.port if config.proxy else 31000
    proxies = [
        ResolvedNode(id=i, name=f"proxy{i}", host=f"127.0.1.{i}", port=proxy_port)
        for i in range(n_proxies)
    ] if protocol == "dombft" and b.use_proxy else []

    return ResolvedCluster(
        platform=config.platform, replicas=replicas, clients=clients, proxies=proxies, bench=b,
    )


def resolve_remote_cluster(config: ClusterConfig, protocol: str, remote: Any) -> ResolvedCluster:
    """Resolve a cluster config for remote execution (real IPs from VMs)."""
    all_names = config.replica.vms + config.client.vms
    if config.proxy:
        all_names += [v for v in config.proxy.vms if v not in all_names]

    if hasattr(remote, "get_all_ips"):
        all_ips = remote.get_all_ips(all_names)
    else:
        all_ips = {name: remote.get_ip(name) for name in all_names}

    replicas = [
        ResolvedNode(id=i, name=name, host=all_ips[name], port=config.replica.port)
        for i, name in enumerate(config.replica.vms)
    ]
    clients = [
        ResolvedNode(id=i, name=name, host=all_ips[name], port=config.client.port)
        for i, name in enumerate(config.client.vms)
    ]
    proxies = []
    if config.proxy:
        proxy_port = config.proxy.port
        proxies = [
            ResolvedNode(id=i, name=name, host=all_ips[name], port=proxy_port)
            for i, name in enumerate(config.proxy.vms)
        ]

    return ResolvedCluster(
        platform=config.platform, replicas=replicas, clients=clients, proxies=proxies,
        bench=config.bench,
    )


def remote_targets(config: ClusterConfig) -> list[str]:
    seen = []
    for name in config.replica.vms + config.client.vms:
        if name not in seen:
            seen.append(name)
    if config.proxy:
        for name in config.proxy.vms:
            if name not in seen:
                seen.append(name)
    return seen


def generate_config(resolved: ResolvedCluster, protocol: str, out_dir: str, remote_keys_prefix: str | None = None) -> str:
    """Generate OooBFT YAML config + keys in out_dir. Returns path to config file.

    Like aspen-bft's 'generate' command: one call produces everything needed
    to run the cluster. Keys are generated if they don't already exist in out_dir.
    All paths in the config are absolute (local) or relative (remote_keys_prefix).

    Args:
        remote_keys_prefix: If set, use this prefix for keysDir in config instead of
            absolute local paths (e.g. "keys" for remote VMs where keys are at ~/keys/).
    """
    import os
    import subprocess

    os.makedirs(out_dir, exist_ok=True)

    # Generate keys into out_dir/keys/{role}/
    roles = [
        ("replica", len(resolved.replicas)),
        ("client", len(resolved.clients)),
        ("proxy", len(resolved.proxies)),
    ]
    for role, count in roles:
        if count == 0:
            continue
        keys_dir = os.path.join(out_dir, "keys", role)
        # Skip if keys already exist
        if os.path.exists(os.path.join(keys_dir, f"{role}0.der")):
            continue
        os.makedirs(keys_dir, exist_ok=True)
        for i in range(count):
            key_path = os.path.join(keys_dir, f"{role}{i}")
            subprocess.run(
                ["openssl", "genpkey", "-outform", "der", "-algorithm", "ed25519",
                 "-out", f"{key_path}.der"],
                check=True, capture_output=True,
            )
            subprocess.run(
                ["openssl", "pkey", "-outform", "der", "-in", f"{key_path}.der",
                 "-pubout", "-out", f"{key_path}.pub"],
                check=True, capture_output=True,
            )
        print(f"  Generated {count} {role} keys in {keys_dir}")

    # Build config with key paths
    b = resolved.bench
    if remote_keys_prefix:
        client_keys = f"{remote_keys_prefix}/client"
        replica_keys = f"{remote_keys_prefix}/replica"
        proxy_keys = f"{remote_keys_prefix}/proxy"
    else:
        client_keys = os.path.abspath(os.path.join(out_dir, "keys", "client"))
        replica_keys = os.path.abspath(os.path.join(out_dir, "keys", "replica"))
        proxy_keys = os.path.abspath(os.path.join(out_dir, "keys", "proxy"))

    config = {
        "app": "counter",
        "transport": b.transport,
        "resiliency": {"f": b.f, "e": b.e},
        "preserializationMode": "disabled",
        "useProxy": b.use_proxy and protocol == "dombft",
        "client": {
            "ips": [c.host for c in resolved.clients],
            "port": resolved.clients[0].port if resolved.clients else 33000,
            "keysDir": client_keys,
            "sendMode": "sendRate",
            "sendRate": b.send_rate,
            "maxInFlight": b.max_in_flight,
            "runtimeSeconds": b.runtime_secs,
            "requestSize": b.request_size,
            "useHMAC": b.use_hmac,
        },
        "replica": {
            "ips": [r.host for r in resolved.replicas],
            "port": resolved.replicas[0].port if resolved.replicas else 34000,
            "keysDir": replica_keys,
            "numSendThreads": b.num_send_threads,
            "numVerifyThreads": b.num_verify_threads,
        },
        "proxy": {
            "ips": [p.host for p in resolved.proxies] if resolved.proxies else [],
            "forwardPort": resolved.proxies[0].port if resolved.proxies else 0,
            "keysDir": proxy_keys if resolved.proxies else "",
            "offsetCoefficient": b.offset_coefficient,
            "proxyBatchEnabled": b.proxy_batch_enabled,
            "proxyBatchMaxCount": b.proxy_batch_max_count,
            "proxyBatchMaxDelay": b.proxy_batch_max_delay,
        },
    }

    if protocol == "flutter":
        config["client"]["initialBet"] = b.initial_bet
        config["client"]["betIncrement"] = b.bet_increment
        config["replica"]["clockBroadcastInterval"] = b.clock_broadcast_interval

    config_path = os.path.join(out_dir, "config.yaml")
    with open(config_path, "w") as f:
        yaml.dump(config, f, default_flow_style=False)

    return config_path
