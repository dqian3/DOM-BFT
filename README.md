# DOM-BFT

Repository for WIP project on applying Deadline Ordered Multicast to a BFT protocol.

Note this repo is based off of [Nezha](https://github.com/Steamgjk/Nezha).

## Installing Bazel

I used [Bazelisk](https://github.com/bazelbuild/bazelisk) as a wrapper for Bazel, which
allows for us to specify a version of Bazel to use (5.2.0). The way third party dependencies
are setup in this repo means that more recent versions of Bazel are not compatible.

## Building Repository

To build the `client`

```
bazel build //client/...
```

The same syntax is used to build the `proxy`, `receiver` and other components. You can
also build all of them at the same time with something like
```
bazel build //proxy/... //receiver/... //client/...
```

## Running 

The built executables will be created by default in `bazel-bin`, inside of which is 
the same directory structure as the source, containing the executables.