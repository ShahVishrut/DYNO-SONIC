#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sn::ds {

template <typename T> class vector {
  static_assert(!std::is_void_v<T>, "sn::ds::vector<T>: T must not be void");

public:
  using value_type = T;
  using allocator_type = std::allocator<value_type>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using iterator = pointer;
  using const_iterator = const_pointer;

  vector() noexcept : data_(nullptr), size_(0), capacity_(0) {}

  explicit vector(size_type count) : vector() { resize(count); }

  vector(std::initializer_list<value_type> init_list) : vector() {
    reserve(init_list.size());
    for (const auto& element : init_list) {
      emplace_back(element);
    }
  }

  vector(const vector& other) : vector() {
    reserve(other.size_);
    for (const auto& element : other) {
      emplace_back(element);
    }
  }

  vector(vector&& other) noexcept : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
    other.data_ = nullptr;
    other.size_ = other.capacity_ = 0;
  }

  ~vector() {
    destroy_all();
    deallocate();
  }

  vector& operator=(const vector& other) {
    if (this != &other) {
      vector temp(other);
      swap(temp);
    }
    return *this;
  }

  vector& operator=(vector&& other) noexcept {
    if (this != &other) {
      destroy_all();
      deallocate();
      data_ = other.data_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      other.data_ = nullptr;
      other.size_ = other.capacity_ = 0;
    }
    return *this;
  }

  constexpr size_type size() const noexcept { return size_; }
  constexpr size_type capacity() const noexcept { return capacity_; }
  constexpr bool empty() const noexcept { return size_ == 0; }

  reference operator[](size_type index) { return data_[index]; }
  const_reference operator[](size_type index) const { return data_[index]; }

  reference at(size_type index) {
    if (index >= size_) {
      throw std::out_of_range("sn::ds::vector::at: index out of range");
    }
    return data_[index];
  }

  const_reference at(size_type index) const {
    if (index >= size_) {
      throw std::out_of_range("sn::ds::vector::at: index out of range");
    }
    return data_[index];
  }

  reference front() {
    assert(!empty());
    return data_[0];
  }

  const_reference front() const {
    assert(!empty());
    return data_[0];
  }

  reference back() {
    assert(!empty());
    return data_[size_ - 1];
  }

  const_reference back() const {
    assert(!empty());
    return data_[size_ - 1];
  }

  iterator begin() noexcept { return data_; }
  const_iterator begin() const noexcept { return data_; }
  const_iterator cbegin() const noexcept { return data_; }

  iterator end() noexcept { return data_ + size_; }
  const_iterator end() const noexcept { return data_ + size_; }
  const_iterator cend() const noexcept { return data_ + size_; }

  pointer data() noexcept { return data_; }
  const_pointer data() const noexcept { return data_; }

  void reserve(size_type new_capacity) {
    if (new_capacity > capacity_) {
      reallocate(new_capacity);
    }
  }

  void resize(size_type new_size) {
    if (new_size < size_) {
      destroy_range(data_ + new_size, data_ + size_);
    } else if (new_size > size_) {
      reserve(new_size);
      for (size_type i = size_; i < new_size; ++i) {
        alloc_traits::construct(allocator_, data_ + i);
      }
    }
    size_ = new_size;
  }

  void clear() {
    destroy_range(data_, data_ + size_);
    size_ = 0;
  }

  template <typename... Args> reference emplace_back(Args&&... args) {
    ensure_capacity_for_insert();
    std::allocator_traits<allocator_type>::construct(allocator_, data_ + size_, std::forward<Args>(args)...);
    ++size_;
    return back();
  }

  reference push_back(const value_type& value) { return emplace_back(value); }
  reference push_back(value_type&& value) { return emplace_back(std::move(value)); }

  void pop_back() {
    assert(!empty());
    std::allocator_traits<allocator_type>::destroy(allocator_, data_ + size_ - 1);
    --size_;
  }

  void swap(vector& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(allocator_, other.allocator_);
  }

  friend void swap(vector& lhs, vector& rhs) noexcept { lhs.swap(rhs); }

private:
  using alloc_traits = std::allocator_traits<allocator_type>;

  pointer data_;
  size_type size_;
  size_type capacity_;
  allocator_type allocator_;

  void destroy_all() {
    destroy_range(data_, data_ + size_);
    size_ = 0;
  }

  void destroy_range(pointer first, pointer last) {
    while (last != first) {
      --last;
      alloc_traits::destroy(allocator_, last);
    }
  }

  void deallocate() {
    if (data_) {
      alloc_traits::deallocate(allocator_, data_, capacity_);
      data_ = nullptr;
      capacity_ = 0;
    }
  }

  void ensure_capacity_for_insert() {
    if (size_ == capacity_) {
      size_type target = capacity_ == 0 ? 1 : capacity_ * 2;
      reallocate(target);
    }
  }

  void reallocate(size_type new_capacity) {
    pointer new_data = alloc_traits::allocate(allocator_, new_capacity);
    size_type old_size = size_;
    try {
      std::uninitialized_move_n(data_, size_, new_data);
    } catch (...) {
      alloc_traits::deallocate(allocator_, new_data, new_capacity);
      throw;
    }
    destroy_range(data_, data_ + old_size);
    deallocate();
    data_ = new_data;
    capacity_ = new_capacity;
    size_ = old_size;
  }
};

}
