#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sn::util::sgx {

inline std::string default_enclave_path(std::string_view enclave_filename) {
  std::error_code ec;
  const auto exe_path = std::filesystem::canonical("/proc/self/exe", ec);
  if (ec) {
    return std::string(enclave_filename);
  }
  return (exe_path.parent_path() / enclave_filename).string();
}

}
