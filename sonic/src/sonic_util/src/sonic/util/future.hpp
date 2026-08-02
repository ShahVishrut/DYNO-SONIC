#pragma once

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if !(defined(SONIC_NO_OS) && SONIC_NO_OS)
#include <condition_variable>
#include <mutex>
#endif

#if defined(SONIC_NO_OS) && SONIC_NO_OS
#define SN_UTIL_FUTURE_HAS_STD_EXCEPTION_PTR 0
#else
#define SN_UTIL_FUTURE_HAS_STD_EXCEPTION_PTR 1
#endif
namespace sn::util {

class future_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

template <typename T> class promise;
template <typename T> class future;

namespace detail {

template <typename T> class shared_state;

class exception_payload {
public:
  exception_payload() = default;

#if SN_UTIL_FUTURE_HAS_STD_EXCEPTION_PTR
  explicit exception_payload(std::exception_ptr ex) : impl_(std::make_shared<std_exception_model>(std::move(ex))) {}
#endif

  template <typename E> static exception_payload capture(E&& value) {
    using decayed = std::decay_t<E>;
    return exception_payload(std::make_shared<model<decayed>>(std::forward<E>(value)));
  }

  bool has_value() const noexcept { return static_cast<bool>(impl_); }

  void rethrow() const {
    if (impl_) {
      impl_->rethrow();
    }
  }

private:
  struct interface_ {
    virtual ~interface_() = default;
    virtual void rethrow() const = 0;
  };

#if SN_UTIL_FUTURE_HAS_STD_EXCEPTION_PTR
  struct std_exception_model : interface_ {
    std::exception_ptr ptr;
    explicit std_exception_model(std::exception_ptr p) : ptr(std::move(p)) {}
    void rethrow() const override { std::rethrow_exception(ptr); }
  };
#endif

  template <typename E> struct model : interface_ {
    E value;
    explicit model(E&& v) : value(std::forward<E>(v)) {}
    void rethrow() const override { throw value; }
  };

  explicit exception_payload(std::shared_ptr<const interface_> impl) : impl_(std::move(impl)) {}

  std::shared_ptr<const interface_> impl_;
};

#if !(defined(SONIC_NO_OS) && SONIC_NO_OS)

template <typename T> class shared_state {
public:
  shared_state() = default;

  template <typename U> void set_value(U&& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_not_ready();
    value_.emplace(std::forward<U>(value));
    ready_ = true;
    cv_.notify_all();
  }

  void set_exception(exception_payload ex) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_not_ready();
    exception_ = std::move(ex);
    ready_ = true;
    cv_.notify_all();
  }

  void wait() const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return ready_; });
  }

  bool is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
  }

  T take_value() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return ready_; });
    if (exception_.has_value()) {
      auto ex = exception_;
      lock.unlock();
      exception_ = exception_payload{};
      ex.rethrow();
    }
    if (!value_) {
      throw future_error("future has no value");
    }
    auto result = std::move(*value_);
    value_.reset();
    return result;
  }

private:
  void ensure_not_ready() const {
    if (ready_) {
      throw future_error("promise already satisfied");
    }
  }

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  bool ready_ = false;
  std::optional<T> value_;
  exception_payload exception_;
};

template <> class shared_state<void> {
public:
  shared_state() = default;

  void set_value() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_not_ready();
    ready_ = true;
    cv_.notify_all();
  }

  void set_exception(exception_payload ex) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_not_ready();
    exception_ = std::move(ex);
    ready_ = true;
    cv_.notify_all();
  }

  void wait() const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return ready_; });
  }

  bool is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
  }

  void take_value() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return ready_; });
    if (exception_.has_value()) {
      auto ex = exception_;
      lock.unlock();
      exception_ = exception_payload{};
      ex.rethrow();
    }
  }

private:
  void ensure_not_ready() const {
    if (ready_) {
      throw future_error("promise already satisfied");
    }
  }

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  bool ready_ = false;
  exception_payload exception_;
};

#else

template <typename T> class shared_state {
public:
  shared_state() = default;

  template <typename U> void set_value(U&& value) {
    ensure_not_ready();
    value_.emplace(std::forward<U>(value));
    ready_ = true;
  }

  void set_exception(exception_payload ex) {
    ensure_not_ready();
    exception_ = std::move(ex);
    ready_ = true;
  }

  void wait() const noexcept {}

  bool is_ready() const noexcept { return ready_; }

  T take_value() {
    if (!ready_) {
      throw future_error("future not ready");
    }
    if (exception_.has_value()) {
      auto ex = exception_;
      exception_ = exception_payload{};
      ex.rethrow();
    }
    if (!value_) {
      throw future_error("future has no value");
    }
    auto result = std::move(*value_);
    value_.reset();
    return result;
  }

private:
  void ensure_not_ready() const {
    if (ready_) {
      throw future_error("promise already satisfied");
    }
  }

  bool ready_ = false;
  std::optional<T> value_;
  exception_payload exception_;
};

template <> class shared_state<void> {
public:
  shared_state() = default;

  void set_value() {
    ensure_not_ready();
    ready_ = true;
  }

  void set_exception(exception_payload ex) {
    ensure_not_ready();
    exception_ = std::move(ex);
    ready_ = true;
  }

  void wait() const noexcept {}

  bool is_ready() const noexcept { return ready_; }

  void take_value() {
    if (!ready_) {
      throw future_error("future not ready");
    }
    if (exception_.has_value()) {
      auto ex = exception_;
      exception_ = exception_payload{};
      ex.rethrow();
    }
  }

private:
  void ensure_not_ready() const {
    if (ready_) {
      throw future_error("promise already satisfied");
    }
  }

  bool ready_ = false;
  exception_payload exception_;
};

#endif

}

template <typename T> class future {
public:
  future() = default;
  future(future&&) noexcept = default;
  future& operator=(future&&) noexcept = default;

  future(const future&) = delete;
  future& operator=(const future&) = delete;

  bool valid() const noexcept { return static_cast<bool>(state_); }

  bool is_ready() const {
    if (!state_) {
      return false;
    }
    return state_->is_ready();
  }

  void wait() const {
    ensure_state();
    state_->wait();
  }

  T get() {
    ensure_state();
    auto result = state_->take_value();
    state_.reset();
    return result;
  }

private:
  explicit future(std::shared_ptr<detail::shared_state<T>> state) : state_(std::move(state)) {}

  void ensure_state() const {
    if (!state_) {
      throw future_error("future has no shared state");
    }
  }

  std::shared_ptr<detail::shared_state<T>> state_;

  friend class promise<T>;
};

template <> class future<void> {
public:
  future() = default;
  future(future&&) noexcept = default;
  future& operator=(future&&) noexcept = default;

  future(const future&) = delete;
  future& operator=(const future&) = delete;

  bool valid() const noexcept { return static_cast<bool>(state_); }

  bool is_ready() const {
    if (!state_) {
      return false;
    }
    return state_->is_ready();
  }

  void wait() const {
    ensure_state();
    state_->wait();
  }

  void get() {
    ensure_state();
    state_->take_value();
    state_.reset();
  }

private:
  explicit future(std::shared_ptr<detail::shared_state<void>> state) : state_(std::move(state)) {}

  void ensure_state() const {
    if (!state_) {
      throw future_error("future has no shared state");
    }
  }

  std::shared_ptr<detail::shared_state<void>> state_;

  friend class promise<void>;
};

template <typename T> class promise {
public:
  promise() : state_(std::make_shared<detail::shared_state<T>>()) {}
  ~promise() {
    if (state_ && !state_->is_ready()) {
      try {
        state_->set_exception(detail::exception_payload::capture(future_error("broken promise")));
      } catch (...) {
      }
    }
  }

  promise(promise&&) noexcept = default;
  promise& operator=(promise&&) noexcept = default;

  promise(const promise&) = delete;
  promise& operator=(const promise&) = delete;

  future<T> get_future() {
    if (future_retrieved_) {
      throw future_error("get_future called more than once");
    }
    future_retrieved_ = true;
    return future<T>(state_);
  }

  template <typename U = T, typename std::enable_if<!std::is_void<U>::value, int>::type = 0> void set_value(U&& value) {
    assert_state();
    state_->set_value(std::forward<U>(value));
    state_.reset();
  }

  void set_exception(detail::exception_payload ex) {
    assert_state();
    state_->set_exception(std::move(ex));
    state_.reset();
  }

private:
  void assert_state() {
    if (!state_) {
      throw future_error("promise has no shared state");
    }
  }

  std::shared_ptr<detail::shared_state<T>> state_;
  bool future_retrieved_ = false;
};

template <> class promise<void> {
public:
  promise() : state_(std::make_shared<detail::shared_state<void>>()) {}
  ~promise() {
    if (state_ && !state_->is_ready()) {
      try {
        state_->set_exception(detail::exception_payload::capture(future_error("broken promise")));
      } catch (...) {
      }
    }
  }

  promise(promise&&) noexcept = default;
  promise& operator=(promise&&) noexcept = default;

  promise(const promise&) = delete;
  promise& operator=(const promise&) = delete;

  future<void> get_future() {
    if (future_retrieved_) {
      throw future_error("get_future called more than once");
    }
    future_retrieved_ = true;
    return future<void>(state_);
  }

  void set_value() {
    assert_state();
    state_->set_value();
    state_.reset();
  }

  void set_exception(detail::exception_payload ex) {
    assert_state();
    state_->set_exception(std::move(ex));
    state_.reset();
  }

private:
  void assert_state() {
    if (!state_) {
      throw future_error("promise has no shared state");
    }
  }

  std::shared_ptr<detail::shared_state<void>> state_;
  bool future_retrieved_ = false;
};

template <typename T> future<typename std::decay<T>::type> make_ready_future(T&& value) {
  promise<typename std::decay<T>::type> prom;
  auto fut = prom.get_future();
  prom.set_value(std::forward<T>(value));
  return fut;
}

inline future<void> make_ready_future() {
  promise<void> prom;
  auto fut = prom.get_future();
  prom.set_value();
  return fut;
}

}
