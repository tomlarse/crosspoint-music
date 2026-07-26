#include "Setlist.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

namespace {
// Collapse "." and ".." segments so human-written entries like
// "../marches/x.cpmx" reach the SD layer as a plain absolute path.
std::string normalizePath(const std::string& path) {
  std::string out;
  out.reserve(path.size());
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') {
      i++;
    }
    size_t j = path.find('/', i);
    if (j == std::string::npos) {
      j = path.size();
    }
    const size_t len = j - i;
    if (len == 0 || (len == 1 && path[i] == '.')) {
      // skip empty and "." segments
    } else if (len == 2 && path[i] == '.' && path[i + 1] == '.') {
      const size_t slash = out.rfind('/');
      out.erase(slash == std::string::npos ? 0 : slash);
    } else {
      out += '/';
      out.append(path, i, len);
    }
    i = j;
  }
  return out.empty() ? "/" : out;
}
}  // namespace

bool Setlist::load(const std::string& setlistPath) {
  paths_.clear();

  HalFile file;
  if (!Storage.openFileForRead("CPSL", setlistPath, file)) {
    LOG_ERR("CPSL", "Cannot open %s", setlistPath.c_str());
    return false;
  }
  const size_t size = file.size();
  if (size == 0 || size > MAX_FILE_SIZE) {
    LOG_ERR("CPSL", "Setlist size %u outside (0, %u]", static_cast<unsigned>(size),
            static_cast<unsigned>(MAX_FILE_SIZE));
    return false;
  }

  const auto buffer = makeUniqueNoThrow<char[]>(size + 1);
  if (!buffer) {
    LOG_ERR("CPSL", "OOM: %u bytes", static_cast<unsigned>(size + 1));
    return false;
  }
  if (file.read(buffer.get(), size) != static_cast<int>(size)) {
    LOG_ERR("CPSL", "Short read");
    return false;
  }
  buffer[size] = '\0';

  const std::string folder = FsHelpers::extractFolderPath(setlistPath);
  // Heap use is bounded: at most MAX_PIECES short path strings out of an
  // <=8KB file (the file browser holds a whole folder in the same
  // std::string form). A failed allocation aborts like any STL growth in
  // this codebase; the reserve makes vector growth one up-front allocation.
  paths_.reserve(MAX_PIECES);

  const char* p = buffer.get();
  while (*p != '\0') {
    // Take one line, tolerating CRLF from desktop-edited files.
    const char* lineStart = p;
    while (*p != '\0' && *p != '\n') {
      p++;
    }
    const char* lineEnd = p;
    if (*p == '\n') {
      p++;
    }
    while (lineEnd > lineStart && (lineEnd[-1] == '\r' || lineEnd[-1] == ' ' || lineEnd[-1] == '\t')) {
      lineEnd--;
    }
    while (lineStart < lineEnd && (*lineStart == ' ' || *lineStart == '\t')) {
      lineStart++;
    }
    if (lineStart == lineEnd || *lineStart == '#') {
      continue;
    }

    std::string entry(lineStart, static_cast<size_t>(lineEnd - lineStart));
    if (!FsHelpers::hasCpmxExtension(entry)) {
      LOG_ERR("CPSL", "Skipping non-.cpmx entry: %s", entry.c_str());
      continue;
    }
    if (paths_.size() >= MAX_PIECES) {
      LOG_ERR("CPSL", "Setlist truncated at %u pieces", static_cast<unsigned>(MAX_PIECES));
      break;
    }
    if (entry[0] == '/') {
      paths_.push_back(normalizePath(entry));
    } else {
      paths_.push_back(normalizePath((folder == "/" ? "" : folder) + "/" + entry));
    }
  }

  if (paths_.empty()) {
    LOG_ERR("CPSL", "No usable entries in %s", setlistPath.c_str());
    return false;
  }
  LOG_INF("CPSL", "Loaded setlist %s: %u pieces", setlistPath.c_str(), static_cast<unsigned>(paths_.size()));
  return true;
}

uint32_t Setlist::fingerprint() const {
  uint32_t h = 2166136261u;
  for (const auto& path : paths_) {
    for (const char c : path) {
      // cppcheck-suppress useStlAlgorithm
      h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    h = (h ^ 0xFFu) * 16777619u;  // separator: "ab","c" != "a","bc"
  }
  return h;
}
