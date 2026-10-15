//===-- SymbolTest.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/ObjectFile/ELF/ObjectFileELF.h"
#include "Plugins/ObjectFile/Mach-O/ObjectFileMachO.h"
#include "Plugins/SymbolFile/DWARF/SymbolFileDWARF.h"
#include "Plugins/SymbolFile/DWARF/SymbolFileDWARFDebugMap.h"
#include "Plugins/SymbolFile/Symtab/SymbolFileSymtab.h"
#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"
#include "TestingSupport/SubsystemRAII.h"
#include "TestingSupport/TestUtilities.h"

#include "lldb/Core/DataFileCache.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Core/Section.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Symtab.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/DataEncoder.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/VirtualDataExtractor.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

#include <future>
#include <iterator>
#include <memory>
#include <utility>

#include "gtest/gtest.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::plugin::dwarf;

class SymtabTest : public testing::Test {
  SubsystemRAII<FileSystem, HostInfo, ObjectFileMachO, SymbolFileDWARF,
                ObjectFileELF, SymbolFileSymtab, TypeSystemClang>
      subsystem;
};

TEST_F(SymtabTest, ExternalASTFilesAreDeduplicatedPerConsumer) {
  auto object = TestFile::fromYaml(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x100000C
  cpusubtype: 0x0
  filetype:   0x2
  ncmds:      0
  sizeofcmds: 0
  flags:      0x0
  reserved:   0x0
)");
  ASSERT_THAT_EXPECTED(object, llvm::Succeeded());

  llvm::SmallString<128> directory;
  ASSERT_NO_ERROR(
      llvm::sys::fs::createUniqueDirectory("lldb-ast-files", directory));
  auto cleanup = llvm::make_scope_exit(
      [&] { llvm::sys::fs::remove_directories(directory); });
  auto file_path = [&](llvm::StringRef parent) {
    llvm::SmallString<128> path(directory);
    llvm::sys::path::append(path, parent, "Shared.swiftmodule");
    return path;
  };
  auto write_file = [&](llvm::StringRef path, llvm::StringRef contents) {
    ASSERT_NO_ERROR(
        llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path)));
    std::error_code error;
    llvm::raw_fd_ostream stream(path, error);
    ASSERT_NO_ERROR(error);
    stream << contents;
  };
  auto make_image = [&](llvm::StringRef path) -> ModuleSP {
    auto module_spec = object->moduleSpec();
    module_spec.GetFileSpec() =
        FileSpec(directory).CopyByAppendingPathComponent("image");
    auto module = std::make_shared<Module>(module_spec);
    ObjectFile *file = module->GetObjectFile();
    EXPECT_NE(file, nullptr);
    if (!file)
      return {};
    file->GetSymtab()->AddSymbol(Symbol(0, path, eSymbolTypeASTFile, false,
                                        true, false, false, SectionSP(), 0, 0,
                                        false, false, 0));
    return module;
  };
  auto ast_data = [&](const ModuleSP &module, llvm::StringSet<> *loaded) {
    SymbolFileDWARFDebugMap provider(
        module->GetObjectFile()->shared_from_this());
    return provider.GetASTData(eLanguageTypeSwift, loaded);
  };
  auto contents = [](const DataBufferSP &buffer) {
    return llvm::StringRef(reinterpret_cast<const char *>(buffer->GetBytes()),
                           buffer->GetByteSize());
  };

  auto first_path = file_path("first");
  auto other_path = file_path("other");
  auto missing_path = file_path("missing");
  write_file(first_path, "first AST");
  write_file(other_path, "different AST with the same filename");
  auto first = make_image(first_path);
  auto duplicate = make_image(first_path);
  auto other = make_image(other_path);
  auto missing = make_image(missing_path);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(duplicate, nullptr);
  ASSERT_NE(other, nullptr);
  ASSERT_NE(missing, nullptr);

  llvm::StringSet<> loaded;
  auto first_buffers = ast_data(first, &loaded);
  ASSERT_EQ(first_buffers.size(), 1u);
  EXPECT_EQ(contents(first_buffers.front()), "first AST");
  EXPECT_TRUE(ast_data(duplicate, &loaded).empty());
  EXPECT_EQ(loaded.size(), 1u);
  // The optional set must not change consumers requesting all buffers.
  EXPECT_EQ(ast_data(duplicate, nullptr).size(), 1u);

  auto other_buffers = ast_data(other, &loaded);
  ASSERT_EQ(other_buffers.size(), 1u);
  EXPECT_EQ(contents(other_buffers.front()),
            "different AST with the same filename");
  EXPECT_EQ(loaded.size(), 2u);

  // A missing file must remain retryable when a later image needs it.
  EXPECT_TRUE(ast_data(missing, &loaded).empty());
  EXPECT_EQ(loaded.size(), 2u);
  write_file(missing_path, "now available");
  auto late_buffers = ast_data(missing, &loaded);
  ASSERT_EQ(late_buffers.size(), 1u);
  EXPECT_EQ(contents(late_buffers.front()), "now available");

  // A new consumer has no stale path cache after a build replaces a file.
  first_buffers.clear();
  write_file(first_path, "replacement AST");
  llvm::StringSet<> fresh_context;
  auto fresh_buffers = ast_data(first, &fresh_context);
  ASSERT_EQ(fresh_buffers.size(), 1u);
  EXPECT_EQ(contents(fresh_buffers.front()), "replacement AST");

  // Relative paths use the current cwd for each provider call, not a cwd
  // captured for the entire consumer lifetime.
  llvm::SmallString<128> original_cwd;
  ASSERT_NO_ERROR(llvm::sys::fs::current_path(original_cwd));
  auto restore_cwd = llvm::make_scope_exit(
      [&] { llvm::sys::fs::set_current_path(original_cwd); });
  auto relative = make_image("Shared.swiftmodule");
  llvm::StringSet<> relative_context;
  ASSERT_NO_ERROR(llvm::sys::fs::set_current_path(
      llvm::sys::path::parent_path(first_path)));
  auto relative_first = ast_data(relative, &relative_context);
  ASSERT_EQ(relative_first.size(), 1u);
  EXPECT_EQ(contents(relative_first.front()), "replacement AST");
  ASSERT_NO_ERROR(llvm::sys::fs::set_current_path(
      llvm::sys::path::parent_path(other_path)));
  auto relative_other = ast_data(relative, &relative_context);
  ASSERT_EQ(relative_other.size(), 1u);
  EXPECT_EQ(contents(relative_other.front()),
            "different AST with the same filename");
  EXPECT_EQ(relative_context.size(), 2u);
  EXPECT_TRUE(ast_data(first, &relative_context).empty());

  // A basename first searches cwd, then next to the referring image. A later
  // cwd file must still win over the already-loaded adjacent candidate.
  std::string adjacent_name =
      llvm::sys::path::filename(directory).str() + ".swiftmodule";
  llvm::SmallString<128> adjacent_path(
      first->GetFileSpec().GetDirectory().GetStringRef());
  llvm::sys::path::append(adjacent_path, adjacent_name);
  write_file(adjacent_path, "adjacent AST");
  auto adjacent = make_image(adjacent_name);
  llvm::StringSet<> adjacent_context;
  auto adjacent_buffers = ast_data(adjacent, &adjacent_context);
  ASSERT_EQ(adjacent_buffers.size(), 1u);
  EXPECT_EQ(contents(adjacent_buffers.front()), "adjacent AST");
  llvm::SmallString<128> cwd_path(llvm::sys::path::parent_path(other_path));
  llvm::sys::path::append(cwd_path, adjacent_name);
  write_file(cwd_path, "cwd AST");
  auto cwd_buffers = ast_data(adjacent, &adjacent_context);
  ASSERT_EQ(cwd_buffers.size(), 1u);
  EXPECT_EQ(contents(cwd_buffers.front()), "cwd AST");
  EXPECT_EQ(adjacent_context.size(), 2u);

  auto late_relative = make_image("late/Shared.swiftmodule");
  llvm::StringSet<> late_context;
  EXPECT_TRUE(ast_data(late_relative, &late_context).empty());
  EXPECT_TRUE(late_context.empty());
  llvm::SmallString<128> late_path(llvm::sys::path::parent_path(other_path));
  llvm::sys::path::append(late_path, "late", "Shared.swiftmodule");
  write_file(late_path, "late relative AST");
  auto late_relative_buffers = ast_data(late_relative, &late_context);
  ASSERT_EQ(late_relative_buffers.size(), 1u);
  EXPECT_EQ(contents(late_relative_buffers.front()), "late relative AST");

  // A custom VFS override is not replaced by the native lexical fast path.
  // Its returned path must anchor both the dedup key and the opened buffer.
  class ASTPathFileSystem : public llvm::vfs::ProxyFileSystem {
  public:
    ASTPathFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs,
                      llvm::StringRef path)
        : ProxyFileSystem(std::move(fs)), path(path) {}
    std::error_code
    makeAbsolute(llvm::SmallVectorImpl<char> &value) const override {
      ++calls;
      if (fail)
        return std::make_error_code(std::errc::permission_denied);
      value.assign(path.begin(), path.end());
      return {};
    }
    std::string path;
    mutable unsigned calls = 0;
    bool fail = false;
  };
  auto original_fs = FileSystem::Instance().GetVirtualFileSystem();
  auto custom_fs =
      llvm::makeIntrusiveRefCnt<ASTPathFileSystem>(original_fs, first_path);
  FileSystem::Terminate();
  FileSystem::Initialize(custom_fs);
  auto restore_fs = llvm::make_scope_exit([&] {
    FileSystem::Terminate();
    FileSystem::Initialize(original_fs);
  });
  llvm::StringSet<> custom_context;
  auto custom_buffers = ast_data(relative, &custom_context);
  ASSERT_EQ(custom_buffers.size(), 1u);
  EXPECT_EQ(contents(custom_buffers.front()), "replacement AST");
  EXPECT_EQ(custom_fs->calls, 1u);
  EXPECT_TRUE(custom_context.contains(first_path));

  // Failed resolution retains legacy relative reads without poisoning the
  // consumer set. Callers without a consumer set still request every buffer.
  custom_fs->fail = true;
  llvm::StringSet<> failed_context;
  auto failed_buffers = ast_data(relative, &failed_context);
  ASSERT_EQ(failed_buffers.size(), 1u);
  EXPECT_EQ(contents(failed_buffers.front()),
            "different AST with the same filename");
  EXPECT_TRUE(failed_context.empty());
  unsigned previous_calls = custom_fs->calls;
  EXPECT_EQ(ast_data(relative, nullptr).size(), 1u);
  EXPECT_EQ(custom_fs->calls, previous_calls);
}

static void EncodeDecode(const Symtab &object, ByteOrder byte_order) {
  const uint8_t addr_size = 8;
  DataEncoder file(byte_order, addr_size);

  object.Encode(file);
  llvm::ArrayRef<uint8_t> bytes = file.GetData();
  DataExtractor data(bytes.data(), bytes.size(), byte_order, addr_size);
  Symtab decoded_object(object.GetObjectFile());
  offset_t data_offset = 0;
  bool uuid_mismatch = false;
  decoded_object.Decode(data, &data_offset, uuid_mismatch);
  ASSERT_EQ(object.GetNumSymbols(), decoded_object.GetNumSymbols());
  for (size_t i = 0; i < object.GetNumSymbols(); ++i)
    EXPECT_EQ(*object.SymbolAtIndex(i), *decoded_object.SymbolAtIndex(i));
}

static void EncodeDecode(const Symtab &object) {
  EncodeDecode(object, eByteOrderLittle);
  EncodeDecode(object, eByteOrderBig);
}

TEST_F(SymtabTest, EncodeDecodeSymtab) {

  auto ExpectedFile = TestFile::fromYaml(R"(
--- !mach-o
FileHeader:
  magic:           0xFEEDFACF
  cputype:         0x100000C
  cpusubtype:      0x0
  filetype:        0x2
  ncmds:           17
  sizeofcmds:      792
  flags:           0x200085
  reserved:        0x0
LoadCommands:
  - cmd:             LC_SEGMENT_64
    cmdsize:         72
    segname:         __PAGEZERO
    vmaddr:          0
    vmsize:          4294967296
    fileoff:         0
    filesize:        0
    maxprot:         0
    initprot:        0
    nsects:          0
    flags:           0
  - cmd:             LC_SEGMENT_64
    cmdsize:         232
    segname:         __TEXT
    vmaddr:          4294967296
    vmsize:          16384
    fileoff:         0
    filesize:        16384
    maxprot:         5
    initprot:        5
    nsects:          2
    flags:           0
    Sections:
      - sectname:        __text
        segname:         __TEXT
        addr:            0x100003F94
        size:            36
        offset:          0x3F94
        align:           2
        reloff:          0x0
        nreloc:          0
        flags:           0x80000400
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         FF8300D1E80300AA00008052FF1F00B9E81B00B9E10B00F9E20700F9FF830091C0035FD6
      - sectname:        __unwind_info
        segname:         __TEXT
        addr:            0x100003FB8
        size:            72
        offset:          0x3FB8
        align:           2
        reloff:          0x0
        nreloc:          0
        flags:           0x0
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         010000001C000000000000001C000000000000001C00000002000000943F00003400000034000000B93F00000000000034000000030000000C000100100001000000000000200002
  - cmd:             LC_SEGMENT_64
    cmdsize:         72
    segname:         __LINKEDIT
    vmaddr:          4294983680
    vmsize:          16384
    fileoff:         16384
    filesize:        674
    maxprot:         1
    initprot:        1
    nsects:          0
    flags:           0
  - cmd:             LC_DYLD_CHAINED_FIXUPS
    cmdsize:         16
    dataoff:         16384
    datasize:        56
  - cmd:             LC_DYLD_EXPORTS_TRIE
    cmdsize:         16
    dataoff:         16440
    datasize:        48
  - cmd:             LC_SYMTAB
    cmdsize:         24
    symoff:          16496
    nsyms:           10
    stroff:          16656
    strsize:         128
  - cmd:             LC_DYSYMTAB
    cmdsize:         80
    ilocalsym:       0
    nlocalsym:       8
    iextdefsym:      8
    nextdefsym:      2
    iundefsym:       10
    nundefsym:       0
    tocoff:          0
    ntoc:            0
    modtaboff:       0
    nmodtab:         0
    extrefsymoff:    0
    nextrefsyms:     0
    indirectsymoff:  0
    nindirectsyms:   0
    extreloff:       0
    nextrel:         0
    locreloff:       0
    nlocrel:         0
  - cmd:             LC_LOAD_DYLINKER
    cmdsize:         32
    name:            12
    Content:         '/usr/lib/dyld'
    ZeroPadBytes:    7
  - cmd:             LC_UUID
    cmdsize:         24
    uuid:            1EECD2B8-16EA-3FEC-AB3C-F46139DBD0E2
  - cmd:             LC_BUILD_VERSION
    cmdsize:         32
    platform:        1
    minos:           786432
    sdk:             786432
    ntools:          1
    Tools:
      - tool:            3
        version:         46596096
  - cmd:             LC_SOURCE_VERSION
    cmdsize:         16
    version:         0
  - cmd:             LC_MAIN
    cmdsize:         24
    entryoff:        16276
    stacksize:       0
  - cmd:             LC_LOAD_DYLIB
    cmdsize:         48
    dylib:
      name:            24
      timestamp:       2
      current_version: 78643968
      compatibility_version: 65536
    Content:         '/usr/lib/libc++.1.dylib'
    ZeroPadBytes:    1
  - cmd:             LC_LOAD_DYLIB
    cmdsize:         56
    dylib:
      name:            24
      timestamp:       2
      current_version: 85917696
      compatibility_version: 65536
    Content:         '/usr/lib/libSystem.B.dylib'
    ZeroPadBytes:    6
  - cmd:             LC_FUNCTION_STARTS
    cmdsize:         16
    dataoff:         16488
    datasize:        8
  - cmd:             LC_DATA_IN_CODE
    cmdsize:         16
    dataoff:         16496
    datasize:        0
  - cmd:             LC_CODE_SIGNATURE
    cmdsize:         16
    dataoff:         16784
    datasize:        274
LinkEditData:
  NameList:
    - n_strx:          28
      n_type:          0x64
      n_sect:          0
      n_desc:          0
      n_value:         0
    - n_strx:          64
      n_type:          0x64
      n_sect:          0
      n_desc:          0
      n_value:         0
    - n_strx:          73
      n_type:          0x66
      n_sect:          0
      n_desc:          1
      n_value:         1639532873
    - n_strx:          1
      n_type:          0x2E
      n_sect:          1
      n_desc:          0
      n_value:         4294983572
    - n_strx:          115
      n_type:          0x24
      n_sect:          1
      n_desc:          0
      n_value:         4294983572
    - n_strx:          1
      n_type:          0x24
      n_sect:          0
      n_desc:          0
      n_value:         36
    - n_strx:          1
      n_type:          0x4E
      n_sect:          1
      n_desc:          0
      n_value:         36
    - n_strx:          1
      n_type:          0x64
      n_sect:          1
      n_desc:          0
      n_value:         0
    - n_strx:          2
      n_type:          0xF
      n_sect:          1
      n_desc:          16
      n_value:         4294967296
    - n_strx:          22
      n_type:          0xF
      n_sect:          1
      n_desc:          0
      n_value:         4294983572
  StringTable:
    - ' '
    - __mh_execute_header
    - _main
    - '/Users/gclayton/Documents/src/args/'
    - main.cpp
    - '/Users/gclayton/Documents/src/args/main.o'
    - _main
    - ''
    - ''
    - ''
    - ''
    - ''
    - ''
    - ''
...
)");

  ASSERT_THAT_EXPECTED(ExpectedFile, llvm::Succeeded());
  auto module_sp = std::make_shared<Module>(ExpectedFile->moduleSpec());
  ObjectFile *objfile = module_sp->GetObjectFile();
  ASSERT_NE(objfile, nullptr);

  // Test encoding and decoding an empty symbol table.
  Symtab symtab(objfile);
  symtab.PreloadSymbols();
  EncodeDecode(symtab);

  // Now encode and decode an actual symbol table from our yaml.
  Symtab *module_symtab = module_sp->GetSymtab();
  ASSERT_NE(module_symtab, nullptr);
  module_symtab->PreloadSymbols();
  EncodeDecode(*module_symtab);
}

TEST_F(SymtabTest, TestDecodeCStringMaps) {
  // Symbol tables save out the symbols, but they also save out the symbol table
  // name indexes. These name indexes are a map of sorted ConstString + T pairs
  // and when they are decoded from a file, they are no longer sorted since
  // ConstString objects can be sorted by "const char *" and the order in which
  // these strings are created won't be the same in a new process. We need to
  // ensure these name lookups happen correctly when we load the name indexes,
  // so this test loads a symbol table from a cache file from
  // "lldb/unittests/Symbol/Inputs/indexnames-symtab-cache" and make sure we
  // can correctly lookup each of the names in the symbol table.
  auto ExpectedFile = TestFile::fromYaml(R"(
--- !mach-o
FileHeader:
  magic:           0xFEEDFACF
  cputype:         0x100000C
  cpusubtype:      0x0
  filetype:        0x2
  ncmds:           16
  sizeofcmds:      744
  flags:           0x200085
  reserved:        0x0
LoadCommands:
  - cmd:             LC_SEGMENT_64
    cmdsize:         72
    segname:         __PAGEZERO
    vmaddr:          0
    vmsize:          4294967296
    fileoff:         0
    filesize:        0
    maxprot:         0
    initprot:        0
    nsects:          0
    flags:           0
  - cmd:             LC_SEGMENT_64
    cmdsize:         232
    segname:         __TEXT
    vmaddr:          4294967296
    vmsize:          16384
    fileoff:         0
    filesize:        16384
    maxprot:         5
    initprot:        5
    nsects:          2
    flags:           0
    Sections:
      - sectname:        __text
        segname:         __TEXT
        addr:            0x100003F64
        size:            76
        offset:          0x3F64
        align:           2
        reloff:          0x0
        nreloc:          0
        flags:           0x80000400
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         80018052C0035FD6E0028052C0035FD640048052C0035FD6FF8300D1FD7B01A9FD43009108008052E80B00B9BFC31FB8F4FFFF97F5FFFF97F6FFFF97E00B40B9FD7B41A9FF830091C0035FD6
      - sectname:        __unwind_info
        segname:         __TEXT
        addr:            0x100003FB0
        size:            80
        offset:          0x3FB0
        align:           2
        reloff:          0x0
        nreloc:          0
        flags:           0x0
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         010000001C000000000000001C000000000000001C00000002000000643F00003400000034000000B13F00000000000034000000030000000C0002001400020000000001180000000000000400000002
  - cmd:             LC_SEGMENT_64
    cmdsize:         72
    segname:         __LINKEDIT
    vmaddr:          4294983680
    vmsize:          16384
    fileoff:         16384
    filesize:        994
    maxprot:         1
    initprot:        1
    nsects:          0
    flags:           0
  - cmd:             LC_DYLD_CHAINED_FIXUPS
    cmdsize:         16
    dataoff:         16384
    datasize:        56
  - cmd:             LC_DYLD_EXPORTS_TRIE
    cmdsize:         16
    dataoff:         16440
    datasize:        80
  - cmd:             LC_SYMTAB
    cmdsize:         24
    symoff:          16528
    nsyms:           25
    stroff:          16928
    strsize:         176
  - cmd:             LC_DYSYMTAB
    cmdsize:         80
    ilocalsym:       0
    nlocalsym:       20
    iextdefsym:      20
    nextdefsym:      5
    iundefsym:       25
    nundefsym:       0
    tocoff:          0
    ntoc:            0
    modtaboff:       0
    nmodtab:         0
    extrefsymoff:    0
    nextrefsyms:     0
    indirectsymoff:  0
    nindirectsyms:   0
    extreloff:       0
    nextrel:         0
    locreloff:       0
    nlocrel:         0
  - cmd:             LC_LOAD_DYLINKER
    cmdsize:         32
    name:            12
    Content:         '/usr/lib/dyld'
    ZeroPadBytes:    7
  - cmd:             LC_UUID
    cmdsize:         24
    uuid:            3E94866E-0D1A-39BD-975B-64E8F1FDBAAE
  - cmd:             LC_BUILD_VERSION
    cmdsize:         32
    platform:        1
    minos:           786432
    sdk:             787200
    ntools:          1
    Tools:
      - tool:            3
        version:         49938432
  - cmd:             LC_SOURCE_VERSION
    cmdsize:         16
    version:         0
  - cmd:             LC_MAIN
    cmdsize:         24
    entryoff:        16252
    stacksize:       0
  - cmd:             LC_LOAD_DYLIB
    cmdsize:         56
    dylib:
      name:            24
      timestamp:       2
      current_version: 85943299
      compatibility_version: 65536
    Content:         '/usr/lib/libSystem.B.dylib'
    ZeroPadBytes:    6
  - cmd:             LC_FUNCTION_STARTS
    cmdsize:         16
    dataoff:         16520
    datasize:        8
  - cmd:             LC_DATA_IN_CODE
    cmdsize:         16
    dataoff:         16528
    datasize:        0
  - cmd:             LC_CODE_SIGNATURE
    cmdsize:         16
    dataoff:         17104
    datasize:        274
LinkEditData:
  NameList:
    - n_strx:          43
      n_type:          0x64
      n_sect:          0
      n_desc:          0
      n_value:         0
    - n_strx:          91
      n_type:          0x64
      n_sect:          0
      n_desc:          0
      n_value:         0
    - n_strx:          98
      n_type:          0x66
      n_sect:          0
      n_desc:          1
      n_value:         1651098491
    - n_strx:          1
      n_type:          0x2E
      n_sect:          1
      n_desc:          0
      n_value:         4294983524
    - n_strx:          152
      n_type:          0x24
      n_sect:          1
      n_desc:          0
      n_value:         4294983524
    - n_strx:          1
      n_type:          0x24
      n_sect:          0
      n_desc:          0
      n_value:         8
    - n_strx:          1
      n_type:          0x4E
      n_sect:          1
      n_desc:          0
      n_value:         8
    - n_strx:          1
      n_type:          0x2E
      n_sect:          1
      n_desc:          0
      n_value:         4294983532
    - n_strx:          157
      n_type:          0x24
      n_sect:          1
      n_desc:          0
      n_value:         4294983532
    - n_strx:          1
      n_type:          0x24
      n_sect:          0
      n_desc:          0
      n_value:         8
    - n_strx:          1
      n_type:          0x4E
      n_sect:          1
      n_desc:          0
      n_value:         8
    - n_strx:          1
      n_type:          0x2E
      n_sect:          1
      n_desc:          0
      n_value:         4294983540
    - n_strx:          162
      n_type:          0x24
      n_sect:          1
      n_desc:          0
      n_value:         4294983540
    - n_strx:          1
      n_type:          0x24
      n_sect:          0
      n_desc:          0
      n_value:         8
    - n_strx:          1
      n_type:          0x4E
      n_sect:          1
      n_desc:          0
      n_value:         8
    - n_strx:          1
      n_type:          0x2E
      n_sect:          1
      n_desc:          0
      n_value:         4294983548
    - n_strx:          167
      n_type:          0x24
      n_sect:          1
      n_desc:          0
      n_value:         4294983548
    - n_strx:          1
      n_type:          0x24
      n_sect:          0
      n_desc:          0
      n_value:         52
    - n_strx:          1
      n_type:          0x4E
      n_sect:          1
      n_desc:          0
      n_value:         52
    - n_strx:          1
      n_type:          0x64
      n_sect:          1
      n_desc:          0
      n_value:         0
    - n_strx:          2
      n_type:          0xF
      n_sect:          1
      n_desc:          16
      n_value:         4294967296
    - n_strx:          22
      n_type:          0xF
      n_sect:          1
      n_desc:          0
      n_value:         4294983532
    - n_strx:          27
      n_type:          0xF
      n_sect:          1
      n_desc:          0
      n_value:         4294983540
    - n_strx:          32
      n_type:          0xF
      n_sect:          1
      n_desc:          0
      n_value:         4294983524
    - n_strx:          37
      n_type:          0xF
      n_sect:          1
      n_desc:          0
      n_value:         4294983548
  StringTable:
    - ' '
    - __mh_execute_header
    - _bar
    - _baz
    - _foo
    - _main
    - '/Users/gclayton/Documents/objfiles/index-names/'
    - main.c
    - '/Users/gclayton/Documents/objfiles/index-names/main.o'
    - _foo
    - _bar
    - _baz
    - _main
    - ''
    - ''
    - ''
  FunctionStarts:  [ 0x3F64, 0x3F6C, 0x3F74, 0x3F7C ]
...
)");
  // This data was taken from a hex dump of the object file from the above yaml
  // and hexdumped so we can load the cache data in this test.
  const uint8_t symtab_cache_bytes[] = {
    0x01, 0x10, 0x3e, 0x94, 0x86, 0x6e, 0x0d, 0x1a,
    0x39, 0xbd, 0x97, 0x5b, 0x64, 0xe8, 0xf1, 0xfd,
    0xba, 0xae, 0xff, 0x53, 0x54, 0x41, 0x42, 0x91,
    0x00, 0x00, 0x00, 0x00, 0x2f, 0x55, 0x73, 0x65,
    0x72, 0x73, 0x2f, 0x67, 0x63, 0x6c, 0x61, 0x79,
    0x74, 0x6f, 0x6e, 0x2f, 0x44, 0x6f, 0x63, 0x75,
    0x6d, 0x65, 0x6e, 0x74, 0x73, 0x2f, 0x6f, 0x62,
    0x6a, 0x66, 0x69, 0x6c, 0x65, 0x73, 0x2f, 0x69,
    0x6e, 0x64, 0x65, 0x78, 0x2d, 0x6e, 0x61, 0x6d,
    0x65, 0x73, 0x2f, 0x6d, 0x61, 0x69, 0x6e, 0x2e,
    0x63, 0x00, 0x2f, 0x55, 0x73, 0x65, 0x72, 0x73,
    0x2f, 0x67, 0x63, 0x6c, 0x61, 0x79, 0x74, 0x6f,
    0x6e, 0x2f, 0x44, 0x6f, 0x63, 0x75, 0x6d, 0x65,
    0x6e, 0x74, 0x73, 0x2f, 0x6f, 0x62, 0x6a, 0x66,
    0x69, 0x6c, 0x65, 0x73, 0x2f, 0x69, 0x6e, 0x64,
    0x65, 0x78, 0x2d, 0x6e, 0x61, 0x6d, 0x65, 0x73,
    0x2f, 0x6d, 0x61, 0x69, 0x6e, 0x2e, 0x6f, 0x00,
    0x66, 0x6f, 0x6f, 0x00, 0x62, 0x61, 0x72, 0x00,
    0x62, 0x61, 0x7a, 0x00, 0x6d, 0x61, 0x69, 0x6e,
    0x00, 0x5f, 0x6d, 0x68, 0x5f, 0x65, 0x78, 0x65,
    0x63, 0x75, 0x74, 0x65, 0x5f, 0x68, 0x65, 0x61,
    0x64, 0x65, 0x72, 0x00, 0x53, 0x59, 0x4d, 0x42,
    0x01, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x2a,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0a, 0x20, 0x01, 0x37, 0x00, 0x00, 0x00, 0x00,
    0x7b, 0xc3, 0x69, 0x62, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x66, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x32, 0x01, 0x6d, 0x00, 0x00,
    0x00, 0x01, 0x64, 0x3f, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x32, 0x01, 0x71,
    0x00, 0x00, 0x00, 0x01, 0x6c, 0x3f, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00,
    0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x32,
    0x01, 0x75, 0x00, 0x00, 0x00, 0x01, 0x74, 0x3f,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x32, 0x01, 0x79, 0x00, 0x00, 0x00, 0x01,
    0x7c, 0x3f, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0x00, 0x14, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x04, 0x12, 0x02, 0x7e, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x64, 0x3f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x0f, 0x00, 0x01, 0x00,
    0x43, 0x4d, 0x41, 0x50, 0x07, 0x00, 0x00, 0x00,
    0x6d, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x75, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x79, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x37, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x71, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x7e, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00
  };

  ASSERT_THAT_EXPECTED(ExpectedFile, llvm::Succeeded());
  auto module_sp = std::make_shared<Module>(ExpectedFile->moduleSpec());
  ObjectFile *objfile = module_sp->GetObjectFile();
  ASSERT_NE(objfile, nullptr);

  // Test encoding and decoding an empty symbol table.
  DataExtractor data(symtab_cache_bytes, sizeof(symtab_cache_bytes),
                     eByteOrderLittle, 8);
  Symtab symtab(objfile);
  offset_t data_offset = 0;
  bool uuid_mismatch = false; // Gets set to true if signature doesn't match.
  const bool success = symtab.Decode(data, &data_offset, uuid_mismatch);
  ASSERT_EQ(success, true);
  ASSERT_EQ(uuid_mismatch, false);

  // Now make sure that name lookup works for all symbols. This indicates that
  // the Symtab::NameToIndexMap was decoded correctly and works as expected.
  Symbol *symbol = nullptr;
  symbol = symtab.FindFirstSymbolWithNameAndType(ConstString("main"),
                                                 eSymbolTypeCode,
                                                 Symtab::eDebugAny,
                                                 Symtab::eVisibilityAny);
  ASSERT_NE(symbol, nullptr);
  symbol = symtab.FindFirstSymbolWithNameAndType(ConstString("foo"),
                                                 eSymbolTypeCode,
                                                 Symtab::eDebugAny,
                                                 Symtab::eVisibilityAny);
  ASSERT_NE(symbol, nullptr);
  symbol = symtab.FindFirstSymbolWithNameAndType(ConstString("bar"),
                                                 eSymbolTypeCode,
                                                 Symtab::eDebugAny,
                                                 Symtab::eVisibilityAny);
  ASSERT_NE(symbol, nullptr);
  symbol = symtab.FindFirstSymbolWithNameAndType(ConstString("baz"),
                                                 eSymbolTypeCode,
                                                 Symtab::eDebugAny,
                                                 Symtab::eVisibilityAny);
  ASSERT_NE(symbol, nullptr);
}

TEST_F(SymtabTest, TestSymbolFileCreatedOnDemand) {
  auto ExpectedFile = TestFile::fromYaml(R"(
--- !ELF
FileHeader:
  Class:           ELFCLASS64
  Data:            ELFDATA2LSB
  Type:            ET_EXEC
  Machine:         EM_X86_64
  Entry:           0x0000000000400180
Sections:
  - Name:            .text
    Type:            SHT_PROGBITS
    Flags:           [ SHF_ALLOC, SHF_EXECINSTR ]
    Address:         0x0000000000400180
    AddressAlign:    0x0000000000000010
    Content:         554889E58B042500106000890425041060005DC3
Symbols:
  - Name:            _start
    Type:            STT_FUNC
    Section:         .text
    Value:           0x0000000000400180
    Size:            0x0000000000000014
    Binding:         STB_GLOBAL
...
)");
  ASSERT_THAT_EXPECTED(ExpectedFile, llvm::Succeeded());
  auto module_sp = std::make_shared<Module>(ExpectedFile->moduleSpec());

  // The symbol file should not be created by default.
  Symtab *module_symtab = module_sp->GetSymtab(/*can_create=*/false);
  ASSERT_EQ(module_symtab, nullptr);

  // But it should be created on demand.
  module_symtab = module_sp->GetSymtab(/*can_create=*/true);
  ASSERT_NE(module_symtab, nullptr);

  // And we should be able to get it again once it has been created.
  Symtab *cached_module_symtab = module_sp->GetSymtab(/*can_create=*/false);
  ASSERT_EQ(module_symtab, cached_module_symtab);
}

TEST_F(SymtabTest, TestSymbolTableCreatedOnDemand) {
  const char *yamldata = R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_EXEC
  Machine: EM_386
DWARF:
  debug_abbrev:
    - Table:
        - Code:            0x00000001
          Tag:             DW_TAG_compile_unit
          Children:        DW_CHILDREN_no
          Attributes:
            - Attribute:       DW_AT_addr_base
              Form:            DW_FORM_sec_offset
  debug_info:
    - Version:         5
      AddrSize:        4
      UnitType:        DW_UT_compile
      Entries:
        - AbbrCode:        0x00000001
          Values:
            - Value:           0x8 # Offset of the first Address past the header
        - AbbrCode:        0x0
  debug_addr:
    - Version: 5
      AddressSize: 4
      Entries:
        - Address: 0x1234
        - Address: 0x5678
  debug_line:
    - Length:          42
      Version:         2
      PrologueLength:  36
      MinInstLength:   1
      DefaultIsStmt:   1
      LineBase:        251
      LineRange:       14
      OpcodeBase:      13
      StandardOpcodeLengths: [ 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 ]
      IncludeDirs:
        - '/tmp'
      Files:
        - Name:            main.cpp
          DirIdx:          1
          ModTime:         0
          Length:          0
)";
  llvm::Expected<TestFile> file = TestFile::fromYaml(yamldata);
  EXPECT_THAT_EXPECTED(file, llvm::Succeeded());
  auto module_sp = std::make_shared<Module>(file->moduleSpec());

  SymbolFile *symbol_file = module_sp->GetSymbolFile();
  // At this point, the symbol table is not created. This is because the above
  // yaml data contains the necessary sections in order for
  // SymbolFileDWARF::CalculateAbilities() to identify all abilities, saving it
  // from calling SymbolFileDWARFDebugMap::CalculateAbilities(), which
  // eventually loads the symbol table, which we don't want.

  // The symbol table should not be created if asked not to.
  Symtab *symtab = symbol_file->GetSymtab(/*can_create=*/false);
  ASSERT_EQ(symtab, nullptr);

  // But it should be created on demand.
  symtab = symbol_file->GetSymtab(/*can_create=*/true);
  ASSERT_NE(symtab, nullptr);

  // And we should be able to get it again once it has been created.
  Symtab *cached_symtab = symbol_file->GetSymtab(/*can_create=*/false);
  ASSERT_EQ(symtab, cached_symtab);
}

TEST_F(SymtabTest, CLinkageFunctionLookupDoesNotIndexOtherNames) {
  auto image = TestFile::fromYaml(R"(
--- !ELF
FileHeader:
  Class: ELFCLASS64
  Data: ELFDATA2LSB
  Type: ET_DYN
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [SHF_ALLOC, SHF_EXECINSTR]
    Address: 0x1000
    Content: C3
)");
  ASSERT_THAT_EXPECTED(image, llvm::Succeeded());
  auto module = std::make_shared<Module>(image->moduleSpec());
  ObjectFile *object = module->GetObjectFile();
  ASSERT_NE(object, nullptr);
  Symtab *symtab = object->GetSymtab();
  uint32_t id = 0;
  auto add_symbol = [&](llvm::StringRef name, SymbolType type, bool trampoline,
                        bool annotations) {
    const uint32_t symbol_id = id++;
    symtab->AddSymbol(Symbol(symbol_id, name, type, /*external=*/false,
                             /*is_debug=*/false, trampoline,
                             /*is_artificial=*/false, SectionSP(), id, 0,
                             /*size_is_valid=*/false, annotations, 0));
  };
  add_symbol("runtime", eSymbolTypeCode, false, false);
  add_symbol("runtime@VERSION", eSymbolTypeResolver, false, true);
  add_symbol("runtime", eSymbolTypeReExported, false, false);
  add_symbol("runtime", eSymbolTypeAbsolute, false, false);
  add_symbol("runtime", eSymbolTypeUndefined, false, false);
  add_symbol("runtime", eSymbolTypeData, false, false);
  // IsTrampoline uses the symbol type; the constructor's legacy bool is unused.
  add_symbol("runtime", eSymbolTypeTrampoline, true, false);
  add_symbol("_Z7runtimev", eSymbolTypeCode, false, false);
  add_symbol("$s4Test7runtimeyyF", eSymbolTypeCode, false, false);
  for (unsigned i = 0; i < 1024; ++i)
    add_symbol("_Z9unrelatedILi" + std::to_string(i) + "EEvv", eSymbolTypeCode,
               false, false);

  SymbolContextList matches;
  symtab->FindFunctionSymbols(ConstString("runtime"), eFunctionNameTypeFull,
                              matches, /*match_c_linkage_names=*/true);
  ASSERT_EQ(matches.GetSize(), 4u);
  for (unsigned i = 0; i < matches.GetSize(); ++i) {
    SymbolContext context;
    ASSERT_TRUE(matches.GetContextAtIndex(i, context));
    ASSERT_NE(context.symbol, nullptr);
    EXPECT_EQ(context.symbol->GetID(), i);
  }
  EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);

  SymbolContextList absent;
  symtab->FindFunctionSymbols(ConstString("absent"), eFunctionNameTypeFull,
                              absent, /*match_c_linkage_names=*/true);
  EXPECT_EQ(absent.GetSize(), 0u);
  EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);

  // The default still builds the general indexes and finds the C++ basename.
  SymbolContextList normal;
  symtab->FindFunctionSymbols(ConstString("runtime"), eFunctionNameTypeBase,
                              normal);
  EXPECT_GT(normal.GetSize(), matches.GetSize());
  EXPECT_GT(module->GetSymtabIndexTime().get().count(), 0.0);
}

namespace {
struct MetadataNList {
  uint8_t type;
  uint16_t descriptor;
  uint64_t value;
  std::string name;
  uint8_t section = 0;
};

llvm::Expected<TestFile> MetadataMachO(llvm::ArrayRef<MetadataNList> records) {
  std::string yaml;
  llvm::raw_string_ostream out(yaml);
  uint32_t string_size = 1;
  for (const auto &record : records)
    string_size += record.name.size() + 1;
  out << "--- !mach-o\nFileHeader:\n"
      << "  magic: 0xFEEDFACF\n  cputype: 0x100000C\n  cpusubtype: 0\n"
      << "  filetype: 2\n  ncmds: 1\n  sizeofcmds: 24\n  flags: 0\n"
      << "  reserved: 0\nLoadCommands:\n  - cmd: LC_SYMTAB\n"
      << "    cmdsize: 24\n    symoff: 56\n    nsyms: " << records.size()
      << "\n    stroff: " << 56 + records.size() * 16
      << "\n    strsize: " << string_size << "\nLinkEditData:\n  NameList:\n";
  uint32_t string_offset = 1;
  for (const auto &record : records) {
    out << "    - n_strx: " << string_offset
        << "\n      n_type: " << unsigned(record.type)
        << "\n      n_sect: 0\n      n_desc: " << record.descriptor
        << "\n      n_value: " << record.value << '\n';
    string_offset += record.name.size() + 1;
  }
  out << "  StringTable:\n    - ''\n";
  for (const auto &record : records)
    out << "    - '" << record.name << "'\n";
  return TestFile::fromYaml(yaml);
}

class MetadataDebugMap : public SymbolFileDWARFDebugMap {
public:
  using SymbolFileDWARFDebugMap::GetModuleByOSOIndex;
  void Hydrate() { InitOSO(); }
  size_t RangeCount() const { return m_debug_map.GetSize(); }
  using SymbolFileDWARFDebugMap::SymbolFileDWARFDebugMap;
  const void *Entry(unsigned index) { return &m_compile_unit_infos[index]; }
  uint32_t FirstSymbolID(unsigned index) {
    return m_compile_unit_infos[index].first_symbol_id;
  }
  uint32_t LastSymbolID(unsigned index) {
    return m_compile_unit_infos[index].last_symbol_id;
  }
  std::string LoadError(unsigned index) {
    return m_compile_unit_infos[index].oso_load_error.AsCString("");
  }
};
} // namespace

TEST_F(SymtabTest, MachODebugMetadataPreservesOrderedReferences) {
  auto object = MetadataMachO({{0x64, 0, 0, "./source/"},
                               {0x64, 0, 0, "_unit.c"},
                               {0x66, 1, 42, "unit.o"},
                               {0x32, 0, 0, "first.swiftmodule"},
                               {0x64, 0, 0, ""},
                               {0x32, 0, 0, "_second.swiftmodule"},
                               {0x32, 0, 0, "first.swiftmodule"}});
  ASSERT_THAT_EXPECTED(object, llvm::Succeeded());
  auto module = std::make_shared<Module>(object->moduleSpec());
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  const auto *metadata = file->GetDebugInfoMetadata();
  ASSERT_NE(metadata, nullptr);
  EXPECT_EQ(file->GetSymtab(false), nullptr);
  ASSERT_EQ(metadata->swift_ast_files.size(), 3u);
  EXPECT_EQ(metadata->swift_ast_files[0], "first.swiftmodule");
  EXPECT_EQ(metadata->swift_ast_files[1], "second.swiftmodule");
  EXPECT_EQ(metadata->swift_ast_files[2], "first.swiftmodule");
  ASSERT_TRUE(metadata->debug_map_objects);
  ASSERT_EQ(metadata->debug_map_objects->size(), 1u);
  EXPECT_EQ(metadata->debug_map_objects->front().source_file,
            "./source/_unit.c");
  EXPECT_EQ(metadata->debug_map_objects->front().object_file, "unit.o");
  EXPECT_EQ(metadata->debug_map_objects->front().modification_time, 42u);

  // The full parser has the same ordered names, including ABI normalization.
  Symtab *symbols = file->GetSymtab();
  ASSERT_NE(symbols, nullptr);
  EXPECT_EQ(symbols->SymbolAtIndex(0)->GetName().GetStringRef(),
            metadata->debug_map_objects->front().source_file);
  uint32_t next = 0;
  for (llvm::StringRef path : metadata->swift_ast_files) {
    Symbol *symbol = symbols->FindSymbolWithType(
        eSymbolTypeASTFile, Symtab::eDebugAny, Symtab::eVisibilityAny, next);
    ASSERT_NE(symbol, nullptr);
    EXPECT_EQ(symbol->GetName().GetStringRef(), path);
    ++next;
  }
  // A materialized table may have been augmented and takes precedence.
  EXPECT_EQ(file->GetDebugInfoMetadata(), nullptr);
}

TEST_F(SymtabTest, DebugMapMissingObjectsDeferSymbolsAndRetainFailureCache) {
  llvm::SmallString<128> directory;
  ASSERT_NO_ERROR(
      llvm::sys::fs::createUniqueDirectory("lldb-debug-map", directory));
  auto cleanup = llvm::make_scope_exit(
      [&] { llvm::sys::fs::remove_directories(directory); });
  llvm::SmallString<128> missing(directory);
  llvm::sys::path::append(missing, "unit.o");
  llvm::SmallString<128> other(directory);
  llvm::sys::path::append(other, "other.o");
  auto object = MetadataMachO({{0x64, 0, 0, "./source/"},
                               {0x64, 0, 0, "unit.c"},
                               {0x66, 1, 0, missing.str().str()},
                               {0x64, 0, 0, ""},
                               {0x64, 0, 0, "./source/"},
                               {0x64, 0, 0, "other.c"},
                               {0x66, 1, 0, other.str().str()},
                               {0x64, 0, 0, ""}});
  ASSERT_THAT_EXPECTED(object, llvm::Succeeded());
  auto module = std::make_shared<Module>(object->moduleSpec());
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  MetadataDebugMap provider(file->shared_from_this());
  EXPECT_EQ(provider.CalculateAbilities(), SymbolFile::kAllAbilities);
  ASSERT_EQ(provider.GetNumCompileUnits(), 2u);
  const void *entry = provider.Entry(0);
  EXPECT_EQ(provider.FirstSymbolID(0), UINT32_MAX);
  EXPECT_EQ(provider.GetModuleByOSOIndex(0), nullptr);
  EXPECT_EQ(file->GetSymtab(false), nullptr);

  // The existing provider remembers a missing file, even if it appears later.
  {
    std::error_code error;
    llvm::raw_fd_ostream stream(missing, error);
    ASSERT_NO_ERROR(error);
    stream << "object now available";
  }
  EXPECT_EQ(provider.GetModuleByOSOIndex(0), nullptr);
  EXPECT_EQ(file->GetSymtab(false), nullptr);

  // A fresh provider retries and hydrates original indices before construction.
  MetadataDebugMap fresh(file->shared_from_this());
  ASSERT_EQ(fresh.GetNumCompileUnits(), 2u);
  const void *fresh_entry = fresh.Entry(0);
  {
    std::error_code error;
    llvm::raw_fd_ostream stream(other, error);
    ASSERT_NO_ERROR(error);
    stream << "another object now available";
  }
  EXPECT_NE(provider.GetModuleByOSOIndex(1), nullptr);
  EXPECT_EQ(provider.Entry(0), entry);
  EXPECT_EQ(provider.GetModuleByOSOIndex(0), nullptr);
  EXPECT_NE(provider.LoadError(0).find("does not exist"), std::string::npos);
  EXPECT_NE(fresh.GetModuleByOSOIndex(0), nullptr);
  EXPECT_NE(file->GetSymtab(false), nullptr);
  EXPECT_EQ(fresh.Entry(0), fresh_entry);
  EXPECT_EQ(fresh.FirstSymbolID(0), 0u);
  EXPECT_EQ(fresh.LastSymbolID(0), 2u);
  EXPECT_EQ(provider.Entry(0), entry);
}

TEST_F(SymtabTest, NonstandardDebugMapMetadataUsesFullParser) {
  auto object = MetadataMachO({{0x64, 0, 0, "source.c"},
                               {0x24, 0, 1, "_between"},
                               {0x66, 1, 0, "missing.o"},
                               {0x64, 0, 0, ""},
                               {0x32, 0, 0, "module.swiftmodule"}});
  ASSERT_THAT_EXPECTED(object, llvm::Succeeded());
  auto module = std::make_shared<Module>(object->moduleSpec());
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  const auto *metadata = file->GetDebugInfoMetadata();
  ASSERT_NE(metadata, nullptr);
  EXPECT_FALSE(metadata->debug_map_objects);
  ASSERT_EQ(metadata->swift_ast_files.size(), 1u);
  EXPECT_EQ(file->GetSymtab(false), nullptr);
  MetadataDebugMap provider(file->shared_from_this());
  EXPECT_EQ(provider.GetNumCompileUnits(), 1u);
  EXPECT_NE(file->GetSymtab(false), nullptr);
}

namespace {
ModuleSP MetadataReaderModule(
    llvm::ArrayRef<MetadataNList> records, uint8_t address_size = 8,
    ByteOrder order = eByteOrderLittle, llvm::ArrayRef<uint8_t> exports = {},
    llvm::function_ref<void(DataEncoder &)> corrupt = [](DataEncoder &) {},
    llvm::ArrayRef<uint8_t> extra_commands = {},
    uint32_t extra_command_count = 0,
    llvm::ArrayRef<uint8_t> trailing_data = {}) {
  DataEncoder data(order, address_size);
  const uint32_t header_size = address_size == 8 ? 32 : 28;
  const uint32_t command_size =
      (exports.empty() ? 24 : 40) + extra_commands.size();
  const uint32_t symbols_offset = header_size + command_size;
  const uint32_t strings_offset =
      symbols_offset + records.size() * (address_size == 8 ? 16 : 12);
  uint32_t strings_size = 1;
  for (const auto &record : records)
    strings_size += record.name.size() + 1;
  data.AppendU32(address_size == 8 ? 0xfeedfacf : 0xfeedface);
  data.AppendU32(address_size == 8 ? 0x100000c : 12);
  data.AppendU32(0);
  data.AppendU32(2);
  data.AppendU32((exports.empty() ? 1 : 2) + extra_command_count);
  data.AppendU32(command_size);
  data.AppendU32(0);
  if (address_size == 8)
    data.AppendU32(0);
  data.AppendU32(2); // LC_SYMTAB
  data.AppendU32(24);
  data.AppendU32(symbols_offset);
  data.AppendU32(records.size());
  data.AppendU32(strings_offset);
  data.AppendU32(strings_size);
  if (!exports.empty()) {
    data.AppendU32(0x80000033); // LC_DYLD_EXPORTS_TRIE
    data.AppendU32(16);
    data.AppendU32(strings_offset + strings_size);
    data.AppendU32(exports.size());
  }
  data.AppendData(extra_commands);
  uint32_t string_offset = 1;
  for (const auto &record : records) {
    data.AppendU32(string_offset);
    data.AppendU8(record.type);
    data.AppendU8(record.section);
    data.AppendU16(record.descriptor);
    data.AppendAddress(record.value);
    string_offset += record.name.size() + 1;
  }
  data.AppendU8(0);
  for (const auto &record : records)
    data.AppendCString(record.name);
  data.AppendData(exports);
  data.AppendData(trailing_data);
  corrupt(data);
  auto buffer = std::make_shared<DataBufferHeap>(data.GetData().data(),
                                                 data.GetByteSize());
  ModuleSpec spec(FileSpec(), UUID(),
                  std::make_shared<DataExtractor>(buffer, order, address_size));
  return std::make_shared<Module>(spec);
}

std::vector<uint8_t> SingleExport(llvm::StringRef name, bool reexport = false) {
  std::vector<uint8_t> result{0, 1};
  result.insert(result.end(), name.begin(), name.end());
  result.push_back(0);
  result.push_back(name.size() + 4); // single-byte offset of the child
  if (reexport) {
    const uint8_t terminal[] = {9, 8, 1, '_', 'o', 't', 'h', 'e', 'r', 0, 0};
    result.insert(result.end(), std::begin(terminal), std::end(terminal));
  } else {
    const uint8_t terminal[] = {2, 0, 0, 0};
    result.insert(result.end(), std::begin(terminal), std::end(terminal));
  }
  return result;
}
struct LinkageExportNode {
  std::vector<uint8_t> terminal;
  std::vector<std::pair<std::string, unsigned>> children;
};

// Fixed-width ULEBs keep child offsets independent of their encoded size and
// let these fixtures describe noncanonical but valid empty/overlapping edges.
std::vector<uint8_t>
LinkageExportTrie(llvm::ArrayRef<LinkageExportNode> nodes) {
  std::vector<uint32_t> offsets;
  uint32_t size = 0;
  for (const auto &node : nodes) {
    offsets.push_back(size);
    size += 2 + node.terminal.size();
    for (const auto &child : node.children)
      size += child.first.size() + 1 + 5;
  }
  std::vector<uint8_t> result;
  for (const auto &node : nodes) {
    result.push_back(node.terminal.size());
    result.insert(result.end(), node.terminal.begin(), node.terminal.end());
    result.push_back(node.children.size());
    for (const auto &child : node.children) {
      result.insert(result.end(), child.first.begin(), child.first.end());
      result.push_back(0);
      uint32_t offset = offsets[child.second];
      for (unsigned i = 0; i != 4; ++i) {
        result.push_back((offset & 0x7f) | 0x80);
        offset >>= 7;
      }
      result.push_back(offset);
    }
  }
  return result;
}

ModuleSP IndirectStubReaderModule(uint8_t address_size, ByteOrder order,
                                  bool resolver = false,
                                  uint32_t indirect_index = 0,
                                  bool truncate_indirect = false,
                                  llvm::StringRef runtime_name = "_runtime") {
  const MetadataNList records[] = {{1, 0, 0, runtime_name.str()},
                                   {1, 0, 0, "_reference_only"}};
  const uint32_t header_size = address_size == 8 ? 32 : 28;
  const uint32_t segment_size = address_size == 8 ? 72 : 56;
  const uint32_t section_size = address_size == 8 ? 80 : 68;
  std::vector<uint8_t> exports;
  if (resolver) {
    exports = SingleExport("_resolver");
    // Flags=STUB_AND_RESOLVER, stub and resolver offsets both zero.
    exports.resize(exports.size() - 4);
    exports.insert(exports.end(), {3, 16, 0, 0, 0});
  }
  const uint32_t commands_size =
      24 + (resolver ? 16 : 0) + segment_size + section_size + 80;
  uint32_t strings_size = 1;
  for (const auto &record : records)
    strings_size += record.name.size() + 1;
  const uint32_t indirect_offset =
      header_size + commands_size +
      std::size(records) * (address_size == 8 ? 16 : 12) + strings_size +
      exports.size();
  DataEncoder commands(order, address_size);
  commands.AppendU32(address_size == 8 ? 0x19 : 1); // LC_SEGMENT[_64]
  commands.AppendU32(segment_size + section_size);
  auto append_name = [&](llvm::StringRef name) {
    std::string padded(name);
    padded.resize(16, 0);
    commands.AppendData(llvm::StringRef(padded));
  };
  append_name("__TEXT");
  commands.AppendAddress(0x1000);
  commands.AppendAddress(0x1000);
  commands.AppendAddress(0);
  commands.AppendAddress(indirect_offset + 4);
  for (uint32_t value : {5, 5, 1, 0})
    commands.AppendU32(value);
  append_name("__stubs");
  append_name("__TEXT");
  commands.AppendAddress(0x1000);
  commands.AppendAddress(4);
  for (uint32_t value : {0, 0, 0, 0, 8, 0, 4})
    commands.AppendU32(
        value); // offset..flags=S_SYMBOL_STUBS,reserved1,reserved2
  if (address_size == 8)
    commands.AppendU32(0);
  commands.AppendU32(0xb); // LC_DYSYMTAB
  commands.AppendU32(80);
  uint32_t fields[18] = {};
  fields[5] = std::size(records);
  fields[12] = indirect_offset;
  fields[13] = 1;
  for (uint32_t value : fields)
    commands.AppendU32(value);
  DataEncoder indices(order, address_size);
  if (!truncate_indirect)
    indices.AppendU32(indirect_index);
  return MetadataReaderModule(
      records, address_size, order, exports, [](DataEncoder &) {},
      commands.GetData(), 2, indices.GetData());
}
// This uses real Mach-O load commands and the same VirtualDataExtractor
// topology supplied by HostInfo, with raw cache offsets distinct from both
// logical VM offsets and physical backing offsets.
ModuleSP CodeDataReaderModule(
    llvm::ArrayRef<MetadataNList> records, bool mapped_cache = false,
    uint8_t address_size = 8, ByteOrder order = eByteOrderLittle,
    llvm::ArrayRef<uint8_t> exports = {}, bool indirect_stub = false,
    bool truncate_linkedit = false, const UUID &selected_uuid = {}) {
  const uint32_t header_size = address_size == 8 ? 32 : 28;
  const uint32_t segment_size = address_size == 8 ? 72 : 56;
  const uint32_t section_size = address_size == 8 ? 80 : 68;
  const uint32_t section_count = indirect_stub ? 3 : 2;
  const uint32_t commands_size =
      2 * segment_size + section_count * section_size + 48 +
      (exports.empty() ? 0 : 16) + (indirect_stub ? 80 : 0);
  const uint32_t linkedit_offset = mapped_cache ? 0x9000 : 0x4000;
  DataEncoder payload(order, address_size);
  uint32_t string_offset = 1;
  for (const auto &record : records) {
    payload.AppendU32(string_offset);
    payload.AppendU8(record.type);
    payload.AppendU8(record.section);
    payload.AppendU16(record.descriptor);
    payload.AppendAddress(record.value);
    string_offset += record.name.size() + 1;
  }
  const uint32_t strings_offset = linkedit_offset + payload.GetByteSize();
  payload.AppendU8(0);
  for (const auto &record : records)
    payload.AppendCString(record.name);
  const uint32_t export_offset = linkedit_offset + payload.GetByteSize();
  payload.AppendData(exports);
  const uint32_t indirect_offset = linkedit_offset + payload.GetByteSize();
  if (indirect_stub)
    payload.AppendU32(records.size() - 1);

  DataEncoder data(order, address_size);
  data.AppendU32(address_size == 8 ? 0xfeedfacf : 0xfeedface);
  data.AppendU32(address_size == 8 ? 0x100000c : 12);
  data.AppendU32(0);
  data.AppendU32(6); // MH_DYLIB
  data.AppendU32(4 + !exports.empty() + indirect_stub);
  data.AppendU32(commands_size);
  data.AppendU32(mapped_cache ? 0x80000000 : 0);
  if (address_size == 8)
    data.AppendU32(0);
  auto name = [&](llvm::StringRef value) {
    std::string padded(value);
    padded.resize(16, 0);
    data.AppendData(llvm::StringRef(padded));
  };
  auto segment = [&](llvm::StringRef segment_name, uint64_t vmaddr,
                     uint64_t fileoff, uint64_t filesize, uint32_t sections) {
    data.AppendU32(address_size == 8 ? 0x19 : 1);
    data.AppendU32(segment_size + sections * section_size);
    name(segment_name);
    data.AppendAddress(vmaddr);
    data.AppendAddress(0x1000);
    data.AppendAddress(fileoff);
    data.AppendAddress(filesize);
    for (uint32_t value : {5u, 5u, sections, 0u})
      data.AppendU32(value);
  };
  auto section = [&](llvm::StringRef section_name, uint64_t address,
                     uint32_t flags, uint32_t stub_size = 0) {
    name(section_name);
    name("__TEXT");
    data.AppendAddress(address);
    data.AppendAddress(0x10);
    for (uint32_t value :
         {uint32_t(address - 0x1000), 0u, 0u, 0u, flags, 0u, stub_size})
      data.AppendU32(value);
    if (address_size == 8)
      data.AppendU32(0);
  };
  segment("__TEXT", 0x1000, 0, 0x1000, section_count);
  section("__text", 0x1800, 0x80000000);
  section("__data", 0x1900, 0);
  if (indirect_stub)
    section("__stubs", 0x1a00, 8, 0x10);
  segment("__LINKEDIT", 0x5000, linkedit_offset, payload.GetByteSize(), 0);
  data.AppendU32(2); // LC_SYMTAB
  data.AppendU32(24);
  data.AppendU32(linkedit_offset);
  data.AppendU32(records.size());
  data.AppendU32(strings_offset);
  data.AppendU32(string_offset);
  if (!exports.empty()) {
    data.AppendU32(0x80000033);
    data.AppendU32(16);
    data.AppendU32(export_offset);
    data.AppendU32(exports.size());
  }
  if (indirect_stub) {
    data.AppendU32(0xb);
    data.AppendU32(80);
    uint32_t fields[18] = {};
    fields[12] = indirect_offset;
    fields[13] = 1;
    for (uint32_t value : fields)
      data.AppendU32(value);
  }
  const uint8_t uuid_bytes[] = {1, 2,  3,  4,  5,  6,  7,  8,
                                9, 10, 11, 12, 13, 14, 15, 16};
  data.AppendU32(0x1b); // LC_UUID, matching the selected ModuleSpec.
  data.AppendU32(24);
  data.AppendData(uuid_bytes);
  EXPECT_EQ(data.GetByteSize(), header_size + commands_size);
  std::vector<uint8_t> bytes(data.GetData().begin(), data.GetData().end());
  bytes.resize(mapped_cache ? 0x1000 : 0x4000);
  bytes.insert(bytes.end(), payload.GetData().begin(), payload.GetData().end());
  auto backing = std::make_shared<DataBufferHeap>(bytes.data(), bytes.size());
  DataExtractorSP extractor;
  if (mapped_cache) {
    const uint64_t readable = payload.GetByteSize() - truncate_linkedit;
    extractor = std::make_shared<VirtualDataExtractor>(
        backing, order, address_size,
        VirtualDataExtractor::LookupTable{
            VirtualDataExtractor::LookupTable::Entry(0, 0x1000, 0),
            VirtualDataExtractor::LookupTable::Entry(0x4000, readable,
                                                     0x1000)});
  } else {
    extractor = std::make_shared<DataExtractor>(backing, order, address_size);
  }
  return std::make_shared<Module>(ModuleSpec(
      FileSpec(),
      selected_uuid ? selected_uuid : UUID(uuid_bytes, sizeof(uuid_bytes)),
      extractor));
}
} // namespace

TEST_F(SymtabTest, MachOMetadataReaderSupportsAddressSizesAndByteOrders) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      auto module = MetadataReaderModule({{0x64, 0, 0, "source.c"},
                                          {0x66, 1, 42, "source.o"},
                                          {0x64, 0, 0, ""},
                                          {0x32, 0, 0, "module.swiftmodule"},
                                          {0x3, 0, 0x100, "_present"}},
                                         address_size, order);
      ObjectFile *file = module->GetObjectFile();
      ASSERT_NE(file, nullptr);
      const auto *metadata = file->GetDebugInfoMetadata();
      ASSERT_NE(metadata, nullptr);
      ASSERT_TRUE(metadata->debug_map_objects);
      ASSERT_EQ(metadata->debug_map_objects->size(), 1u);
      EXPECT_EQ(metadata->debug_map_objects->front().modification_time, 42u);
      ASSERT_EQ(metadata->swift_ast_files.size(), 1u);
      EXPECT_EQ(metadata->swift_ast_files.front(), "module.swiftmodule");
      EXPECT_TRUE(file->MayContainCLinkageSymbols({"present"}));
      EXPECT_FALSE(file->MayContainCLinkageSymbols({"absent"}));
      EXPECT_EQ(file->GetSymtab(false), nullptr);
    }
  }
}

TEST_F(SymtabTest, MachORawNameFastPathMatchesBoundedReader) {
  const MetadataNList records[] = {
      {0x64, 0, 0, "./source/"},
      {0x64, 0, 0, "_unit.c"},
      {0x66, 1, 42, "unit.o"},
      {0x64, 0, 0, ""},
      {0x32, 0, 0, "first.swiftmodule"},
      {3, 0, 1, "_$s" + std::string(4096, 'x')},
      {3, 0, 2, "_present"},
      {0xb, 0, 1, "_alias"},
      {3, 0, 3, "__double_prefix"},
      {3, 0, 4, "plain"},
      {3, 0, 5, std::string("_embedded\0tail", sizeof("_embedded\0tail") - 1)},
      {0x32, 0, 0, "_second.swiftmodule"},
      {0x32, 0, 0, "first.swiftmodule"},
      {0, 0, 0, "unused-tail"},
  };
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (bool final_nul : {true, false}) {
        SCOPED_TRACE(testing::Message()
                     << "address size=" << unsigned(address_size) << ", order="
                     << unsigned(order) << ", final NUL=" << final_nul);
        auto module = MetadataReaderModule(
            records, address_size, order, {}, [&](DataEncoder &data) {
              const uint32_t header_size = address_size == 8 ? 32 : 28;
              const uint32_t stride = address_size == 8 ? 16 : 12;
              // The trailing bytes are deliberately unreferenced. Without
              // their final NUL, all referenced strings remain valid and the
              // original bounded reader must still accept the same metadata.
              data.PutU32(header_size + 24 + (std::size(records) - 1) * stride,
                          0);
              if (!final_nul)
                data.PutU8(data.GetByteSize() - 1, 'x');
            });
        ObjectFile *file = module->GetObjectFile();
        ASSERT_NE(file, nullptr);
        const auto *metadata = file->GetDebugInfoMetadata();
        ASSERT_NE(metadata, nullptr);
        ASSERT_TRUE(metadata->debug_map_objects);
        ASSERT_EQ(metadata->debug_map_objects->size(), 1u);
        const auto &oso = metadata->debug_map_objects->front();
        EXPECT_EQ(oso.source_file, "./source/_unit.c");
        EXPECT_EQ(oso.object_file, "unit.o");
        EXPECT_EQ(oso.modification_time, 42u);
        EXPECT_EQ(metadata->swift_ast_files,
                  (std::vector<llvm::StringRef>{"first.swiftmodule",
                                                "second.swiftmodule",
                                                "first.swiftmodule"}));
        for (llvm::StringRef name :
             {"present", "alias", "_double_prefix", "plain", "embedded"})
          EXPECT_TRUE(file->MayContainCLinkageSymbols({name})) << name.str();
        for (llvm::StringRef name :
             {"presen", "present_suffix", "double_prefix", "embeddedtail",
              "missing"})
          EXPECT_FALSE(file->MayContainCLinkageSymbols({name})) << name.str();
        EXPECT_EQ(file->GetSymtab(false), nullptr);
        EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
      }
    }
  }
}

TEST_F(SymtabTest, MachORawNameFastPathChecksEveryReferencedOffset) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (bool indirect : {false, true}) {
        for (bool at_end : {false, true}) {
          auto module = MetadataReaderModule(
              {{uint8_t(indirect ? 0xb : 3), 0, 1, "_raw"}}, address_size,
              order, {}, [&](DataEncoder &data) {
                const uint32_t symbol_offset =
                    (address_size == 8 ? 32 : 28) + 24;
                // String region is "\0_raw\0". Its final NUL proves
                // termination, never the validity of an offset at/past end.
                const uint32_t invalid = at_end ? 6 : UINT32_MAX;
                if (!indirect)
                  data.PutU32(symbol_offset, invalid);
                else if (address_size == 8)
                  data.PutU64(symbol_offset + 8, invalid);
                else
                  data.PutU32(symbol_offset + 8, invalid);
              });
          ObjectFile *file = module->GetObjectFile();
          ASSERT_NE(file, nullptr);
          EXPECT_EQ(file->GetDebugInfoMetadata(), nullptr);
          EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
          EXPECT_EQ(file->GetSymtab(false), nullptr);
        }
      }
    }
  }
}

TEST_F(SymtabTest, MachORawNameReaderPreservesFinalAndEmptyStrings) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      auto module =
          MetadataReaderModule({{3, 0, 1, "_z"}}, address_size, order);
      ObjectFile *file = module->GetObjectFile();
      ASSERT_NE(file, nullptr);
      EXPECT_TRUE(file->MayContainCLinkageSymbols({"z"}));
      EXPECT_FALSE(file->MayContainCLinkageSymbols({"zz"}));
      EXPECT_FALSE(file->MayContainCLinkageSymbols({"longer_than_the_region"}));

      auto empty = MetadataReaderModule(
          {{3, 0, 1, "_z"}}, address_size, order, {}, [&](DataEncoder &data) {
            const uint32_t symbol_offset = (address_size == 8 ? 32 : 28) + 24;
            data.PutU32(symbol_offset, 3); // The final NUL is an empty name.
          });
      ObjectFile *empty_file = empty->GetObjectFile();
      ASSERT_NE(empty_file, nullptr);
      EXPECT_NE(empty_file->GetDebugInfoMetadata(), nullptr);
      EXPECT_FALSE(empty_file->MayContainCLinkageSymbols({"z"}));

      auto no_strings = MetadataReaderModule(
          {{3, 0, 1, "_z"}}, address_size, order, {}, [&](DataEncoder &data) {
            data.PutU32((address_size == 8 ? 32 : 28) + 20, 0);
          });
      ObjectFile *no_strings_file = no_strings->GetObjectFile();
      ASSERT_NE(no_strings_file, nullptr);
      EXPECT_EQ(no_strings_file->GetDebugInfoMetadata(), nullptr);
      EXPECT_TRUE(no_strings_file->MayContainCLinkageSymbols({"missing"}));
      EXPECT_EQ(file->GetSymtab(false), nullptr);
      EXPECT_EQ(empty_file->GetSymtab(false), nullptr);
      EXPECT_EQ(no_strings_file->GetSymtab(false), nullptr);
    }
  }
}

TEST_F(SymtabTest, MachOCLinkageReaderIncludesExportsReexportsAndObjCAliases) {
  for (bool reexport : {false, true}) {
    auto module = MetadataReaderModule({{3, 0, 1, "_raw"}}, 8, eByteOrderLittle,
                                       SingleExport("_export_only", reexport));
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"raw"}));
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"export_only"}));
    EXPECT_FALSE(file->MayContainCLinkageSymbols({"missing"}));
    EXPECT_EQ(file->GetSymtab(false), nullptr);
  }
  for (llvm::StringRef prefix :
       {"_OBJC_CLASS_$_", "_OBJC_METACLASS_$_", "_OBJC_IVAR_$_"}) {
    auto module = MetadataReaderModule({}, 8, eByteOrderLittle,
                                       SingleExport(prefix.str() + "Widget"));
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"Widget"}));
    EXPECT_FALSE(file->MayContainCLinkageSymbols({"Other"}));
  }
}

TEST_F(SymtabTest, MachOCLinkageNameSetPreservesDifferentRepeatedQueries) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (bool reexport : {false, true}) {
        auto module = MetadataReaderModule(
            {{3, 0, 1, "_raw"},
             {0xb, 0, 1, "_alias"},
             {3, 0, 2, "__double_prefix"},
             {3, 0, 3, "plain"},
             {3, 0, 4, "_$s" + std::string(4096, 'x')},
             {0x24, 0, 5, "_with_suffix:F(0,1)"}},
            address_size, order, SingleExport("_export_only", reexport));
        ObjectFile *file = module->GetObjectFile();
        ASSERT_NE(file, nullptr);
        // The first miss must build a complete view, including later positive
        // queries and exports. Every round changes the name/order of requests.
        EXPECT_FALSE(file->MayContainCLinkageSymbols({"first_miss"}));
        for (unsigned round = 0; round != 3; ++round) {
          EXPECT_TRUE(file->MayContainCLinkageSymbols({"raw"}));
          EXPECT_FALSE(file->MayContainCLinkageSymbols({"raw_suffix"}));
          EXPECT_TRUE(file->MayContainCLinkageSymbols({"alias", "missing"}));
          EXPECT_TRUE(file->MayContainCLinkageSymbols({"export_only"}));
          EXPECT_TRUE(file->MayContainCLinkageSymbols({"_double_prefix"}));
          EXPECT_TRUE(file->MayContainCLinkageSymbols({"plain"}));
          EXPECT_FALSE(file->MayContainCLinkageSymbols({"double_prefix"}));
          EXPECT_FALSE(file->MayContainCLinkageSymbols({"with_suffix"}));
          EXPECT_FALSE(file->MayContainCLinkageSymbols({"last_miss"}));
        }
        EXPECT_EQ(file->GetSymtab(false), nullptr);
        EXPECT_EQ(module->GetSymtabParseTime().get().count(), 0.0);
        EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
      }
    }
  }
  for (llvm::StringRef prefix :
       {"_OBJC_CLASS_$_", "_OBJC_METACLASS_$_", "_OBJC_IVAR_$_"}) {
    auto module = MetadataReaderModule({{3, 0, 1, "_raw"}}, 8, eByteOrderLittle,
                                       SingleExport(prefix.str() + "Widget"));
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    // Finding a raw name first must not publish an incomplete export set.
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"raw"}));
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"Widget"}));
    EXPECT_FALSE(file->MayContainCLinkageSymbols({"Other"}));
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"Widget"}));
    EXPECT_EQ(file->GetSymtab(false), nullptr);
  }
}

TEST_F(SymtabTest, MachOCLinkageNameSetNeverPublishesPartialResults) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      auto invalid_name = MetadataReaderModule(
          {{3, 0, 1, "_present"}, {3, 0, 2, "_last"}}, address_size, order, {},
          [&](DataEncoder &data) {
            const uint32_t header_size = address_size == 8 ? 32 : 28;
            const uint32_t stride = address_size == 8 ? 16 : 12;
            data.PutU32(header_size + 24 + stride, UINT32_MAX);
          });
      auto trie = SingleExport("_export_only");
      // Keep a valid terminal, then add an unreadable child after it.
      trie.back() = 1;
      auto invalid_trie = MetadataReaderModule({{3, 0, 1, "_present"}},
                                               address_size, order, trie);
      for (const auto &module : {invalid_name, invalid_trie}) {
        ObjectFile *file = module->GetObjectFile();
        ASSERT_NE(file, nullptr);
        EXPECT_TRUE(file->MayContainCLinkageSymbols({"present"}));
        EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
        EXPECT_TRUE(file->MayContainCLinkageSymbols({"another_miss"}));
        EXPECT_EQ(file->GetSymtab(false), nullptr);
      }
    }
  }
}

TEST_F(SymtabTest, MachOCLinkageNameSetRechecksEligibilityAfterInitialization) {
  auto &properties = ModuleList::GetGlobalModuleListProperties();
  const bool previous = properties.GetEnableLLDBIndexCache();
  auto restore = llvm::make_scope_exit(
      [&] { properties.SetEnableLLDBIndexCache(previous); });
  ASSERT_TRUE(properties.SetEnableLLDBIndexCache(false));
  auto module = MetadataReaderModule({{3, 0, 1, "_raw"}});
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"missing"}));
  ASSERT_TRUE(properties.SetEnableLLDBIndexCache(true));
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
  EXPECT_EQ(file->GetSymtab(false), nullptr);
  ASSERT_TRUE(properties.SetEnableLLDBIndexCache(false));
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"missing"}));
  for (llvm::StringRef query :
       {"", "a/b", "::name", "$SSwiftLegacy", "___lldb_unnamed_symbol_1"})
    EXPECT_TRUE(file->MayContainCLinkageSymbols({query}));
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"missing"}));
  EXPECT_EQ(file->GetSymtab(false), nullptr);
  // A later table, including one that can be augmented, always takes priority
  // over the immutable raw-file summary.
  ASSERT_NE(file->GetSymtab(), nullptr);
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
}

TEST_F(SymtabTest, MachODefinedLinkageExcludesOnlyUndefinedReferences) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      auto module = MetadataReaderModule({{1, 0, 0, "_swift_isaMask"},
                                          {0xd, 0, 0, "_prebound"},
                                          {3, 0, 1, "_defined"},
                                          {0x20, 0, 1, "_debug"},
                                          {0xb, 0, 1, "_alias"}},
                                         address_size, order);
      ObjectFile *file = module->GetObjectFile();
      ASSERT_NE(file, nullptr);
      EXPECT_TRUE(file->MayContainCLinkageSymbols({"swift_isaMask"}));
      EXPECT_FALSE(file->MayContainCLinkageSymbols(
          {"swift_isaMask"}, SymbolNameLookupKind::Defined));
      EXPECT_TRUE(file->MayContainCLinkageSymbols({"prebound"}));
      EXPECT_FALSE(file->MayContainCLinkageSymbols(
          {"prebound"}, SymbolNameLookupKind::Defined));
      for (llvm::StringRef name : {"defined", "debug", "alias"})
        EXPECT_TRUE(file->MayContainCLinkageSymbols(
            {name}, SymbolNameLookupKind::Defined));
      EXPECT_FALSE(file->MayContainCLinkageSymbols(
          {"missing"}, SymbolNameLookupKind::Defined));
      EXPECT_EQ(file->GetSymtab(false), nullptr);
    }
  }
}

TEST_F(SymtabTest, MachODefinedLinkageRetainsIndirectTrampolinesAndResolvers) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (bool resolver : {false, true}) {
        auto module = IndirectStubReaderModule(address_size, order, resolver);
        ObjectFile *file = module->GetObjectFile();
        ASSERT_NE(file, nullptr);
        EXPECT_TRUE(file->MayContainCLinkageSymbols(
            {"runtime"}, SymbolNameLookupKind::Defined));
        EXPECT_FALSE(file->MayContainCLinkageSymbols(
            {"reference_only"}, SymbolNameLookupKind::Defined));
        EXPECT_TRUE(file->MayContainCLinkageSymbols({"reference_only"}));
        EXPECT_EQ(file->GetSymtab(false), nullptr);
        // ParseSymtab snapshots m_dysymtab before its own GetSectionList call.
        // Initialize sections, as provider selection does, before forcing it.
        ASSERT_NE(file->GetSectionList(), nullptr);
        Symtab *symtab = file->GetSymtab();
        ASSERT_NE(symtab, nullptr);
        std::vector<uint32_t> indices;
        symtab->FindAllSymbolsWithNameAndType(ConstString("runtime"),
                                              eSymbolTypeAny, indices, true);
        ASSERT_EQ(indices.size(), 1u);
        EXPECT_EQ(symtab->SymbolAtIndex(indices.front())->GetType(),
                  resolver ? eSymbolTypeResolver : eSymbolTypeTrampoline);
        EXPECT_EQ(symtab->SymbolAtIndex(indices.front())->GetID(), 0u);
        std::vector<uint32_t> references;
        symtab->FindAllSymbolsWithNameAndType(ConstString("reference_only"),
                                              eSymbolTypeUndefined, references,
                                              true);
        ASSERT_EQ(references.size(), 1u);
        EXPECT_EQ(symtab->SymbolAtIndex(references.front())->GetID(), 1u);
        EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
      }
      for (uint32_t special : {0x80000000u, 0x40000000u}) {
        auto module =
            IndirectStubReaderModule(address_size, order, false, special);
        EXPECT_FALSE(module->GetObjectFile()->MayContainCLinkageSymbols(
            {"runtime"}, SymbolNameLookupKind::Defined));
      }
    }
  }
}

TEST_F(SymtabTest,
       MachODefinedLinkageKeepsMalformedStubMembershipConservative) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (bool truncated : {false, true}) {
        auto module =
            IndirectStubReaderModule(address_size, order, false, 2, truncated);
        ObjectFile *file = module->GetObjectFile();
        ASSERT_NE(file, nullptr);
        // A late invalid index or unreadable index table prevents classifying
        // ANY of the present undefined names as a guaranteed reference only.
        EXPECT_TRUE(file->MayContainCLinkageSymbols(
            {"runtime"}, SymbolNameLookupKind::Defined));
        EXPECT_TRUE(file->MayContainCLinkageSymbols(
            {"reference_only"}, SymbolNameLookupKind::Defined));
        EXPECT_EQ(file->GetSymtab(false), nullptr);
      }
    }
  }
}

TEST_F(SymtabTest, MachOModernSwiftLinkageUsesBoundedExactNames) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (bool reexport : {false, true}) {
        auto module = MetadataReaderModule(
            {{3, 0, 1, "_$s7Fixture4TypeCMn"},
             {3, 0, 1, "__$s7Fixture4TypeCMa"},
             {1, 0, 0, "_$s7Fixture8externalyyF"},
             {3, 0, 1, "_$s" + std::string(4096, 'x')}},
            address_size, order,
            SingleExport("_$s7Fixture8exportedyyF", reexport));
        ObjectFile *file = module->GetObjectFile();
        ASSERT_NE(file, nullptr);
        EXPECT_FALSE(file->MayContainCLinkageSymbols({"ordinary_miss"}));
        EXPECT_TRUE(file->MayContainCLinkageSymbols({"$s7Fixture4TypeCMn"}));
        EXPECT_TRUE(file->MayContainCLinkageSymbols({"_$s7Fixture4TypeCMa"}));
        EXPECT_FALSE(file->MayContainCLinkageSymbols({"$s7Fixture4TypeCMa"}));
        EXPECT_FALSE(
            file->MayContainCLinkageSymbols({"$s7Fixture4TypeCMnSuffix"}));
        EXPECT_TRUE(
            file->MayContainCLinkageSymbols({"$s7Fixture8externalyyF"}));
        EXPECT_FALSE(file->MayContainCLinkageSymbols(
            {"$s7Fixture8externalyyF"}, SymbolNameLookupKind::Defined));
        EXPECT_TRUE(file->MayContainCLinkageSymbols(
            {"$s7Fixture8exportedyyF"}, SymbolNameLookupKind::Defined));
        EXPECT_FALSE(
            file->MayContainCLinkageSymbols({"$s7Fixture7missingyyF"}));
        for (llvm::StringRef unknown :
             {"$S7Fixture", "$s", "$sbad/path", "$sbad.name"})
          EXPECT_TRUE(file->MayContainCLinkageSymbols(
              {unknown}, SymbolNameLookupKind::Defined));
        EXPECT_EQ(file->GetSymtab(false), nullptr);
        EXPECT_EQ(module->GetSymtabParseTime().get().count(), 0.0);
        EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
      }
    }
  }
  auto objc_alias = MetadataReaderModule(
      {}, 8, eByteOrderLittle, SingleExport("_OBJC_CLASS_$_$s7Fixture4TypeC"));
  EXPECT_TRUE(objc_alias->GetObjectFile()->MayContainCLinkageSymbols(
      {"$s7Fixture4TypeC"}, SymbolNameLookupKind::Defined));
}

TEST_F(SymtabTest, MachOModernSwiftLinkageNeverUsesMalformedPartialNegatives) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      auto module = MetadataReaderModule(
          {{3, 0, 1, "_$s7Fixture4TypeCMn"}, {0xb, 0, UINT32_MAX, "_alias"}},
          address_size, order);
      ObjectFile *file = module->GetObjectFile();
      ASSERT_NE(file, nullptr);
      EXPECT_TRUE(file->MayContainCLinkageSymbols({"$s7Fixture4TypeCMn"}));
      EXPECT_TRUE(file->MayContainCLinkageSymbols(
          {"$s7Fixture7missingyyF"}, SymbolNameLookupKind::Defined));
      EXPECT_EQ(file->GetSymtab(false), nullptr);
      auto bad_trie = MetadataReaderModule({}, address_size, order, {0x80});
      EXPECT_TRUE(bad_trie->GetObjectFile()->MayContainCLinkageSymbols(
          {"$s7Fixture7missingyyF"}, SymbolNameLookupKind::Defined));
    }
  }
}

TEST_F(SymtabTest, MachOMetadataReaderRejectsMalformedExtentsAndStrings) {
  auto check_unknown = [&](llvm::function_ref<void(DataEncoder &)> corrupt) {
    auto module = MetadataReaderModule({{3, 0, 1, "_raw"}}, 8, eByteOrderLittle,
                                       {}, corrupt);
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->GetDebugInfoMetadata(), nullptr);
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
    EXPECT_EQ(file->GetSymtab(false), nullptr);
  };
  check_unknown([](DataEncoder &data) { data.PutU32(36, 4); }); // cmdsize
  check_unknown(
      [](DataEncoder &data) { data.PutU32(40, UINT32_MAX); }); // symoff
  check_unknown(
      [](DataEncoder &data) { data.PutU32(48, UINT32_MAX); }); // stroff
  check_unknown(
      [](DataEncoder &data) { data.PutU32(56, UINT32_MAX); }); // n_strx
  check_unknown([](DataEncoder &data) {
    data.PutU8(data.GetByteSize() - 1, 'x'); // unterminated final string
  });
  auto indirect = MetadataReaderModule({{0xb, 0, UINT32_MAX, "_alias"}});
  ObjectFile *file = indirect->GetObjectFile();
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->GetDebugInfoMetadata(), nullptr);
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
  EXPECT_EQ(file->GetSymtab(false), nullptr);
}

TEST_F(SymtabTest, MachOCLinkageReaderTreatsMalformedTriesAndQueriesAsUnknown) {
  for (const auto &trie :
       {std::vector<uint8_t>{0x80}, std::vector<uint8_t>{0, 1, 'x'},
        std::vector<uint8_t>{0, 1, 'x', 0, 127},
        std::vector<uint8_t>{1, 0x80, 0}, // unterminated flags
        std::vector<uint8_t>{1, 0, 0},    // missing address inside terminal
        std::vector<uint8_t>{2, 8, 1, 0}, // missing reexport name
        std::vector<uint8_t>{1, 0x20, 0}, // unsupported flags
        std::vector<uint8_t>{0, 1, 'x', 0, 5, 0, 1, 'y', 0, 5}}) {
    auto module = MetadataReaderModule({}, 8, eByteOrderLittle, trie);
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
  }
  auto module = MetadataReaderModule({{3, 0, 1, "_raw"}});
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  for (llvm::StringRef query :
       {"", "a/b", "::name", "name.c", "1name", "___lldb_unnamed_symbol_1"})
    EXPECT_TRUE(file->MayContainCLinkageSymbols({query}));
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"missing"}));
  ASSERT_NE(file->GetSymtab(), nullptr);
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
  EXPECT_EQ(file->GetDebugInfoMetadata(), nullptr);
}

TEST_F(SymtabTest, MachOMetadataReadersRespectPotentiallyAugmentedIndexCache) {
  auto &properties = ModuleList::GetGlobalModuleListProperties();
  const bool previous = properties.GetEnableLLDBIndexCache();
  auto restore = llvm::make_scope_exit(
      [&] { properties.SetEnableLLDBIndexCache(previous); });
  ASSERT_TRUE(properties.SetEnableLLDBIndexCache(true));
  auto module = MetadataReaderModule({{3, 0, 1, "_raw"}});
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->GetDebugInfoMetadata(), nullptr);
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
  EXPECT_EQ(file->GetSymtab(false), nullptr);
}

TEST_F(SymtabTest, DebugMapMetadataChecksTimestampBeforeHydration) {
  llvm::SmallString<128> directory;
  ASSERT_NO_ERROR(
      llvm::sys::fs::createUniqueDirectory("lldb-oso-time", directory));
  auto cleanup = llvm::make_scope_exit(
      [&] { llvm::sys::fs::remove_directories(directory); });
  llvm::SmallString<128> path(directory);
  llvm::sys::path::append(path, "changed.o");
  {
    std::error_code error;
    llvm::raw_fd_ostream stream(path, error);
    ASSERT_NO_ERROR(error);
    stream << "changed object";
  }
  auto module = MetadataReaderModule({{0x64, 0, 0, "source.c"},
                                      {0x66, 1, 1, path.str().str()},
                                      {0x64, 0, 0, ""}});
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  MetadataDebugMap provider(file->shared_from_this());
  ASSERT_EQ(provider.GetNumCompileUnits(), 1u);
  EXPECT_EQ(provider.GetModuleByOSOIndex(0), nullptr);
  EXPECT_NE(provider.LoadError(0).find("changed"), std::string::npos);
  EXPECT_EQ(file->GetSymtab(false), nullptr);
}

TEST_F(SymtabTest, DebugMapMetadataHydratesBeforeArchiveMemberErrors) {
  llvm::SmallString<128> directory;
  ASSERT_NO_ERROR(
      llvm::sys::fs::createUniqueDirectory("lldb-oso-archive", directory));
  auto cleanup = llvm::make_scope_exit(
      [&] { llvm::sys::fs::remove_directories(directory); });
  llvm::SmallString<128> path(directory);
  llvm::sys::path::append(path, "empty.a");
  {
    std::error_code error;
    llvm::raw_fd_ostream stream(path, error);
    ASSERT_NO_ERROR(error);
    stream << "!<arch>\n";
  }
  auto module =
      MetadataReaderModule({{0x64, 0, 0, "source.c"},
                            {0x66, 1, 0, path.str().str() + "(missing.o)"},
                            {0x64, 0, 0, ""}});
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  MetadataDebugMap provider(file->shared_from_this());
  ASSERT_EQ(provider.GetNumCompileUnits(), 1u);
  const void *entry = provider.Entry(0);
  EXPECT_NE(provider.GetModuleByOSOIndex(0), nullptr);
  EXPECT_EQ(provider.Entry(0), entry);
  EXPECT_NE(file->GetSymtab(false), nullptr);
  EXPECT_EQ(provider.FirstSymbolID(0), 0u);
  EXPECT_NE(provider.LoadError(0).find("archive"), std::string::npos);
}

TEST_F(SymtabTest, DebugMapConcurrentLateHydrationPublishesStableIndices) {
  auto module = MetadataReaderModule({{0x64, 0, 0, "./source/"},
                                      {0x64, 0, 0, "unit.c"},
                                      {0x66, 1, 0, "missing.o"},
                                      {0x24, 0, 0x100, "_function"},
                                      {0x24, 0, 16, ""},
                                      {0x64, 0, 0, ""}});
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  MetadataDebugMap provider(file->shared_from_this());
  ASSERT_EQ(provider.GetNumCompileUnits(), 1u);
  const void *entry = provider.Entry(0);
  EXPECT_EQ(file->GetSymtab(false), nullptr);
  EXPECT_EQ(provider.FirstSymbolID(0), UINT32_MAX);
  std::vector<std::future<void>> initializers;
  for (unsigned i = 0; i != 8; ++i)
    initializers.push_back(
        std::async(std::launch::async, [&] { provider.Hydrate(); }));
  for (auto &initializer : initializers)
    initializer.get();
  EXPECT_EQ(provider.GetNumCompileUnits(), 1u);
  EXPECT_EQ(provider.Entry(0), entry);
  EXPECT_EQ(provider.FirstSymbolID(0), 0u);
  EXPECT_EQ(provider.LastSymbolID(0), 3u);
  EXPECT_EQ(provider.RangeCount(), 1u);

  // Once published, an OSO worker callback must not reacquire the parent
  // module's mutex while the parent is waiting for parallel DWARF work.
  std::future<void> callback;
  {
    std::lock_guard<std::recursive_mutex> guard(module->GetMutex());
    callback = std::async(std::launch::async, [&] { provider.Hydrate(); });
    EXPECT_EQ(callback.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
  }
  callback.get();
}

TEST_F(SymtabTest, MachOCLinkageReaderKeepsRawSTABSandIndirectSpellings) {
  auto module = MetadataReaderModule({{0x24, 0, 0x100, "_stab_function"},
                                      {0x24, 0, 0x200, "_with_suffix:F1"},
                                      {0xb, 0, 1, "_alias"},
                                      {3, 0, 0x300, "__double_prefix"},
                                      {3, 0, 0x400, "plain"}});
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"stab_function"}));
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"alias"}));
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"_double_prefix"}));
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"plain"}));
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"with_suffix"}));
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"double_prefix"}));
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"missing"}));
  EXPECT_EQ(file->GetSymtab(false), nullptr);
}

TEST_F(SymtabTest, MachOMetadataReadersDeferSharedCacheImagesToNormalParser) {
  auto module = MetadataReaderModule(
      {{3, 0, 1, "_raw"}}, 8, eByteOrderLittle, {},
      [](DataEncoder &data) { data.PutU32(24, 0x80000000); });
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->GetDebugInfoMetadata(), nullptr);
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"missing"}));
  EXPECT_EQ(file->GetSymtab(false), nullptr);
}

namespace {
constexpr llvm::StringLiteral CLinkageNegativeMachO = R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x100000C
  cpusubtype: 0
  filetype:   2
  ncmds:      1
  sizeofcmds: 24
  flags:      0
  reserved:   0
LoadCommands:
  - cmd:      LC_SYMTAB
    cmdsize:  24
    symoff:   56
    nsyms:    1
    stroff:   72
    strsize:  12
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x3
      n_sect:  0
      n_desc:  0
      n_value: 0x1000
  StringTable:
    - ''
    - _unrelated
)";

class CLinkageSearchModule : public Module {
public:
  using Module::Module;
  std::unique_ptr<SymbolFile> symbols;

  SymbolFile *GetSymbolFile(bool can_create = true,
                            Stream *feedback_strm = nullptr) override {
    return symbols.get();
  }
};

class CLinkageDWARFSearch : public SymbolFileDWARF {
public:
  explicit CLinkageDWARFSearch(ObjectFileSP object)
      : SymbolFileDWARF(std::move(object), nullptr) {}

  bool searched = false;
  bool searched_inlines = false;
  SymbolNameLookupKind last_lookup_kind = SymbolNameLookupKind::Any;

  bool MayContainCLinkageSymbols(
      llvm::ArrayRef<llvm::StringRef> names,
      SymbolNameLookupKind kind = SymbolNameLookupKind::Any) override {
    last_lookup_kind = kind;
    return SymbolFileDWARF::MayContainCLinkageSymbols(names, kind);
  }

  void FindFunctions(const Module::LookupInfo &lookup,
                     const CompilerDeclContext &parent, bool include_inlines,
                     SymbolContextList &contexts) override {
    searched = true;
    searched_inlines = include_inlines;
    contexts.Append(SymbolContext(GetObjectFile()->GetModule()));
  }
};

class CLinkageAugmentingSymbolFile : public SymbolFileSymtab {
public:
  using SymbolFileSymtab::SymbolFileSymtab;

  void AddSymbols(Symtab &symtab) override {
    Symbol symbol(1, "runtime", eSymbolTypeCode, false, false, false, false,
                  SectionSP(), 0x1000, 0, false, false, 0);
    symbol.SetIsSynthetic(true);
    symtab.AddSymbol(symbol);
  }
};
} // namespace

TEST_F(SymtabTest, CLinkageSearchPreservesDWARFBeforeSkippingSymbolTable) {
  auto image = TestFile::fromYaml(CLinkageNegativeMachO);
  ASSERT_THAT_EXPECTED(image, llvm::Succeeded());
  auto module = std::make_shared<CLinkageSearchModule>(image->moduleSpec());
  ObjectFile *object = module->GetObjectFile();
  ASSERT_NE(object, nullptr);
  auto symbols =
      std::make_unique<CLinkageDWARFSearch>(object->shared_from_this());
  CLinkageDWARFSearch *dwarf = symbols.get();
  module->symbols = std::move(symbols);

  ModuleFunctionSearchOptions options;
  options.include_symbols = true;
  options.include_inlines = true;
  options.match_c_linkage_names = true;
  SymbolContextList contexts;
  module->FindFunctions(ConstString("runtime"), CompilerDeclContext(),
                        eFunctionNameTypeFull, options, contexts);
  EXPECT_TRUE(dwarf->searched);
  EXPECT_TRUE(dwarf->searched_inlines);
  EXPECT_EQ(contexts.GetSize(), 1u);
  EXPECT_EQ(object->GetSymtab(/*can_create=*/false), nullptr);

  // Other name lookup modes retain their symbol-table behavior.
  options.match_c_linkage_names = false;
  contexts.Clear();
  module->FindFunctions(ConstString("runtime"), CompilerDeclContext(),
                        eFunctionNameTypeFull, options, contexts);
  EXPECT_NE(object->GetSymtab(/*can_create=*/false), nullptr);
}

TEST_F(SymtabTest, CLinkagePredicatePreservesAugmentedAndMaterializedSymbols) {
  auto image = TestFile::fromYaml(CLinkageNegativeMachO);
  ASSERT_THAT_EXPECTED(image, llvm::Succeeded());
  auto module = std::make_shared<CLinkageSearchModule>(image->moduleSpec());
  ObjectFile *object = module->GetObjectFile();
  ASSERT_NE(object, nullptr);
  ObjectFileSP object_sp = object->shared_from_this();
  const llvm::StringRef name("runtime");
  SymbolFileDWARF dwarf(object_sp, nullptr);
  SymbolFileDWARFDebugMap debug_map(object_sp);
  EXPECT_FALSE(dwarf.MayContainCLinkageSymbols(name));
  EXPECT_FALSE(debug_map.MayContainCLinkageSymbols(name));
  EXPECT_FALSE(
      dwarf.MayContainCLinkageSymbols(name, SymbolNameLookupKind::Defined));
  EXPECT_FALSE(
      dwarf.MayContainCLinkageSymbols(name, SymbolNameLookupKind::Function));
  EXPECT_FALSE(
      debug_map.MayContainCLinkageSymbols(name, SymbolNameLookupKind::Defined));
  EXPECT_FALSE(debug_map.MayContainCLinkageSymbols(
      name, SymbolNameLookupKind::Function));
  EXPECT_FALSE(dwarf.MayContainCLinkageSymbols(llvm::StringRef("later")));
  EXPECT_FALSE(debug_map.MayContainCLinkageSymbols(llvm::StringRef("later")));
  EXPECT_EQ(object->GetSymtab(/*can_create=*/false), nullptr);

  auto symbols = std::make_unique<CLinkageAugmentingSymbolFile>(object_sp);
  // A provider may add names absent from the main object's raw metadata.
  EXPECT_TRUE(symbols->MayContainCLinkageSymbols(name));
  EXPECT_TRUE(
      symbols->MayContainCLinkageSymbols(name, SymbolNameLookupKind::Defined));
  EXPECT_TRUE(
      symbols->MayContainCLinkageSymbols(name, SymbolNameLookupKind::Function));
  module->symbols = std::move(symbols);
  ModuleFunctionSearchOptions options;
  options.include_symbols = true;
  options.match_c_linkage_names = true;
  SymbolContextList contexts;
  module->FindFunctions(ConstString(name), CompilerDeclContext(),
                        eFunctionNameTypeFull, options, contexts);
  ASSERT_EQ(contexts.GetSize(), 1u);
  SymbolContext context;
  ASSERT_TRUE(contexts.GetContextAtIndex(0, context));
  ASSERT_NE(context.symbol, nullptr);
  EXPECT_EQ(context.symbol->GetName(), ConstString(name));
  EXPECT_TRUE(context.symbol->IsSynthetic());

  // Existing tables can also acquire names from runtime or manual additions.
  Symtab *symtab = object->GetSymtab(/*can_create=*/false);
  ASSERT_NE(symtab, nullptr);
  symtab->AddSymbol(Symbol(2, "later", eSymbolTypeCode, false, false, false,
                           false, SectionSP(), 0x2000, 0, false, false, 0));
  EXPECT_TRUE(dwarf.MayContainCLinkageSymbols(llvm::StringRef("later")));
  EXPECT_TRUE(debug_map.MayContainCLinkageSymbols(llvm::StringRef("later")));
  EXPECT_TRUE(dwarf.MayContainCLinkageSymbols(llvm::StringRef("later"),
                                              SymbolNameLookupKind::Defined));
  EXPECT_TRUE(dwarf.MayContainCLinkageSymbols(llvm::StringRef("later"),
                                              SymbolNameLookupKind::Function));
  EXPECT_TRUE(debug_map.MayContainCLinkageSymbols(
      llvm::StringRef("later"), SymbolNameLookupKind::Defined));
  EXPECT_TRUE(debug_map.MayContainCLinkageSymbols(
      llvm::StringRef("later"), SymbolNameLookupKind::Function));
  EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
}

TEST_F(SymtabTest, DefinedLinkageLookupSkipsColdUndefinedOnlyObject) {
  auto image = MetadataMachO({{1, 0, 0, "_swift_isaMask"}});
  ASSERT_THAT_EXPECTED(image, llvm::Succeeded());
  auto module = std::make_shared<CLinkageSearchModule>(image->moduleSpec());
  ObjectFile *object = module->GetObjectFile();
  ASSERT_NE(object, nullptr);
  module->symbols =
      std::make_unique<SymbolFileDWARF>(object->shared_from_this(), nullptr);
  ModuleList modules;
  modules.Append(module);
  SymbolContextList matches;
  modules.FindSymbolsWithNameAndType(ConstString("swift_isaMask"),
                                     eSymbolTypeAny, matches, true, false);
  EXPECT_EQ(matches.GetSize(), 0u);
  EXPECT_EQ(object->GetSymtab(false), nullptr);
  EXPECT_EQ(module->GetSymtabParseTime().get().count(), 0.0);
  EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
  // Default lookup still returns the original lone undefined result. The
  // caller, including MemoryReader's fallback, keeps its existing handling.
  modules.FindSymbolsWithNameAndType(ConstString("swift_isaMask"),
                                     eSymbolTypeAny, matches, true);
  ASSERT_EQ(matches.GetSize(), 1u);
  EXPECT_EQ(matches[0].symbol->GetType(), eSymbolTypeUndefined);
}

TEST_F(SymtabTest, DefinedLinkageLookupFiltersOnlyNewUndefinedContexts) {
  auto image = TestFile::fromYaml(CLinkageNegativeMachO);
  ASSERT_THAT_EXPECTED(image, llvm::Succeeded());
  auto module = std::make_shared<CLinkageSearchModule>(image->moduleSpec());
  ObjectFile *object = module->GetObjectFile();
  ASSERT_NE(object, nullptr);
  module->symbols =
      std::make_unique<SymbolFileDWARF>(object->shared_from_this(), nullptr);
  Symtab *symtab = object->GetSymtab();
  ASSERT_NE(symtab, nullptr);
  for (SymbolType type :
       {eSymbolTypeUndefined, eSymbolTypeData, eSymbolTypeTrampoline})
    symtab->AddSymbol(Symbol(10 + unsigned(type), "runtime", type, true, false,
                             false, false, SectionSP(), 0, 0, false, false, 0));
  ModuleList modules;
  modules.Append(module);
  SymbolContextList defaults;
  modules.FindSymbolsWithNameAndType(ConstString("runtime"), eSymbolTypeAny,
                                     defaults, true);
  ASSERT_EQ(defaults.GetSize(), 3u);
  SymbolContextList matches;
  matches.Append(defaults[0]);
  modules.FindSymbolsWithNameAndType(ConstString("runtime"), eSymbolTypeAny,
                                     matches, true, false);
  ASSERT_EQ(matches.GetSize(), 3u);
  EXPECT_EQ(matches[0].symbol->GetType(), eSymbolTypeUndefined);
  EXPECT_EQ(matches[1].symbol->GetType(), eSymbolTypeData);
  EXPECT_EQ(matches[2].symbol->GetType(), eSymbolTypeTrampoline);
  SymbolContextList undefined;
  modules.FindSymbolsWithNameAndType(
      ConstString("runtime"), eSymbolTypeUndefined, undefined, true, false);
  EXPECT_EQ(undefined.GetSize(), 0u);
}

TEST_F(SymtabTest, CLinkageSymbolLookupPreservesTypesOrderAndAnnotations) {
  auto image = TestFile::fromYaml(R"(
--- !ELF
FileHeader:
  Class: ELFCLASS64
  Data: ELFDATA2LSB
  Type: ET_DYN
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [SHF_ALLOC, SHF_EXECINSTR]
    Address: 0x1000
    Content: C3
)");
  ASSERT_THAT_EXPECTED(image, llvm::Succeeded());
  auto module = std::make_shared<Module>(image->moduleSpec());
  Symtab *symtab = module->GetObjectFile()->GetSymtab();
  ASSERT_NE(symtab, nullptr);
  auto add = [&](user_id_t id, llvm::StringRef name, SymbolType type,
                 bool external, bool trampoline, bool annotations) {
    symtab->AddSymbol(Symbol(id, name, type, external, false, trampoline, false,
                             SectionSP(), id, 0, false, annotations, 0));
  };
  add(0, "dispatch_queue_offsets", eSymbolTypeData, false, false, false);
  add(1, "dispatch_queue_offsets@VERSION", eSymbolTypeData, true, false, true);
  add(2, "dispatch_queue_offsets", eSymbolTypeReExported, true, false, false);
  add(3, "dispatch_queue_offsets", eSymbolTypeUndefined, true, false, false);
  add(4, "dispatch_queue_offsets", eSymbolTypeTrampoline, true, true, false);
  add(5, "_Z22dispatch_queue_offsetsv", eSymbolTypeCode, true, false, false);
  add(6, "_Z18other_wire_requestv", eSymbolTypeCode, true, false, false);
  Symbol named_synthetic(7, "dispatch_queue_offsets", eSymbolTypeData, false,
                         false, false, false, SectionSP(), 7, 0, false, false,
                         0);
  named_synthetic.SetIsSynthetic(true);
  symtab->AddSymbol(named_synthetic);
  ConstString synthetic_name("_Z30qsymbol_synthetic_unrelated_06v");
  Symbol unrelated_synthetic(8, synthetic_name.GetStringRef(), eSymbolTypeCode,
                             false, false, false, false, SectionSP(), 8, 0,
                             false, false, 0);
  unrelated_synthetic.SetIsSynthetic(true);
  symtab->AddSymbol(unrelated_synthetic);
  ConstString counterpart;
  ASSERT_FALSE(synthetic_name.GetMangledCounterpart(counterpart));

  std::vector<uint32_t> matches;
  symtab->FindAllSymbolsWithNameAndType(ConstString("dispatch_queue_offsets"),
                                        eSymbolTypeAny, matches,
                                        /*match_c_linkage_names=*/true);
  ASSERT_EQ(matches.size(), 6u);
  const user_id_t all_ids[] = {0, 1, 2, 3, 4, 7};
  for (auto [index, id] : llvm::zip(matches, all_ids))
    EXPECT_EQ(symtab->SymbolAtIndex(index)->GetID(), id);
  // Checking autogenerated-name status on every named synthetic symbol would
  // demangle this unrelated name even though no index is built.
  EXPECT_FALSE(synthetic_name.GetMangledCounterpart(counterpart));

  matches.clear();
  symtab->FindAllSymbolsWithNameAndType(ConstString("dispatch_queue_offsets"),
                                        eSymbolTypeData, matches,
                                        /*match_c_linkage_names=*/true);
  ASSERT_EQ(matches.size(), 3u);
  const user_id_t data_ids[] = {0, 1, 7};
  for (auto [index, id] : llvm::zip(matches, data_ids))
    EXPECT_EQ(symtab->SymbolAtIndex(index)->GetID(), id);
  EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);

  matches.clear();
  symtab->FindAllSymbolsWithNameAndType(ConstString("dispatch_queue_offsets"),
                                        eSymbolTypeTrampoline, matches,
                                        /*match_c_linkage_names=*/true);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(symtab->SymbolAtIndex(matches.front())->GetID(), 4u);
  EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);

  matches.clear();
  symtab->FindAllSymbolsWithNameAndType(ConstString(), eSymbolTypeAny, matches,
                                        /*match_c_linkage_names=*/true);
  EXPECT_TRUE(matches.empty());
  EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);

  // Unrelated qSymbol requests still use the established name index, including
  // a C++ function's unqualified basename.
  symtab->FindAllSymbolsWithNameAndType(ConstString("other_wire_request"),
                                        eSymbolTypeAny, matches);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(symtab->SymbolAtIndex(matches.front())->GetID(), 6u);
  EXPECT_GT(module->GetSymtabIndexTime().get().count(), 0.0);
}

TEST_F(SymtabTest,
       CLinkageModuleListLookupLeavesNegativeModulesUnmaterialized) {
  auto negative_image = TestFile::fromYaml(CLinkageNegativeMachO);
  ASSERT_THAT_EXPECTED(negative_image, llvm::Succeeded());
  auto negative =
      std::make_shared<CLinkageSearchModule>(negative_image->moduleSpec());
  ObjectFile *negative_object = negative->GetObjectFile();
  ASSERT_NE(negative_object, nullptr);
  negative->symbols = std::make_unique<CLinkageDWARFSearch>(
      negative_object->shared_from_this());
  auto positive_image =
      MetadataMachO({{0x02, 0, 0x1000, "_dispatch_queue_offsets"},
                     {0x03, 0, 0x2000, "_dispatch_queue_offsets"}});
  ASSERT_THAT_EXPECTED(positive_image, llvm::Succeeded());
  auto positive =
      std::make_shared<CLinkageSearchModule>(positive_image->moduleSpec());
  ObjectFile *positive_object = positive->GetObjectFile();
  ASSERT_NE(positive_object, nullptr);
  positive->symbols = std::make_unique<CLinkageDWARFSearch>(
      positive_object->shared_from_this());
  ModuleList modules;
  modules.Append(negative);
  modules.Append(positive);
  SymbolContextList contexts;
  modules.FindSymbolsWithNameAndType(ConstString("dispatch_queue_offsets"),
                                     eSymbolTypeAny, contexts,
                                     /*match_c_linkage_names=*/true);
  ASSERT_EQ(contexts.GetSize(), 2u);
  for (uint32_t i = 0; i < contexts.GetSize(); ++i) {
    const SymbolContext &context = contexts[i];
    EXPECT_EQ(context.module_sp, positive);
    ASSERT_NE(context.symbol, nullptr);
    EXPECT_EQ(context.symbol->GetID(), i);
    EXPECT_EQ(context.symbol->GetName(), ConstString("dispatch_queue_offsets"));
  }
  EXPECT_EQ(negative_object->GetSymtab(/*can_create=*/false), nullptr);
  EXPECT_EQ(negative->GetSymtabParseTime().get().count(), 0.0);
  EXPECT_EQ(negative->GetSymtabIndexTime().get().count(), 0.0);
  EXPECT_EQ(positive->GetSymtabIndexTime().get().count(), 0.0);

  contexts.Clear();
  modules.FindSymbolsWithNameAndType(ConstString("absent"), eSymbolTypeAny,
                                     contexts, /*match_c_linkage_names=*/true);
  EXPECT_EQ(contexts.GetSize(), 0u);
  EXPECT_EQ(negative_object->GetSymtab(/*can_create=*/false), nullptr);
  EXPECT_EQ(positive->GetSymtabIndexTime().get().count(), 0.0);
}

TEST_F(SymtabTest, CLinkageSymbolQueryPreservesAugmentingProviders) {
  auto image = TestFile::fromYaml(CLinkageNegativeMachO);
  ASSERT_THAT_EXPECTED(image, llvm::Succeeded());
  auto module = std::make_shared<CLinkageSearchModule>(image->moduleSpec());
  ObjectFile *object = module->GetObjectFile();
  ASSERT_NE(object, nullptr);
  module->symbols = std::make_unique<CLinkageAugmentingSymbolFile>(
      object->shared_from_this());
  ModuleList modules;
  modules.Append(module);
  SymbolContextList contexts;
  modules.FindSymbolsWithNameAndType(ConstString("runtime"), eSymbolTypeCode,
                                     contexts, /*match_c_linkage_names=*/true);
  ASSERT_EQ(contexts.GetSize(), 1u);
  ASSERT_NE(contexts[0].symbol, nullptr);
  EXPECT_EQ(contexts[0].symbol->GetName(), ConstString("runtime"));
  EXPECT_TRUE(contexts[0].symbol->IsSynthetic());
  EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
}

TEST_F(SymtabTest, CLinkageTrampolineDoesNotHideDylinkerDefinition) {
  auto ordinary_image = TestFile::fromYaml(CLinkageNegativeMachO);
  ASSERT_THAT_EXPECTED(ordinary_image, llvm::Succeeded());
  auto ordinary =
      std::make_shared<CLinkageSearchModule>(ordinary_image->moduleSpec());
  auto dylinker_image = TestFile::fromYaml(R"(
--- !mach-o
FileHeader:
  magic: 0xFEEDFACF
  cputype: 0x100000C
  cpusubtype: 0
  filetype: 7
  ncmds: 0
  sizeofcmds: 0
  flags: 0
  reserved: 0
)");
  ASSERT_THAT_EXPECTED(dylinker_image, llvm::Succeeded());
  auto dylinker =
      std::make_shared<CLinkageSearchModule>(dylinker_image->moduleSpec());
  ObjectFile *ordinary_object = ordinary->GetObjectFile();
  ObjectFile *dylinker_object = dylinker->GetObjectFile();
  ASSERT_NE(ordinary_object, nullptr);
  ASSERT_NE(dylinker_object, nullptr);
  ordinary->symbols = std::make_unique<CLinkageDWARFSearch>(
      ordinary_object->shared_from_this());
  dylinker->symbols = std::make_unique<CLinkageDWARFSearch>(
      dylinker_object->shared_from_this());
  ASSERT_FALSE(ordinary->GetIsDynamicLinkEditor());
  ASSERT_TRUE(dylinker->GetIsDynamicLinkEditor());

  auto data_section = std::make_shared<Section>(
      dylinker, dylinker_object, 1, ConstString("__data"), eSectionTypeData,
      0x2000, 8, 0, 0, 0, 0);
  Symtab *ordinary_symtab = ordinary_object->GetSymtab();
  ASSERT_NE(ordinary_symtab, nullptr);
  const uint32_t trampoline_index = ordinary_symtab->AddSymbol(
      Symbol(10, "dispatch_queue_offsets", eSymbolTypeTrampoline, true, false,
             true, false, SectionSP(), 0x1000, 0, false, false, 0));
  Symtab *dylinker_symtab = dylinker_object->GetSymtab();
  ASSERT_NE(dylinker_symtab, nullptr);
  dylinker_symtab->AddSymbol(Symbol(20, "dispatch_queue_offsets",
                                    eSymbolTypeData, true, false, false, false,
                                    data_section, 0, 8, true, false, 0));

  ModuleList modules;
  // Dylinkers are searched after ordinary modules even when inserted first.
  modules.Append(dylinker);
  modules.Append(ordinary);
  SymbolContextList contexts;
  modules.FindSymbolsWithNameAndType(ConstString("dispatch_queue_offsets"),
                                     eSymbolTypeAny, contexts,
                                     /*match_c_linkage_names=*/true);
  ASSERT_EQ(contexts.GetSize(), 2u);
  EXPECT_EQ(contexts[0].module_sp, ordinary);
  EXPECT_EQ(contexts[0].symbol->GetType(), eSymbolTypeTrampoline);
  EXPECT_EQ(contexts[1].module_sp, dylinker);
  EXPECT_EQ(contexts[1].symbol->GetType(), eSymbolTypeData);
  EXPECT_TRUE(contexts[1].symbol->ValueIsAddress());
  EXPECT_EQ(contexts[1].symbol->GetAddressRef().GetFileAddress(), 0x2000u);
  EXPECT_EQ(ordinary->GetSymtabIndexTime().get().count(), 0.0);
  EXPECT_EQ(dylinker->GetSymtabIndexTime().get().count(), 0.0);

  contexts.Clear();
  modules.FindSymbolsWithNameAndType(ConstString("dispatch_queue_offsets"),
                                     eSymbolTypeTrampoline, contexts,
                                     /*match_c_linkage_names=*/true);
  ASSERT_EQ(contexts.GetSize(), 1u);
  EXPECT_EQ(contexts[0].module_sp, ordinary);
  EXPECT_EQ(contexts[0].symbol->GetID(), 10u);

  // Default matching still excludes the trampoline through its name index.
  contexts.Clear();
  modules.FindSymbolsWithNameAndType(ConstString("dispatch_queue_offsets"),
                                     eSymbolTypeAny, contexts);
  ASSERT_EQ(contexts.GetSize(), 1u);
  EXPECT_EQ(contexts[0].module_sp, dylinker);

  // An ordinary data symbol still suppresses the dylinker in the raw mode.
  ordinary_symtab->SymbolAtIndex(trampoline_index)->SetType(eSymbolTypeData);
  contexts.Clear();
  modules.FindSymbolsWithNameAndType(ConstString("dispatch_queue_offsets"),
                                     eSymbolTypeAny, contexts,
                                     /*match_c_linkage_names=*/true);
  ASSERT_EQ(contexts.GetSize(), 1u);
  EXPECT_EQ(contexts[0].module_sp, ordinary);
}

TEST_F(SymtabTest, LocalCacheMetadataMatchesOrdinaryFileWithoutMaterializing) {
  const MetadataNList records[] = {{0xf, 0, 0x1800, "_code", 1},
                                   {0xf, 0, 0x1900, "_data", 2},
                                   {1, 0, 0, "_stubbed"}};
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (bool cache : {false, true}) {
        auto module = CodeDataReaderModule(records, cache, address_size, order,
                                           SingleExport("_export_only"), true);
        ObjectFile *object = module->GetObjectFile();
        ASSERT_NE(object, nullptr);
        const auto *metadata = object->GetDebugInfoMetadata();
        ASSERT_NE(metadata, nullptr);
        ASSERT_TRUE(metadata->debug_map_objects);
        EXPECT_TRUE(metadata->debug_map_objects->empty());
        EXPECT_TRUE(metadata->swift_ast_files.empty());
        EXPECT_TRUE(metadata->has_no_stabs_and_code_and_data);
        for (llvm::StringRef name : {"code", "data", "export_only", "stubbed"})
          EXPECT_TRUE(object->MayContainCLinkageSymbols(
              {name}, SymbolNameLookupKind::Defined));
        EXPECT_FALSE(object->MayContainCLinkageSymbols({"absent"}));
        EXPECT_EQ(object->GetSymtab(false), nullptr);
        SymbolFileDWARFDebugMap debug_map(object->shared_from_this());
        EXPECT_EQ(debug_map.GetAbilities(), 0u);
        EXPECT_EQ(object->GetSymtab(false), nullptr);
        SymbolFileSymtab legacy(object->shared_from_this());
        EXPECT_EQ(legacy.GetAbilities(),
                  SymbolFile::Functions | SymbolFile::GlobalVariables);
        auto *symtab = object->GetSymtab(false);
        ASSERT_NE(symtab, nullptr);
        std::vector<uint32_t> indices;
        symtab->FindAllSymbolsWithNameAndType(
            ConstString("stubbed"), eSymbolTypeTrampoline, indices, true);
        EXPECT_EQ(indices.size(), 1u);
        EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
      }
    }
  }
}

TEST_F(SymtabTest, FactorySymtabAbilitiesAndCompileUnitCountStayLazy) {
  const MetadataNList records[] = {{0xf, 0, 0x1800, "_code", 1},
                                   {0xf, 0, 0x1900, "_data", 2}};
  for (bool cache : {false, true}) {
    auto module = CodeDataReaderModule(records, cache);
    auto *object = module->GetObjectFile();
    ASSERT_NE(object, nullptr);
    std::unique_ptr<SymbolFile> provider(
        SymbolFileSymtab::CreateInstance(object->shared_from_this()));
    EXPECT_EQ(provider->GetAbilities(),
              SymbolFile::Functions | SymbolFile::GlobalVariables);
    EXPECT_EQ(provider->GetAbilities(),
              SymbolFile::Functions | SymbolFile::GlobalVariables);
    EXPECT_EQ(provider->GetNumCompileUnits(), 0u);
    EXPECT_EQ(provider->GetCompileUnitAtIndex(0), nullptr);
    EXPECT_FALSE(provider->MayContainCLinkageSymbols({"absent"}));
    EXPECT_FALSE(provider->MayContainCLinkageSymbols(
        {"absent"}, SymbolNameLookupKind::Function));
    EXPECT_EQ(object->GetSymtab(false), nullptr);
    EXPECT_EQ(module->GetSymtabParseTime().get().count(), 0.0);
    CompileUnit unit(module, nullptr, "manual.c", 0, eLanguageTypeC,
                     eLazyBoolNo);
    EXPECT_EQ(provider->ParseFunctions(unit), 1u);
    EXPECT_EQ(unit.GetNumFunctions(), 1u);
    EXPECT_NE(object->GetSymtab(false), nullptr);
    EXPECT_EQ(provider->ParseFunctions(unit), 1u);
    EXPECT_EQ(unit.GetNumFunctions(), 1u);
    EXPECT_EQ(provider->GetNumCompileUnits(), 0u);
  }
}

TEST_F(SymtabTest, UnprovedSymtabAbilitiesRetainFullCalculation) {
  const std::vector<MetadataNList> cases[] = {
      {},
      {{0xf, 0, 0x1800, "_code", 1}},
      {{0xf, 0, 0x1900, "_data", 2}},
      {{0xf, 0, 0x1800, "_code", 1},
       {0xf, 0, 0x1900, "_data", 2},
       {0x64, 0, 0, "source.c"}},
      {{0xf, 0, 0x1800, "_code", 9}, {0xf, 0, 0x1900, "_data", 2}}};
  for (const auto &records : cases) {
    auto module = CodeDataReaderModule(records);
    auto *object = module->GetObjectFile();
    ASSERT_NE(object, nullptr);
    const auto *metadata = object->GetDebugInfoMetadata();
    ASSERT_NE(metadata, nullptr);
    EXPECT_FALSE(metadata->has_no_stabs_and_code_and_data);
    std::unique_ptr<SymbolFile> factory(
        SymbolFileSymtab::CreateInstance(object->shared_from_this()));
    const auto abilities = factory->GetAbilities();
    EXPECT_NE(object->GetSymtab(false), nullptr);
    auto other = CodeDataReaderModule(records);
    SymbolFileSymtab legacy(other->GetObjectFile()->shared_from_this());
    EXPECT_EQ(abilities, legacy.GetAbilities());
  }
}

TEST_F(SymtabTest, ResolverCodeCannotProveSymtabFunctionAbility) {
  auto trie = SingleExport("_resolver");
  trie.resize(trie.size() - 4);
  // The resolver is the only Code symbol's address, relative to __TEXT.
  trie.insert(trie.end(), {5, 16, 0x80, 0x10, 0x80, 0x10, 0});
  const MetadataNList records[] = {{0xf, 0, 0x1800, "_code", 1},
                                   {0xf, 0, 0x1900, "_data", 2}};
  for (bool cache : {false, true}) {
    auto module =
        CodeDataReaderModule(records, cache, 8, eByteOrderLittle, trie);
    auto *object = module->GetObjectFile();
    const auto *metadata = object->GetDebugInfoMetadata();
    ASSERT_NE(metadata, nullptr);
    EXPECT_FALSE(metadata->has_no_stabs_and_code_and_data);
    std::unique_ptr<SymbolFile> factory(
        SymbolFileSymtab::CreateInstance(object->shared_from_this()));
    EXPECT_EQ(factory->GetAbilities(), SymbolFile::GlobalVariables);
    EXPECT_NE(object->GetSymtab(false), nullptr);
  }
}

TEST_F(SymtabTest, VanillaFactoryPreservesLateMaterializedSymbolsAndIndexes) {
  auto module = CodeDataReaderModule(
      {{0xf, 0, 0x1800, "_code", 1}, {0xf, 0, 0x1900, "_data", 2}}, true);
  auto *object = module->GetObjectFile();
  std::unique_ptr<SymbolFile> provider(
      SymbolFileSymtab::CreateInstance(object->shared_from_this()));
  EXPECT_EQ(provider->GetAbilities(),
            SymbolFile::Functions | SymbolFile::GlobalVariables);
  EXPECT_FALSE(provider->MayContainCLinkageSymbols({"later"}));
  EXPECT_FALSE(provider->MayContainCLinkageSymbols(
      {"later"}, SymbolNameLookupKind::Function));
  auto *symtab = object->GetSymtab();
  ASSERT_NE(symtab, nullptr);
  symtab->AddSymbol(Symbol(1000, "later", eSymbolTypeData, true, false, false,
                           false, SectionSP(), 0, 0, false, false, 0));
  EXPECT_TRUE(provider->MayContainCLinkageSymbols(
      {"later"}, SymbolNameLookupKind::Defined));
  EXPECT_TRUE(provider->MayContainCLinkageSymbols(
      {"later"}, SymbolNameLookupKind::Function));
  EXPECT_TRUE(provider->MayContainCLinkageSymbols({"absent"}));
  CompileUnit unit(module, nullptr, "manual.c", 0, eLanguageTypeC, eLazyBoolNo);
  EXPECT_EQ(provider->ParseFunctions(unit), 1u);
  EXPECT_EQ(unit.GetNumFunctions(), 1u);
}

TEST_F(SymtabTest, PublicAndAugmentingSymtabInstancesKeepLegacyContract) {
  auto module = CodeDataReaderModule(
      {{0xf, 0, 0x1800, "_code", 1}, {0xf, 0, 0x1900, "_data", 2}});
  auto *object = module->GetObjectFile();
  SymbolFileSymtab legacy(object->shared_from_this());
  EXPECT_TRUE(legacy.MayContainCLinkageSymbols({"absent"}));
  EXPECT_TRUE(legacy.MayContainCLinkageSymbols({"absent"},
                                               SymbolNameLookupKind::Function));
  EXPECT_EQ(legacy.GetAbilities(),
            SymbolFile::Functions | SymbolFile::GlobalVariables);
  EXPECT_NE(object->GetSymtab(false), nullptr);
  auto other = CodeDataReaderModule(
      {{0xf, 0, 0x1800, "_code", 1}, {0xf, 0, 0x1900, "_data", 2}});
  CLinkageAugmentingSymbolFile augment(
      other->GetObjectFile()->shared_from_this());
  EXPECT_TRUE(augment.MayContainCLinkageSymbols({"runtime"}));
  EXPECT_TRUE(augment.MayContainCLinkageSymbols({"runtime"},
                                                SymbolNameLookupKind::Defined));
  EXPECT_TRUE(augment.MayContainCLinkageSymbols(
      {"runtime"}, SymbolNameLookupKind::Function));
  EXPECT_EQ(other->GetObjectFile()->GetSymtab(false), nullptr);
  auto *symtab = augment.GetSymtab();
  std::vector<uint32_t> indices;
  symtab->FindAllSymbolsWithNameAndType(ConstString("runtime"), eSymbolTypeCode,
                                        indices, true);
  EXPECT_EQ(indices.size(), 1u);
}

TEST_F(SymtabTest, LocalCacheMetadataRejectsPartialViewsAndEnabledIndexCache) {
  const MetadataNList records[] = {{0xf, 0, 0x1800, "_code", 1},
                                   {0xf, 0, 0x1900, "_data", 2}};
  auto truncated =
      CodeDataReaderModule(records, true, 8, eByteOrderLittle, {}, false, true);
  auto *partial = truncated->GetObjectFile();
  ASSERT_NE(partial, nullptr);
  EXPECT_EQ(partial->GetDebugInfoMetadata(), nullptr);
  EXPECT_TRUE(partial->MayContainCLinkageSymbols({"absent"}));
  EXPECT_EQ(partial->GetSymtab(false), nullptr);
  DataExtractorSP unavailable;
  EXPECT_EQ(partial->GetData(0x4000, 0x1000, unavailable), 0u);
  EXPECT_EQ(unavailable, nullptr);
  Section missing_section(truncated, partial, 1000, ConstString("partial"),
                          eSectionTypeData, 0x5000, 0x1000, 0x4000, 0x1000, 0,
                          0);
  DataExtractor section_data(std::make_shared<DataBufferHeap>(8, 0),
                             eByteOrderLittle, 8);
  EXPECT_EQ(partial->ReadSectionData(&missing_section, section_data), 0u);
  EXPECT_EQ(section_data.GetByteSize(), 0u);
  std::unique_ptr<SymbolFile> partial_provider(
      SymbolFileSymtab::CreateInstance(partial->shared_from_this()));
  EXPECT_EQ(partial_provider->GetAbilities(), 0u);
  ASSERT_NE(partial->GetSymtab(false), nullptr);
  EXPECT_EQ(partial->GetSymtab(false)->GetNumSymbols(), 0u);

  // Losing an optional table must not lose the readable Code/Data symbols.
  const MetadataNList stub_records[] = {{0xf, 0, 0x1800, "_code", 1},
                                        {0xf, 0, 0x1900, "_data", 2},
                                        {1, 0, 0, "_stubbed"}};
  for (bool indirect : {false, true}) {
    auto optional = CodeDataReaderModule(
        indirect ? llvm::ArrayRef<MetadataNList>(stub_records)
                 : llvm::ArrayRef<MetadataNList>(records),
        true, 8, eByteOrderLittle,
        indirect ? std::vector<uint8_t>() : SingleExport("_export_only"),
        indirect, true);
    auto *file = optional->GetObjectFile();
    ASSERT_NE(file->GetSectionList(), nullptr);
    auto *symbols = file->GetSymtab();
    ASSERT_NE(symbols, nullptr);
    for (SymbolType type : {eSymbolTypeCode, eSymbolTypeData}) {
      std::vector<uint32_t> indices;
      symbols->AppendSymbolIndexesWithType(type, indices);
      EXPECT_EQ(indices.size(), 1u);
    }
  }
  const uint8_t wrong_uuid[] = {9, 2,  3,  4,  5,  6,  7,  8,
                                9, 10, 11, 12, 13, 14, 15, 16};
  auto mismatch =
      CodeDataReaderModule(records, true, 8, eByteOrderLittle, {}, false, false,
                           UUID(wrong_uuid, sizeof(wrong_uuid)));
  // Module rejects a mismatched requested UUID before creating the object.
  EXPECT_EQ(mismatch->GetObjectFile(), nullptr);

  auto module = CodeDataReaderModule(records, true);
  auto *object = module->GetObjectFile();
  auto memory = std::make_shared<ObjectFileMachO>(
      module, std::make_shared<DataBufferHeap>(64, 0), ProcessSP(), 0x1000);
  EXPECT_EQ(memory->GetDebugInfoMetadata(), nullptr);
  EXPECT_TRUE(memory->MayContainCLinkageSymbols({"absent"}));
  EXPECT_TRUE(memory->MayContainCLinkageSymbols(
      {"absent"}, SymbolNameLookupKind::Function));
  EXPECT_EQ(memory->GetSymtab(false), nullptr);
  auto &properties = ModuleList::GetGlobalModuleListProperties();
  const bool old = properties.GetEnableLLDBIndexCache();
  auto restore =
      llvm::make_scope_exit([&] { properties.SetEnableLLDBIndexCache(old); });
  ASSERT_TRUE(properties.SetEnableLLDBIndexCache(true));
  EXPECT_EQ(object->GetDebugInfoMetadata(), nullptr);
  EXPECT_TRUE(object->MayContainCLinkageSymbols({"absent"}));
  EXPECT_TRUE(object->MayContainCLinkageSymbols(
      {"absent"}, SymbolNameLookupKind::Function));
  EXPECT_EQ(object->GetSymtab(false), nullptr);
  ASSERT_TRUE(properties.SetEnableLLDBIndexCache(false));
  EXPECT_FALSE(object->MayContainCLinkageSymbols({"absent"}));
  EXPECT_FALSE(object->MayContainCLinkageSymbols(
      {"absent"}, SymbolNameLookupKind::Function));
  EXPECT_EQ(object->GetSymtab(false), nullptr);
}

TEST_F(SymtabTest, MachOFunctionLinkageKeepsStubModesIndependent) {
  const SymbolNameLookupKind kinds[] = {SymbolNameLookupKind::Any,
                                        SymbolNameLookupKind::Defined,
                                        SymbolNameLookupKind::Function};
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (llvm::StringRef raw_name : {"_runtime", "_$s7Fixture7runtimeyyF"}) {
        const llvm::StringRef name = raw_name.drop_front();
        for (bool resolver : {false, true}) {
          for (unsigned first = 0; first != std::size(kinds); ++first) {
            auto module = IndirectStubReaderModule(
                address_size, order, resolver, 0, false, raw_name);
            ObjectFile *file = module->GetObjectFile();
            ASSERT_NE(file, nullptr);
            for (unsigned repeat = 0; repeat != 2; ++repeat) {
              for (unsigned step = 0; step != std::size(kinds); ++step) {
                const auto kind = kinds[(first + step) % std::size(kinds)];
                EXPECT_EQ(file->MayContainCLinkageSymbols({name}, kind),
                          kind != SymbolNameLookupKind::Function || resolver);
                EXPECT_EQ(
                    file->MayContainCLinkageSymbols({"reference_only"}, kind),
                    kind == SymbolNameLookupKind::Any);
                EXPECT_FALSE(file->MayContainCLinkageSymbols({"absent"}, kind));
              }
            }
            EXPECT_EQ(file->GetSymtab(false), nullptr);
            ASSERT_NE(file->GetSectionList(), nullptr);
            Symtab *table = file->GetSymtab();
            ASSERT_NE(table, nullptr);
            std::vector<uint32_t> indices;
            table->FindAllSymbolsWithNameAndType(ConstString(name),
                                                 eSymbolTypeAny, indices, true);
            ASSERT_EQ(indices.size(), 1u);
            EXPECT_EQ(table->SymbolAtIndex(indices.front())->GetID(), 0u);
            EXPECT_EQ(table->SymbolAtIndex(indices.front())->GetType(),
                      resolver ? eSymbolTypeResolver : eSymbolTypeTrampoline);
            SymbolContextList functions;
            table->FindFunctionSymbols(ConstString(name), eFunctionNameTypeFull,
                                       functions, true);
            EXPECT_EQ(functions.GetSize(), resolver ? 1u : 0u);
            EXPECT_TRUE(file->MayContainCLinkageSymbols(
                {name}, SymbolNameLookupKind::Function));
            EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
          }
        }
      }
    }
  }
}

TEST_F(SymtabTest, MachOFunctionLinkageRetainsDefinitionsAndExports) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      const MetadataNList records[] = {{0xf, 0, 0x1800, "_code", 1},
                                       {0xf, 0, 0x1900, "_data", 2},
                                       {3, 0, 0x2000, "_absolute"},
                                       {0xb, 0, 1, "_alias"}};
      auto module = CodeDataReaderModule(records, false, address_size, order,
                                         SingleExport("_exported", true));
      ObjectFile *file = module->GetObjectFile();
      ASSERT_NE(file, nullptr);
      for (auto kind :
           {SymbolNameLookupKind::Function, SymbolNameLookupKind::Any,
            SymbolNameLookupKind::Defined}) {
        for (llvm::StringRef name : {"code", "absolute", "alias", "exported"})
          EXPECT_TRUE(file->MayContainCLinkageSymbols({name}, kind));
        EXPECT_FALSE(file->MayContainCLinkageSymbols({"absent"}, kind));
      }
      // The absence proof may conservatively retain a nonfunction definition.
      EXPECT_TRUE(file->MayContainCLinkageSymbols(
          {"data"}, SymbolNameLookupKind::Function));
      EXPECT_EQ(file->GetSymtab(false), nullptr);
      ASSERT_NE(file->GetSectionList(), nullptr);
      Symtab *table = file->GetSymtab();
      ASSERT_NE(table, nullptr);
      for (llvm::StringRef name : {"code", "absolute", "alias", "exported"}) {
        SymbolContextList functions;
        table->FindFunctionSymbols(ConstString(name), eFunctionNameTypeFull,
                                   functions, true);
        ASSERT_EQ(functions.GetSize(), 1u) << name.str();
        EXPECT_NE(functions[0].symbol->GetType(), eSymbolTypeTrampoline);
      }
      SymbolContextList data;
      table->FindFunctionSymbols(ConstString("data"), eFunctionNameTypeFull,
                                 data, true);
      EXPECT_EQ(data.GetSize(), 0u);
      EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
    }
  }
}

TEST_F(SymtabTest, MachOFunctionLinkageKeepsUnknownsConservative) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (bool truncated : {false, true}) {
        auto module =
            IndirectStubReaderModule(address_size, order, false, 2, truncated);
        ObjectFile *file = module->GetObjectFile();
        ASSERT_NE(file, nullptr);
        for (auto kind :
             {SymbolNameLookupKind::Function, SymbolNameLookupKind::Any,
              SymbolNameLookupKind::Defined}) {
          EXPECT_TRUE(file->MayContainCLinkageSymbols({"runtime"}, kind));
          EXPECT_TRUE(
              file->MayContainCLinkageSymbols({"reference_only"}, kind));
        }
        EXPECT_EQ(file->GetSymtab(false), nullptr);
      }
      auto bad_trie = MetadataReaderModule({{1, 0, 0, "_runtime"}},
                                           address_size, order, {0, 1, 'x'});
      ObjectFile *file = bad_trie->GetObjectFile();
      ASSERT_NE(file, nullptr);
      for (auto kind :
           {SymbolNameLookupKind::Function, SymbolNameLookupKind::Defined,
            SymbolNameLookupKind::Any}) {
        EXPECT_TRUE(file->MayContainCLinkageSymbols({"runtime"}, kind));
        EXPECT_TRUE(
            file->MayContainCLinkageSymbols({"$s7Fixture7missingyyF"}, kind));
      }
      EXPECT_EQ(file->GetSymtab(false), nullptr);
      auto debug = MetadataReaderModule({{0x24, 0, 0x1000, "_debug"}},
                                        address_size, order);
      EXPECT_TRUE(debug->GetObjectFile()->MayContainCLinkageSymbols(
          {"debug"}, SymbolNameLookupKind::Function));
      EXPECT_TRUE(debug->GetObjectFile()->MayContainCLinkageSymbols(
          {"$Sunsupported"}, SymbolNameLookupKind::Function));
    }
  }
}

TEST_F(SymtabTest, FunctionLinkageSearchSkipsColdStubTable) {
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      auto fixture = IndirectStubReaderModule(address_size, order);
      ObjectFile *source = fixture->GetObjectFile();
      ASSERT_NE(source, nullptr);
      DataExtractorSP bytes;
      ASSERT_GT(source->GetData(0, source->GetByteSize(), bytes), 0u);
      auto module = std::make_shared<CLinkageSearchModule>(
          ModuleSpec(FileSpec(), UUID(), bytes));
      ObjectFile *file = module->GetObjectFile();
      ASSERT_NE(file, nullptr);
      ASSERT_NE(file->GetSectionList(), nullptr);
      auto provider =
          std::make_unique<CLinkageDWARFSearch>(file->shared_from_this());
      CLinkageDWARFSearch *dwarf = provider.get();
      module->symbols = std::move(provider);
      ModuleFunctionSearchOptions options;
      options.include_symbols = true;
      options.include_inlines = true;
      options.match_c_linkage_names = true;
      SymbolContextList contexts;
      module->FindFunctions(ConstString("runtime"), CompilerDeclContext(),
                            eFunctionNameTypeFull, options, contexts);
      EXPECT_TRUE(dwarf->searched);
      EXPECT_TRUE(dwarf->searched_inlines);
      EXPECT_EQ(dwarf->last_lookup_kind, SymbolNameLookupKind::Function);
      EXPECT_EQ(contexts.GetSize(), 1u);
      EXPECT_EQ(file->GetSymtab(false), nullptr);

      // General raw queries retain typed trampolines and their original IDs.
      contexts.Clear();
      module->FindSymbolsWithNameAndType(ConstString("runtime"),
                                         eSymbolTypeTrampoline, contexts, true);
      EXPECT_EQ(dwarf->last_lookup_kind, SymbolNameLookupKind::Any);
      ASSERT_EQ(contexts.GetSize(), 1u);
      EXPECT_EQ(contexts[0].symbol->GetID(), 0u);
      contexts.Clear();
      module->FindSymbolsWithNameAndType(ConstString("runtime"), eSymbolTypeAny,
                                         contexts, true, false);
      EXPECT_EQ(dwarf->last_lookup_kind, SymbolNameLookupKind::Defined);
      ASSERT_EQ(contexts.GetSize(), 1u);
      EXPECT_EQ(contexts[0].symbol->GetType(), eSymbolTypeTrampoline);
      EXPECT_EQ(contexts[0].symbol->GetID(), 0u);
      EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
    }
  }
}

TEST_F(SymtabTest, MachODirectedExportsPreserveExactNamesAndAliases) {
  const std::string query = "$s7Fixture7runtimeyyF";
  for (uint8_t address_size : {4, 8}) {
    for (ByteOrder order : {eByteOrderLittle, eByteOrderBig}) {
      for (llvm::StringRef prefix :
           {"", "OBJC_CLASS_$_", "OBJC_METACLASS_$_", "OBJC_IVAR_$_"}) {
        for (bool reexport : {false, true}) {
          // The ABI byte need not be '_', and may follow an empty edge.
          const std::string raw = "x" + prefix.str() + query;
          const std::vector<uint8_t> terminal =
              reexport ? std::vector<uint8_t>{8, 1, '_', 'o', 0}
                       : std::vector<uint8_t>{0, 0};
          auto trie = LinkageExportTrie({{{}, {{"", 1}, {"_unrelated", 4}}},
                                         {{}, {{raw.substr(0, 2), 2}}},
                                         {{}, {{raw.substr(2), 3}}},
                                         {terminal, {}},
                                         {{0, 0}, {}}});
          const MetadataNList records[] = {{0xf, 0, 0x1800, "_code", 1},
                                           {0xf, 0, 0x1900, "_data", 2}};
          auto module =
              CodeDataReaderModule(records, false, address_size, order, trie);
          ObjectFile *file = module->GetObjectFile();
          ASSERT_NE(file, nullptr);
          // A C query performs the complete validation before the directed
          // modern queries, without retaining any of their Swift spellings.
          EXPECT_FALSE(file->MayContainCLinkageSymbols({"absent"}));
          for (auto kind :
               {SymbolNameLookupKind::Function, SymbolNameLookupKind::Any,
                SymbolNameLookupKind::Defined}) {
            EXPECT_TRUE(file->MayContainCLinkageSymbols({query}, kind));
            EXPECT_TRUE(
                file->MayContainCLinkageSymbols({"absent", query}, kind));
            EXPECT_FALSE(
                file->MayContainCLinkageSymbols({query + "Suffix"}, kind));
            EXPECT_FALSE(file->MayContainCLinkageSymbols(
                {"$s7Fixture", "absent"}, kind));
          }
          EXPECT_EQ(file->GetSymtab(false), nullptr);
          ASSERT_NE(file->GetSectionList(), nullptr);
          Symtab *table = file->GetSymtab();
          ASSERT_NE(table, nullptr);
          std::vector<uint32_t> indices;
          // ObjC aliases are conservative; the literal ABI spelling remains
          // the full parser oracle in this ordinary non-ObjC section fixture.
          table->FindAllSymbolsWithNameAndType(ConstString(raw.substr(1)),
                                               eSymbolTypeAny, indices, true);
          EXPECT_FALSE(indices.empty());
          EXPECT_EQ(module->GetSymtabIndexTime().get().count(), 0.0);
        }
      }
    }
  }
  // A mixed query also needs aliases for ordinary identifiers rather than
  // consulting only the already-built C map or only modern Swift names.
  for (llvm::StringRef prefix :
       {"OBJC_CLASS_$_", "OBJC_METACLASS_$_", "OBJC_IVAR_$_"}) {
    auto module =
        MetadataReaderModule({}, 8, eByteOrderLittle,
                             SingleExport("x" + prefix.str() + "VisibleClass"));
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    EXPECT_FALSE(file->MayContainCLinkageSymbols({"absent"}));
    EXPECT_TRUE(file->MayContainCLinkageSymbols(
        {"$s7Fixture7missingyyF", "VisibleClass"}));
    EXPECT_FALSE(file->MayContainCLinkageSymbols(
        {"$s7Fixture7missingyyF", "VisibleClassSuffix"}));
  }
}

TEST_F(SymtabTest, MachODirectedExportsFollowAllCompatibleEdges) {
  const std::string query = "$s7Fixture7runtimeyyF";
  const std::string missing = "$s7Fixture7missingyyF";
  // The first duplicate edge is not an exact terminal match. A first-child
  // radix search would miss the empty-edge terminal in the second branch.
  auto trie = LinkageExportTrie(
      {{{},
        {{"_" + query, 1}, {"_" + query, 2}, {"_$s7", 5}, {"_" + missing, 0}}},
       {{}, {{"Suffix", 3}}},
       {{}, {{"", 4}}},
       {{0, 0}, {}},
       {{0, 0}, {}},
       {{}, {{query.substr(3), 6}}},
       {{0, 0}, {}}});
  for (bool c_first : {false, true}) {
    auto module = MetadataReaderModule({}, 8, eByteOrderLittle, trie);
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    if (c_first)
      EXPECT_FALSE(file->MayContainCLinkageSymbols({"absent"}));
    for (unsigned repeat = 0; repeat != 2; ++repeat) {
      for (auto kind :
           {SymbolNameLookupKind::Any, SymbolNameLookupKind::Defined,
            SymbolNameLookupKind::Function}) {
        EXPECT_TRUE(file->MayContainCLinkageSymbols({query}, kind));
        EXPECT_TRUE(file->MayContainCLinkageSymbols({query + "Suffix"}, kind));
        EXPECT_FALSE(file->MayContainCLinkageSymbols({missing}, kind));
        EXPECT_FALSE(
            file->MayContainCLinkageSymbols({"$s7Fixture7runtime"}, kind));
      }
    }
    EXPECT_EQ(file->GetSymtab(false), nullptr);
  }
}

TEST_F(SymtabTest, MachODirectedExportsKeepGlobalResolversAndUnknowns) {
  const std::string query = "$s7Fixture7runtimeyyF";
  for (bool resolver : {false, true}) {
    auto module = IndirectStubReaderModule(8, eByteOrderLittle, resolver, 0,
                                           false, "_" + query);
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    EXPECT_FALSE(file->MayContainCLinkageSymbols({"absent"}));
    // The resolver is on an unrelated path that a directed query skips.
    EXPECT_EQ(file->MayContainCLinkageSymbols({query},
                                              SymbolNameLookupKind::Function),
              resolver);
    EXPECT_TRUE(file->MayContainCLinkageSymbols({query},
                                                SymbolNameLookupKind::Defined));
    EXPECT_TRUE(file->MayContainCLinkageSymbols({query}));
    EXPECT_EQ(file->GetSymtab(false), nullptr);
    ASSERT_NE(file->GetSectionList(), nullptr);
    Symtab *table = file->GetSymtab();
    ASSERT_NE(table, nullptr);
    std::vector<uint32_t> indices;
    table->FindAllSymbolsWithNameAndType(ConstString(query), eSymbolTypeAny,
                                         indices, true);
    ASSERT_EQ(indices.size(), 1u);
    EXPECT_EQ(table->SymbolAtIndex(indices.front())->GetID(), 0u);
    EXPECT_EQ(table->SymbolAtIndex(indices.front())->GetType(),
              resolver ? eSymbolTypeResolver : eSymbolTypeTrampoline);
  }

  for (const auto &trie :
       {LinkageExportTrie({{{}, {{"_" + query, 1}, {"_unrelated", 2}}},
                           {{0, 0}, {}},
                           {{0x80}, {}}}),
        LinkageExportTrie({{{}, {{"_one", 1}, {"_two", 1}}}, {{0, 0}, {}}})}) {
    auto module = MetadataReaderModule({}, 8, eByteOrderLittle, trie);
    ObjectFile *file = module->GetObjectFile();
    ASSERT_NE(file, nullptr);
    // An unrelated malformed/shared-node branch prevents a full proof. No
    // successful prefix may publish a partial validation for later queries.
    EXPECT_TRUE(file->MayContainCLinkageSymbols({query}));
    EXPECT_TRUE(file->MayContainCLinkageSymbols({"absent"}));
    for (unsigned repeat = 0; repeat != 2; ++repeat)
      EXPECT_TRUE(file->MayContainCLinkageSymbols(
          {"$s7Fixture7missingyyF"}, SymbolNameLookupKind::Function));
    EXPECT_EQ(file->GetSymtab(false), nullptr);
  }
}

TEST_F(SymtabTest, MachODirectedExportsRespectLateTablesAndIndexCache) {
  const MetadataNList records[] = {{0xf, 0, 0x1800, "_code", 1},
                                   {0xf, 0, 0x1900, "_data", 2}};
  auto module = CodeDataReaderModule(records, false, 8, eByteOrderLittle,
                                     SingleExport("_$s7Fixture7runtimeyyF"));
  ObjectFile *file = module->GetObjectFile();
  ASSERT_NE(file, nullptr);
  const auto *metadata = file->GetDebugInfoMetadata();
  ASSERT_NE(metadata, nullptr);
  ASSERT_TRUE(metadata->has_no_stabs_and_code_and_data);
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"$s7Fixture7missingyyF"}));
  auto &properties = ModuleList::GetGlobalModuleListProperties();
  const bool previous = properties.GetEnableLLDBIndexCache();
  auto restore = llvm::make_scope_exit(
      [&] { properties.SetEnableLLDBIndexCache(previous); });
  ASSERT_TRUE(properties.SetEnableLLDBIndexCache(true));
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"$s7Fixture7missingyyF"}));
  EXPECT_EQ(file->GetSymtab(false), nullptr);
  ASSERT_TRUE(properties.SetEnableLLDBIndexCache(false));
  EXPECT_FALSE(file->MayContainCLinkageSymbols({"$s7Fixture7missingyyF"}));
  ASSERT_NE(file->GetSectionList(), nullptr);
  Symtab *table = file->GetSymtab();
  ASSERT_NE(table, nullptr);
  table->AddSymbol(Symbol(100, "$s7Fixture7missingyyF", eSymbolTypeCode, true,
                          false, false, false, SectionSP(), 0, 0, false, false,
                          0));
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"$s7Fixture7missingyyF"}));
  EXPECT_TRUE(file->MayContainCLinkageSymbols({"$s7Fixture7anotheryyF"}));
}
