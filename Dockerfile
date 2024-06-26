FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive


RUN apt-get update && apt-get install -y \
    apt-transport-https \
    curl \
    gnupg \
    curl \
    libssl-dev \
    git-all \
    build-essential \
    wget

# Install Bazel
RUN curl -fsSL https://bazel.build/bazel-release.pub.gpg | gpg --dearmor > bazel.gpg \
    && mv bazel.gpg /etc/apt/trusted.gpg.d/ \
    && echo "deb [arch=amd64] https://storage.googleapis.com/bazel-apt stable jdk1.8" >/etc/apt/sources.list.d/bazel.list \
    && curl https://bazel.build/bazel-release.pub.gpg | apt-key add - \
    && apt-get update && apt-get install -y bazel=5.2.0 
# RUN wget https://github.com/bazelbuild/bazelisk/releases/download/v1.20.0/bazelisk-linux-amd64 -o /usr/local/bin/bazel

WORKDIR /app

CMD ["/bin/sh", "-c", "bash"]