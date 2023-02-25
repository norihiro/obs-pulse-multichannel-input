# OBS PulseAudio Multi-Channel Input Source

## Introduction

This plugin provides a modified PulseAudio Input for OBS Studio.
Some audio interfaces have more than 2 channels and you may want to route
arbitrary input channels.

## Features
- Map any input channels to OBS's channels.

## Build and install

Use cmake to build on Ubuntu. After checkout, run these commands.
```
mkdir build && cd build
cmake \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_LIBDIR=/usr/lib \
    ..
make -j4
sudo make install
```
You might need to adjust `CMAKE_INSTALL_LIBDIR` for your system.
