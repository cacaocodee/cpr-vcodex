#pragma once
#include <HalStorage.h>

#include <cstring>
#include <iostream>

namespace serialization {
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

inline void readString(FsFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  s.resize(len);
  file.read(&s[0], len);
}

// Abstract byte source for the page deserialize chain: lets a page be parsed
// out of a RAM buffer (one SD read per page turn) while keeping streaming
// FsFile as the fallback under memory pressure.
class Reader {
 public:
  virtual ~Reader() = default;
  // Returns the number of bytes actually copied into dst.
  virtual size_t read(void* dst, size_t len) = 0;
};

class FileReader : public Reader {
 public:
  explicit FileReader(FsFile& file) : file_(file) {}
  size_t read(void* dst, size_t len) override {
    const int got = file_.read(reinterpret_cast<uint8_t*>(dst), len);
    return got < 0 ? 0 : static_cast<size_t>(got);
  }

 private:
  FsFile& file_;
};

class MemReader : public Reader {
 public:
  MemReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  size_t read(void* dst, size_t len) override {
    const size_t avail = size_ - pos_;
    const size_t take = len <= avail ? len : avail;
    if (take != len) overrun_ = true;
    if (take > 0) {
      memcpy(dst, data_ + pos_, take);
      pos_ += take;
    }
    return take;
  }
  bool overrun() const { return overrun_; }
  size_t remaining() const { return size_ - pos_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool overrun_ = false;
};

template <typename T>
void readPod(Reader& r, T& value) {
  if (r.read(&value, sizeof(T)) != sizeof(T)) {
    value = T{};
  }
}

// Cap protects against a corrupt length prefix triggering a huge (and with
// -fno-exceptions, fatal) resize; page blobs are at most tens of KB.
constexpr uint32_t MAX_SERIALIZED_STRING = 64u * 1024u;

inline void readString(Reader& r, std::string& s) {
  uint32_t len = 0;
  readPod(r, len);
  if (len > MAX_SERIALIZED_STRING) {
    s.clear();
    return;
  }
  s.resize(len);
  if (len > 0 && r.read(&s[0], len) != len) {
    s.clear();
  }
}
}  // namespace serialization
