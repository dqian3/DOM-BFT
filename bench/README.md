# Basic Transport Benchmarks

The code in this folder is used to do basic benchmarking of our wrappers around
the NNG transport library. It simply measures how long it takes to receive
50k requests.

The benchmark measures the speed that a single thread can receive messages.
On the testbed we are using, this caps out to about ~50k requests per second.

## Build
To build follow the main repo setup instructions and then run.
```
bazel build //bench/...
```

## Running

The arguments for each process are as follows

```
./nng_recv <bind_ip> <bind_base_port> <other_ip> <other_base_port> <num_shards> <num_clients>
```

Note the `other_ip` and `other_base_port` arguments don't actually matter because the communication is only a single direction.

The `bind_base_port` is the first port the socket will use. Ports will be assigned starting from this based on shards and client_ids.

```
./nng_recv <src_ip> <src_base_port> <dst_ip> <dst_base_port> <num_shards> <num_clients>
```


## Examples

#### Localhost
Following is an example of running over localhost

Start the receiver
```
./bazel-bin/bench/nng_recv 127.0.0.2 3000 127.0.0.1 3000 1 1
```

And start the sender
```
./bazel-bin/bench/nng_send 127.0.0.1 3000 127.0.0.2 3000 1 0
```

The number of shards can be tweaked as well.

#### Multiple clients
Start the receiver (assuming it is on `10.0.0.1`)
```
./bazel-bin/bench/nng_recv 0.0.0.0 3000 127.0.0.1 3000 2 2
```
As mentioned the other ip/src doesn't actually matter, but could be used for bidirectional experiments


And start the senders on different machines (assuming they are on `10.0.0.2` and `10.0.0.3`)

```
./bazel-bin/bench/nng_send 10.0.0.2 3000 10.0.0.1 3000 2 0
```

```
./bazel-bin/bench/nng_send 10.0.0.3 3000 10.0.0.1 3000 2 1
```