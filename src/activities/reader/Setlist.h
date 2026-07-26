#pragma once

#include <cstddef>
#include <string>
#include <vector>

/// A .cpsl setlist: an ordered list of .cpmx pieces the music reader flows
/// through — the "march order" for a gig.
///
/// File format (deliberately human-writable until the library app exists):
/// UTF-8 text, one .cpmx path per line. Lines starting with '#' and blank
/// lines are ignored; CRLF is tolerated. Paths starting with '/' are
/// absolute on the SD card; anything else resolves relative to the
/// setlist's own folder.
class Setlist {
 public:
  static constexpr size_t MAX_PIECES = 64;
  static constexpr size_t MAX_FILE_SIZE = 8 * 1024;

  /// Load and resolve a .cpsl file. Returns false when the file cannot be
  /// read or contains no usable entries; logs specifics.
  bool load(const std::string& setlistPath);

  size_t count() const { return paths_.size(); }
  const std::string& pieceAt(size_t index) const { return paths_[index]; }

 private:
  std::vector<std::string> paths_;
};
