FROM --platform=linux/amd64 ubuntu:12.04
RUN sed -i 's#http://[a-z.]*ubuntu.com#http://old-releases.ubuntu.com#g' /etc/apt/sources.list && \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --force-yes \
      gcc-4.6-arm-linux-gnueabi binutils-arm-linux-gnueabi build-essential \
      u-boot-tools lzop bc libncurses5-dev git && \
    ln -s /usr/bin/arm-linux-gnueabi-gcc-4.6 /usr/bin/arm-linux-gnueabi-gcc
WORKDIR /work
