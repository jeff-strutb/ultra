#pragma once
// Save-state visitor. See docs/adr/0003-savestate.md.
//
// Every module implements `void serialize(SaveState&)` and visits all of its mutable state in
// a fixed order. The same function both saves and loads: in save mode each visit appends the
// value to the buffer, in load mode it overwrites the value from the buffer. Modules wrap their
// fields in a section() carrying a name and version so that a mismatched or reordered state
// fails loudly instead of loading garbage.

#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "common/types.hpp"

namespace reality {

class SaveState {
 public:
  enum class Mode : u8 { Save, Load };

  static constexpr u32 kMagic = 0x524C5459;  // "RLTY"
  static constexpr u32 kFormatVersion = 1;

  static SaveState saver() { return SaveState(Mode::Save); }
  static SaveState loader(std::span<const u8> data) {
    SaveState s(Mode::Load);
    s.buf_.assign(data.begin(), data.end());
    return s;
  }

  Mode mode() const noexcept { return mode_; }
  bool loading() const noexcept { return mode_ == Mode::Load; }
  const std::vector<u8>& data() const noexcept { return buf_; }

  void header() {
    u32 magic = kMagic, ver = kFormatVersion;
    value(magic);
    value(ver);
    if (loading() && (magic != kMagic || ver != kFormatVersion))
      throw std::runtime_error("save state: bad magic or format version");
  }

  // Marks the start of a module's state. The name is hashed (FNV-1a) so the on-disk record
  // is fixed-size; a mismatch on load means the producer and consumer disagree on layout.
  void section(std::string_view name, u32 version) {
    u32 h = fnv1a(name), v = version;
    value(h);
    value(v);
    if (loading() && (h != fnv1a(name) || v != version))
      throw std::runtime_error("save state: section mismatch at '" + std::string(name) + "'");
  }

  template <typename T>
    requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
  void value(T& v) {
    bytes(reinterpret_cast<u8*>(&v), sizeof(T));
  }

  template <typename T, usize N>
    requires std::is_trivially_copyable_v<T>
  void array(std::array<T, N>& a) {
    bytes(reinterpret_cast<u8*>(a.data()), sizeof(T) * N);
  }

  void bytes(u8* p, usize n) {
    if (mode_ == Mode::Save) {
      buf_.insert(buf_.end(), p, p + n);
    } else {
      if (pos_ + n > buf_.size()) throw std::runtime_error("save state: truncated");
      std::memcpy(p, buf_.data() + pos_, n);
      pos_ += n;
    }
  }

  void finish() {
    if (loading() && pos_ != buf_.size()) throw std::runtime_error("save state: trailing data");
  }

 private:
  explicit SaveState(Mode m) : mode_(m) {}

  static constexpr u32 fnv1a(std::string_view s) noexcept {
    u32 h = 2166136261u;
    for (char c : s) {
      h ^= static_cast<u8>(c);
      h *= 16777619u;
    }
    return h;
  }

  Mode mode_;
  std::vector<u8> buf_;
  usize pos_ = 0;
};

}  // namespace reality
