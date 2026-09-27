#include "h2_desktop_recording_output.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

h2_desktop_recording_output::h2_desktop_recording_output() = default;

h2_desktop_recording_output::~h2_desktop_recording_output() { (void)finish(); }

bool h2_desktop_recording_output::start(int fd) {
  fd_ = fd;
  try {
    bytes_.resize(capacity);
    writer_ = std::thread(&h2_desktop_recording_output::run, this);
  } catch (...) {
    (void)close(fd_);
    fd_ = -1;
    return false;
  }
  return true;
}

int h2_desktop_recording_output::append(const uint8_t *data, size_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (result_ != 0)
    return result_;
  if (closing_)
    return -EPIPE;
  if (size > capacity - count_)
    return -ENOBUFS;
  const size_t tail = (head_ + count_) % capacity;
  const size_t first = std::min(size, capacity - tail);
  std::memcpy(bytes_.data() + tail, data, first);
  std::memcpy(bytes_.data(), data + first, size - first);
  count_ += size;
  wake_.notify_one();
  return static_cast<int>(size);
}

int h2_desktop_recording_output::result() {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}

size_t h2_desktop_recording_output::pending() {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

int h2_desktop_recording_output::finish() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
    wake_.notify_one();
  }
  if (writer_.joinable())
    writer_.join();
  return result();
}

void h2_desktop_recording_output::run() {
  while (true) {
    size_t head, size;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return count_ != 0u || closing_; });
      if (count_ == 0u)
        break;
      head = head_;
      size = std::min({count_, capacity - head_, size_t{65536u}});
    }
    // The mutex is released across blocking I/O. append() can fill other slots.
    const ssize_t written = write(fd_, bytes_.data() + head, size);
    const int error = errno;
    if (written < 0 && error == EINTR)
      continue;
    std::lock_guard<std::mutex> lock(mutex_);
    if (written <= 0) {
      result_ = -(written < 0 && error != 0 ? error : EIO);
      break;
    }
    head_ = (head_ + static_cast<size_t>(written)) % capacity;
    count_ -= static_cast<size_t>(written);
  }
  const int closed = close(fd_);
  const int error = errno;
  fd_ = -1;
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed != 0 && result_ == 0)
    result_ = -(error != 0 ? error : EIO);
}
