# snsgxcxx

header-only compatibility shim for sgx enclaves:
- reenable libc++ atomics so we can use `std::atomic`
- tiny pthread-backed `std::mutex` / `std::lock_guard` / `std::unique_lock`
- minimalist implementations of `std::cout`, `std::ostringstream`
- explicit `sgxlib::` wrappers for sgx primitives

every translation unit will see `snsgxcxx`’s headers.

## usage

```cmake
add_subdirectory(path/to/src/compat/sgxcxx)
target_link_libraries(your_enclave_target PRIVATE snsgxcxx)
target_include_directories(your_enclave_target BEFORE PRIVATE
    $<TARGET_PROPERTY:snsgxcxx,INTERFACE_INCLUDE_DIRECTORIES>)
```

## implement bridge

```cpp
extern "C" void snsgxcxx_stream_sink(const char* msg) {
  ocall_print(msg);
}
```
