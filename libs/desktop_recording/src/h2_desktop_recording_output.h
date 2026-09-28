#ifndef H2_DESKTOP_RECORDING_OUTPUT_H
#define H2_DESKTOP_RECORDING_OUTPUT_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// Private sequential MP4 sink. The encoder never waits for a device write.
// Pending bytes include the in-flight write and cannot be reused until it ends.
class h2_desktop_recording_output {
public:
  static constexpr size_t capacity = 4u * 1024u * 1024u;
  h2_desktop_recording_output();
  ~h2_desktop_recording_output();
  // Takes ownership of fd, including on thread creation failure.
  bool start(int fd);
  // Copies all bytes or returns a negative errno; never partially accepts.
  int append(const uint8_t *data, size_t size);
  int result();
  size_t pending();
  // Drain accepted bytes, close fd, and join. Idempotent, control thread only.
  int finish();

private:
  void run();
  std::vector<uint8_t> bytes_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread writer_;
  size_t head_ = 0u;
  size_t count_ = 0u;
  bool closing_ = false;
  int result_ = 0;
  int fd_ = -1;
};

#endif
