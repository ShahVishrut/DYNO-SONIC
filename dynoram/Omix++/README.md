# Omix++
This repository contains OMIX++ ported to SGX2. The open-source is based on the following paper:

Ghareh Chamani, Javad , Ioannis Demertzis, Dimitrios Papadopoulos, Charalampos Papamanthou, and Rasool Jalili. "GraphOS: Towards Oblivious Graph Processing." accepted in VLDB 2023

## Prepare
Starting from an Azure `DC4ds V3` VM running Ubuntu 20.04:

```bash
sudo apt update
sudo apt upgrade -y
sudo reboot -h now
# ...
# Maybe start a tmux session...
do-release-upgrade -f DistUpgradeViewNonInteractive
sudo reboot -h now
```
Now you have 22.04! Skip the `do-release-upgrade` if by the time you read this Azure has a 22.04 offering!
Then,
```bash
sudo apt update
sudo apt install -y build-essential python-is-python3 curl
sudo mkdir -p /etc/apt/keyrings
if ! [ -f /etc/apt/sources.list.d/intel-sgx.list ]; then
    echo 'deb [arch=amd64 signed-by=/etc/apt/keyrings/intel-sgx-deb.gpg] https://download.01.org/intel-sgx/sgx_repo/ubuntu jammy main' \
        | sudo tee /etc/apt/sources.list.d/intel-sgx.list
fi
if ! [ -f /etc/apt/keyrings/intel-sgx-deb.gpg ]; then
    curl -fsSL https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key \
        | gpg --dearmor \
        | sudo tee /etc/apt/keyrings/intel-sgx-deb.gpg >/dev/null
fi
wget https://download.01.org/intel-sgx/latest/linux-latest/distro/ubuntu22.04-server/sgx_linux_x64_sdk_2.23.100.2.bin
chmod +x sgx_linux_x64_sdk_2.23.100.2.bin
sudo ./sgx_linux_x64_sdk_2.23.100.2.bin --prefix /opt/intel/
sudo apt-get install -y libsgx-enclave-common-dev libsgx-dcap-ql-dev
```

In case of compilation errors, rebuild sgx-ssl:
```bash
wget -c https://github.com/intel/intel-sgx-ssl/archive/refs/tags/3.0_Rev2.tar.gz -O sgxssl.tgz
tar xf sgxssl.tgz --one-top-level=sgxssl-source --strip-components=1
wget -c https://openssl.org/source/old/3.0/openssl-3.0.12.tar.gz -P sgxssl-source/openssl_source/
make -C sgxssl-source/Linux sgxssl install DESTDIR=$(pwd)/sgxssl/
```

## Build
```bash
source /opt/intel/sgxsdk/environment
make
```

## Run
```bash
./app
```


# TODO:

Port to nix flakes...
