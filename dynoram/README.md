# DynO: Dynamic Oblivious Primitives

Using this guide you should be able to reproduce our results.

## 1. Prepare

You need [Conan](https://conan.io), [CMake](https://cmake.org), [Ninja](https://ninja-build.org), and a C++ compiler (we
use [CLang](https://clang.llvm.org)).

### Install requirements (Ubuntu):

```bash
sudo apt update
#sudo apt upgrade # It's generally good to do this; especially on a fresh VM.
sudo apt install -y cmake lsb-release ninja-build python3-pip software-properties-common wget
pip install conan # Installs to ~/.local/bin
echo 'PATH="$HOME/.local/bin:$PATH"' >> ~/.profile
. ~/.profile
wget https://apt.llvm.org/llvm.sh \
 && chmod +x llvm.sh \
 && sudo ./llvm.sh 14 \
 && rm llvm.sh
```

### What we used:

Ubuntu 22.04.1 on x86\_64; clang 14

```bash
$ uname -srvp
Linux 5.15.0-1021-aws #25-Ubuntu SMP Fri Sep 23 12:20:42 UTC 2022 x86_64
$ clang++-14 --version
Ubuntu clang version 14.0.6-++20220816122211+f28c006a5895-1~exp1~20220816122246.108
```

## 2. Build

Get this repository, `cd` into the project directory, and run:

```bash
make build
```

If all goes well, the executable(s) will be in `cmake-build-release/bin/`.

## 3. Run

Go back to the project root and run [`bench.sh`](./bench.sh).

```bash
cd .. # Assuming you're still in cmake-build-release
./bench.sh
```
