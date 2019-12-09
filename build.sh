#!/bin/bash

sudo apt install -y uuid-dev libndp-dev libsystemd-dev libjansson-dev libselinux1-dev libaudit-dev libpolkit-gobject-1-dev ppp-dev libmm-glib-dev libpsl-dev libcurl4-openssl-dev libnewt-dev libqt4-dev libreadline-dev
meson build --prefix=/usr  
cd build
ninja
