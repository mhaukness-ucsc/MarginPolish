FROM ubuntu:14.04
MAINTAINER Marina Haukness, mhauknes@ucsc.edu

ARG git_commit

# update and install dependencies
RUN apt-get update && \
    apt-get -y install time git make wget autoconf gcc g++ zlib1g-dev libcurl4-openssl-dev libbz2-dev libhdf5-dev && \
    apt-get clean && \
    apt-get purge && \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# install cmake
WORKDIR /tmp
RUN mkdir /opt/cmake && \
    wget https://cmake.org/files/v3.11/cmake-3.11.4-Linux-x86_64.sh && \
    sh /tmp/cmake-3.11.4-Linux-x86_64.sh --prefix=/opt/cmake --skip-license && \
    ln -s /opt/cmake/bin/cmake /usr/local/bin/cmake

# get marginPolish
WORKDIR /opt
RUN git clone https://github.com/mhaukness-ucsc/MarginPolish.git && \
    cd /opt/MarginPolish && \
    git fetch && \
    git checkout $git_commit && \
    git submodule update --init && \
    rm -rf /opt/MarginPolish/.git

WORKDIR /opt/MarginPolish/build
RUN cmake .. && make && ./marginPolish

# setup entrypoint
COPY wrapper.sh /opt/wrapper.sh
WORKDIR /data
ENTRYPOINT ["sh", "/opt/wrapper.sh"]
