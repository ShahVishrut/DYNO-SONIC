# SONIC Artifact

## Native non-SGX (Development only)

```sh
docker build -t localhost/sonic2-native -f docker/sonic2_native.docker .
docker run --rm -it -v "$PWD":/prj localhost/sonic2-native
```

```sh
CC=clang CXX=clang++ cmake -G Ninja -B build-clang-release -DCMAKE_BUILD_TYPE=Release -DSONIC_BUILD_APPLICATIONS=ON
cmake --build build-clang-release --parallel
```

## SGX

```sh
docker build -t localhost/sonic2-sgx -f docker/sonic2_sgx.docker .
docker run --privileged --device /dev/sgx_enclave:/dev/sgx_enclave --device /dev/sgx_provision:/dev/sgx_provision --rm -it -v "$PWD":/prj localhost/sonic2-sgx bash
```

```sh
CC=clang CXX=clang++ cmake -G Ninja -B build-sgx-clang -DCMAKE_BUILD_TYPE=Release -DSONIC_PLATFORM=sgx -DSONIC_DEMO_SGX_MODE=HW -DSONIC_BUILD_APPLICATIONS=ON -DSONIC_BUILD_DISTRIBUTED=ON
cmake --build build-sgx-clang --parallel
```

```sh
./build-sgx-clang/bin/sonic_demo_sgx -h
```

## Run GraphMap

```sh
./build-sgx-clang/bin/sonic_demo_sgx pathoram
```

```sh
./build-clang-release/bin/sonic_oram_demo pathoram
```

## Run SONIC

```sh
./build-sgx-clang/bin/sonic_demo_sgx zingoram
./build-sgx-clang/bin/sonic_demo_sgx zingoram-disjoint
```

```sh
./build-clang-release/bin/sonic_oram_demo zingoram
./build-clang-release/bin/sonic_oram_demo zingoram-disjoint
```
