#!/bin/bash

set -eu

do_run() {
  n="$1"
  ./app "$n" > "${n}.out" 2>&1
}

ns=(
  256
  512
  1024
  2048
  4096
  8192
  16384
  32768
  65536
  131072
  262144
  524288
  1048576
  2097152
  4194304
  8388608
)

for x in "${ns[@]}"; do
  echo "Running ${x}..."
  do_run "$x"
done
