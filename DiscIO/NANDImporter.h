// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace DiscIO
{
class NANDImporter final
{
public:
  NANDImporter();
  ~NANDImporter();

  // Extract a NAND image to the configured NAND root.
  // If the associated OTP/SEEPROM dump (keys.bin) is not included in the image,
  // get_otp_dump_path will be called to get a path to it.
  void ImportNANDBin(const std::string& path_to_bin, std::function<void()> update_callback,
                     std::function<std::string()> get_otp_dump_path);
  bool ExtractCertificates();

  enum class Type
  {
    File = 1,
    Directory = 2,
  };

#pragma pack(push, 1)
  struct NANDFSTEntry
  {
    char name[12];
    u8 mode;
    u8 attr;
    Common::BigEndianValue<u16> sub;
    Common::BigEndianValue<u16> sib;
    Common::BigEndianValue<u32> size;
    Common::BigEndianValue<u32> uid;
    Common::BigEndianValue<u16> gid;
    Common::BigEndianValue<u32> x3;
  };
  static_assert(sizeof(NANDFSTEntry) == 0x20, "Wrong size");

  struct NANDSuperblock
  {
    char magic[4];  // "SFFS"
    Common::BigEndianValue<u32> version;
    Common::BigEndianValue<u32> unknown;
    Common::BigEndianValue<u16> fat[0x8000];
    NANDFSTEntry fst[0x17FF];
    u8 pad[0x14];
  };
  static_assert(sizeof(NANDSuperblock) == 0x40000, "Wrong size");
#pragma pack(pop)

private:
  bool ReadNANDBin(const std::string& path_to_bin, std::function<std::string()> get_otp_dump_path);
  bool FindSuperblock();
  std::string GetPath(const NANDFSTEntry& entry, const std::string& parent_path);
  std::string FormatDebugString(const NANDFSTEntry& entry);
  void ProcessEntry(u16 entry_number, const std::string& parent_path);
  std::vector<u8> GetEntryData(const NANDFSTEntry& entry);
  void ExportKeys();

  std::string m_nand_root;
  std::vector<u8> m_nand;
  std::vector<u8> m_nand_keys;
  std::array<u8, 16> m_aes_key;
  std::unique_ptr<NANDSuperblock> m_superblock;
  std::function<void()> m_update_callback;
};
}  // namespace DiscIO

template <>
struct fmt::formatter<DiscIO::NANDImporter::NANDFSTEntry>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const DiscIO::NANDImporter::NANDFSTEntry& entry, FormatContext& ctx) const
  {
    return fmt::format_to(
        ctx.out(), "{:12.12} {:#010b} {:#04x} {:#06x} {:#06x} {:#010x} {:#010x} {:#06x} {:#010x}",
        entry.name, entry.mode, entry.attr, entry.sub, entry.sib, entry.size, entry.uid, entry.gid,
        entry.x3);
  }
};
