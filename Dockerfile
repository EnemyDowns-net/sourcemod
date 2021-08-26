FROM ubuntu:20.04

# Author
LABEL maintainer="maxime1907 <maxime1907.dev@gmail.com>"

ARG DEBIAN_FRONTEND=noninteractive

# Install dependencies for SourceMod
RUN dpkg --add-architecture i386 && \
    apt update && apt install -y \
        clang \
        g++-multilib \
        lib32stdc++-7-dev \
        lib32z1-dev \
        libc6-dev-i386 \
        linux-libc-dev:i386 \
        curl \
        git \
        python3 \
        python3-pip \
        python3-setuptools \
        automake \
        cmake \
        libtool

RUN mkdir -p /home/alliedmodders/sourcemod/build
COPY . /home/alliedmodders/sourcemod

WORKDIR /home/alliedmodders

RUN bash /home/alliedmodders/sourcemod/tools/checkout-deps.sh

# Fix ambuild installation
WORKDIR /home/alliedmodders/ambuild

RUN python3 setup.py install

WORKDIR /home/alliedmodders/sourcemod/build

RUN python3 /home/alliedmodders/sourcemod/configure.py --enable-optimize --enable-asan --sdks=csgo,css

RUN CLANG_DEFAULT_CXX_STDLIB=libc++ ambuild
