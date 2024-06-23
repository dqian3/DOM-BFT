apt-get update && apt-get install -y \
    apt-transport-https \
    curl \
    gnupg \
    curl \
    libssl-dev \
    git-all \
    build-essential 

curl -fsSL https://bazel.build/bazel-release.pub.gpg | gpg --dearmor > bazel.gpg \
    && mv bazel.gpg /etc/apt/trusted.gpg.d/ \
    && echo "deb [arch=amd64] https://storage.googleapis.com/bazel-apt stable jdk1.8" >/etc/apt/sources.list.d/bazel.list \
    && curl https://bazel.build/bazel-release.pub.gpg | apt-key add - \
    && apt-get update && apt-get install -y bazel=5.2.0 

git clone -b sender_sliding_window https://github.com/dqian3/DOM-BFT

cd DOM-BFT

bazel build //processes/...