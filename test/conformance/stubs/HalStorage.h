#pragma once

// Host-side stub of the firmware's HalStorage/HalFile for the .cpmx
// conformance test: just enough surface for CpmxReader, backed by stdio.
// The real header lives in lib/hal/ and wraps SdFat behind a mutex.

#include <cstdint>
#include <cstdio>
#include <string>

class HalFile {
 public:
  ~HalFile() { close(); }

  bool openRead(const char* path) {
    close();
    f_ = fopen(path, "rb");
    return f_ != nullptr;
  }

  explicit operator bool() const { return f_ != nullptr; }

  size_t size() {
    if (!f_) return 0;
    const long cur = ftell(f_);
    fseek(f_, 0, SEEK_END);
    const long end = ftell(f_);
    fseek(f_, cur, SEEK_SET);
    return static_cast<size_t>(end);
  }

  bool seek(size_t pos) { return f_ && fseek(f_, static_cast<long>(pos), SEEK_SET) == 0; }

  int read(void* buf, size_t count) {
    if (!f_) return -1;
    return static_cast<int>(fread(buf, 1, count, f_));
  }

  void close() {
    if (f_) {
      fclose(f_);
      f_ = nullptr;
    }
  }

 private:
  FILE* f_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char* /*moduleName*/, const char* path, HalFile& file) {
    return file.openRead(path);
  }
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }
};

#define Storage HalStorage::getInstance()
