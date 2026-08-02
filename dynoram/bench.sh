#!/bin/bash
set -Eeuxo pipefail

# All block sizes are adjusted to achieve the same ORAM block size.
# You can run each bench only with the block sizes you need to speed up the whole process.
num_runs=4
file_store_path=./test_data--delete_me
ram_block_sizes='32,152,248,4088'
map_block_sizes='11,131,227,4067'
stack_block_sizes="${ram_block_sizes}"
heap_block_sizes="${ram_block_sizes}"
min_nlg=1
max_nlg=26

## CSV Headers
rm -f ${file_store_path} 2>/dev/null
cmake-build-release/bin/print_csv_headers
## RAM
cmake-build-release/bin/time_static_path_oram "${num_runs}" "${min_nlg}" "${max_nlg}" "${ram_block_sizes}"
cmake-build-release/bin/time_all_but_alloc_dynamic_stepping_path_oram "${num_runs}" "${min_nlg}" "${max_nlg}" "${ram_block_sizes}"
## Map
cmake-build-release/bin/time_static_path_omap "${num_runs}" "${min_nlg}" "${max_nlg}" "${map_block_sizes}"
cmake-build-release/bin/time_all_but_alloc_dynamic_stepping_path_omap "${num_runs}" "${min_nlg}" "${max_nlg}" "${map_block_sizes}"
## Map (Disk)
cmake-build-release/bin/time_static_path_omap "${num_runs}" "${min_nlg}" "${max_nlg}" "${map_block_sizes}" "${file_store_path}"
cmake-build-release/bin/time_all_but_alloc_dynamic_stepping_path_omap "${num_runs}" "${min_nlg}" "${max_nlg}" "${map_block_sizes}" "${file_store_path}"
## Map (Hybrid Store)
for i in 2 3 4; do
  # 2: 50% memory; 3: 25% memory; 4: 12.5% memory.
  mml="$((max_nlg - i))"
  cmake-build-release/bin/time_static_path_omap "${num_runs}" "$((mml + 2))" "${max_nlg}" "${map_block_sizes}" "${file_store_path}" "${mml}"
  cmake-build-release/bin/time_all_but_alloc_dynamic_stepping_path_omap "${num_runs}" "${mml}" "${max_nlg}" "${map_block_sizes}" "${file_store_path}" "$((mml - 1))"
done
## Stack
cmake-build-release/bin/time_static_path_ostack "${num_runs}" "${min_nlg}" "${max_nlg}" "${stack_block_sizes}"
## Heap
cmake-build-release/bin/time_static_path_oheap "${num_runs}" "${min_nlg}" "${max_nlg}" "${heap_block_sizes}"
cmake-build-release/bin/time_all_but_alloc_dynamic_stepping_path_oheap "${num_runs}" "${min_nlg}" "${max_nlg}" "${heap_block_sizes}"
rm -f ${file_store_path} 2>/dev/null
