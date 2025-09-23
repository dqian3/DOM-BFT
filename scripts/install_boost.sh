#!/usr/bin/env bash

if [ ! -d "boost_1_85_0" ]; then
    wget https://archives.boost.io/release/1.85.0/source/boost_1_85_0.tar.gz
    tar -xzf boost_1_85_0.tar.gz
    (
      cd boost_1_85_0 || exit
      ./bootstrap.sh
      ./b2
      sudo ./b2 install --prefix=/usr/
    )
else
    echo "boost_1_85_0 already exists — skipping download/build."
fi
