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
bazel build //processes/client/...
```

The same syntax is used to build the `proxy`, `receiver` and other components. You can
also build all of them at the same time with something like
```
bazel build //processes/...
```

## Running 

The built executables will be created by default in `bazel-bin`, inside of which is 
the same directory structure as the source, containing the executables.

The [/scripts](/scripts) directory also contains a number of helpful scripts for
running the executables both locally and remotely. See the [README there](/scripts/README.md)
for more details.

## Tests

Tests exist for this code, though I make no guarantees about how comprehensive they are. The
tests can be built with 
```
bazel build //test/...
```
The files starting with `test_` are actualy gtest tests for various components that I've built.
There are also a few experimental "sandbox" programs that serve as examples of usage. 

## Basic Docker Setup

This repo includes a super basic Docker setup which can be used for development. Future work is planned to make it more realistic for testing and deployment.

The current way to use this setup is to simply run
```
docker compose run --rm dev
```
This will give you a bash shell in a conatiner where you can run the above commands to compile the code. Docker compose mounts the source code into the container as well, so any changes you make locally (i.e. in an editor) will be reflected in your conatiner.
