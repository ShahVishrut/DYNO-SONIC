#pragma once

#include <cstdint>
#include <string>
#if !defined(SN_SGX_ENCLAVE)
#include <filesystem>
#endif

namespace sn::sgxbridge::fs {

enum class status : std::uint32_t {
  ok = 0,
  invalid_arguments = 1,
  io_error = 2,
};

struct remove_result {
  status code{status::ok};
  bool removed{false};
};

struct remove_all_result {
  status code{status::ok};
  std::uint64_t removed{0};
};

struct exists_result {
  status code{status::ok};
  bool exists{false};
};

struct directories_result {
  status code{status::ok};
  bool created{false};
};

struct rename_result {
  status code{status::ok};
  bool renamed{false};
};

struct copy_file_result {
  status code{status::ok};
  bool copied{false};
};

struct file_size_result {
  status code{status::ok};
  std::uint64_t size{0};
};

#if !defined(SN_SGX_ENCLAVE)

inline remove_result remove_file(const std::string& path) {
  remove_result out{};
  if (path.empty()) {
    out.code = status::invalid_arguments;
    return out;
  }
  std::error_code ec{};
  out.removed = std::filesystem::remove(path, ec);
  if (ec) {
    out.code = status::io_error;
  }
  return out;
}

inline remove_all_result remove_all(const std::string& path) {
  remove_all_result out{};
  if (path.empty()) {
    out.code = status::invalid_arguments;
    return out;
  }
  std::error_code ec{};
  out.removed = static_cast<std::uint64_t>(std::filesystem::remove_all(path, ec));
  if (ec) {
    out.code = status::io_error;
    out.removed = 0;
  }
  return out;
}

inline exists_result exists(const std::string& path) {
  exists_result out{};
  if (path.empty()) {
    out.code = status::invalid_arguments;
    return out;
  }
  std::error_code ec{};
  out.exists = std::filesystem::exists(path, ec);
  if (ec) {
    out.code = status::io_error;
    out.exists = false;
  }
  return out;
}

inline directories_result create_directories(const std::string& path) {
  directories_result out{};
  if (path.empty()) {
    out.code = status::invalid_arguments;
    return out;
  }
  std::error_code ec{};
  out.created = std::filesystem::create_directories(path, ec);
  if (ec) {
    out.code = status::io_error;
    out.created = false;
  }
  return out;
}

inline rename_result rename(const std::string& from, const std::string& to) {
  rename_result out{};
  if (from.empty() || to.empty()) {
    out.code = status::invalid_arguments;
    return out;
  }
  std::error_code ec{};
  std::filesystem::rename(from, to, ec);
  if (ec) {
    out.code = status::io_error;
    out.renamed = false;
  } else {
    out.renamed = true;
  }
  return out;
}

inline copy_file_result copy_file(const std::string& from, const std::string& to, bool overwrite_existing = false) {
  copy_file_result out{};
  if (from.empty() || to.empty()) {
    out.code = status::invalid_arguments;
    return out;
  }
  std::error_code ec{};
  const auto opt =
      overwrite_existing ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none;
  out.copied = std::filesystem::copy_file(from, to, opt, ec);
  if (ec) {
    out.code = status::io_error;
    out.copied = false;
  }
  return out;
}

inline file_size_result file_size(const std::string& path) {
  file_size_result out{};
  if (path.empty()) {
    out.code = status::invalid_arguments;
    return out;
  }
  std::error_code ec{};
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    out.code = status::io_error;
    out.size = 0;
  } else {
    out.size = static_cast<std::uint64_t>(size);
  }
  return out;
}

#else

remove_result remove_file(const std::string& path);
remove_all_result remove_all(const std::string& path);
exists_result exists(const std::string& path);
directories_result create_directories(const std::string& path);
rename_result rename(const std::string& from, const std::string& to);
copy_file_result copy_file(const std::string& from, const std::string& to, bool overwrite_existing = false);
file_size_result file_size(const std::string& path);

#endif

}
