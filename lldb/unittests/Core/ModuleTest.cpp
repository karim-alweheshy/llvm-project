//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/Module.h"
#include "Plugins/Language/CPlusPlus/CPlusPlusLanguage.h"
#include "Plugins/ObjectFile/ELF/ObjectFileELF.h"
#include "Plugins/ObjectFile/Mach-O/ObjectFileMachO.h"
#include "Plugins/SymbolFile/Symtab/SymbolFileSymtab.h"
#include "TestingSupport/SubsystemRAII.h"
#include "TestingSupport/TestUtilities.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Target/Language.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/FormatVariadic.h"
#include "gtest/gtest.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>

using namespace lldb;
using namespace lldb_private;

// Test that Module::FindFunctions correctly finds C++ mangled symbols
// even when multiple language plugins are registered.
TEST(ModuleTest, FindFunctionsCppMangledName) {
  // Create a mock language. The point of this language is to return something
  // in GetFunctionNameInfo that would interfere with the C++ language plugin,
  // were they sharing the same LookupInfo.
  class MockLanguageWithBogusLookupInfo : public Language {
  public:
    MockLanguageWithBogusLookupInfo() = default;
    ~MockLanguageWithBogusLookupInfo() override = default;

    lldb::LanguageType GetLanguageType() const override {
      // The language here doesn't really matter, it just has to be something
      // that is not C/C++/ObjC.
      return lldb::eLanguageTypeSwift;
    }

    llvm::StringRef GetPluginName() override { return "mock-bogus-language"; }

    bool IsSourceFile(llvm::StringRef file_path) const override {
      return file_path.ends_with(".swift");
    }

    std::pair<lldb::FunctionNameType, std::optional<ConstString>>
    GetFunctionNameInfo(ConstString name) const override {
      // Say that every function is a selector.
      return {lldb::eFunctionNameTypeSelector, ConstString("BOGUS_BASENAME")};
    }

    static void Initialize() {
      PluginManager::RegisterPlugin(GetPluginNameStatic(), "Mock Language",
                                    CreateInstance);
    }

    static void Terminate() { PluginManager::UnregisterPlugin(CreateInstance); }

    static lldb_private::Language *CreateInstance(lldb::LanguageType language) {
      if (language == lldb::eLanguageTypeSwift)
        return new MockLanguageWithBogusLookupInfo();
      return nullptr;
    }

    static llvm::StringRef GetPluginNameStatic() {
      return "mock-bogus-language";
    }
  };
  SubsystemRAII<FileSystem, HostInfo, ObjectFileELF, SymbolFileSymtab,
                CPlusPlusLanguage, MockLanguageWithBogusLookupInfo>
      subsystems;

  // Create a simple ELF module with std::vector::size() as the only symbol.
  auto ExpectedFile = TestFile::fromYaml(R"(
--- !ELF
FileHeader:
  Class:           ELFCLASS64
  Data:            ELFDATA2LSB
  Type:            ET_DYN
  Machine:         EM_X86_64
Sections:
  - Name:            .text
    Type:            SHT_PROGBITS
    Flags:           [ SHF_ALLOC, SHF_EXECINSTR ]
    Address:         0x1000
    AddressAlign:    0x10
    Size:            0x100
Symbols:
  - Name:            _ZNSt6vectorIiE4sizeEv
    Type:            STT_FUNC
    Section:         .text
    Value:           0x1030
    Size:            0x20
...
)");
  ASSERT_THAT_EXPECTED(ExpectedFile, llvm::Succeeded());

  auto module_sp = std::make_shared<Module>(ExpectedFile->moduleSpec());

  // Verify both C++ and our mock language are registered.
  Language *cpp_lang = Language::FindPlugin(lldb::eLanguageTypeC_plus_plus);
  Language *mock_lang = Language::FindPlugin(lldb::eLanguageTypeSwift);
  ASSERT_NE(cpp_lang, nullptr) << "C++ language plugin should be registered";
  ASSERT_NE(mock_lang, nullptr)
      << "Mock Swift language plugin should be registered";

  ModuleFunctionSearchOptions function_options;
  function_options.include_symbols = true;

  ConstString symbol_name("_ZNSt6vectorIiE4sizeEv");
  SymbolContextList results;
  module_sp->FindFunctions(symbol_name, CompilerDeclContext(),
                           eFunctionNameTypeAuto, function_options, results);

  // Assert that we found one symbol.
  ASSERT_EQ(results.GetSize(), 1u);

  auto result = results[0];
  auto name = result.GetFunctionName();
  // Assert that the symbol we found is what we expected.
  ASSERT_EQ(name, "std::vector<int>::size()");
  ASSERT_EQ(result.GetLanguage(), eLanguageTypeC_plus_plus);
}

TEST(ModuleTest, ResolveSymbolContextForAddressExactMatch) {
  // Test that ResolveSymbolContextForAddress prefers exact symbol matches
  // over symbols that merely contain the address.
  SubsystemRAII<FileSystem, HostInfo, ObjectFileELF, SymbolFileSymtab>
      subsystems;

  auto ExpectedFile = TestFile::fromYaml(R"(
--- !ELF
FileHeader:
  Class:           ELFCLASS64
  Data:            ELFDATA2LSB
  Type:            ET_DYN
  Machine:         EM_X86_64
Sections:
  - Name:            .text
    Type:            SHT_PROGBITS
    Flags:           [ SHF_ALLOC, SHF_EXECINSTR ]
    Address:         0x1000
    AddressAlign:    0x10
    Size:            0x200
Symbols:
  - Name:            outer_function
    Type:            STT_FUNC
    Section:         .text
    Value:           0x1000
    Size:            0x100
  - Name:            inner_function
    Type:            STT_FUNC
    Section:         .text
    Value:           0x1050
    Size:            0x10
...
)");
  ASSERT_THAT_EXPECTED(ExpectedFile, llvm::Succeeded());

  auto module_sp = std::make_shared<Module>(ExpectedFile->moduleSpec());

  Address addr(module_sp->GetSectionList()->GetSectionAtIndex(0), 0x50);
  SymbolContext sc;
  uint32_t resolved =
      module_sp->ResolveSymbolContextForAddress(addr, eSymbolContextSymbol, sc);

  ASSERT_TRUE(resolved & eSymbolContextSymbol);
  ASSERT_NE(sc.symbol, nullptr);
  EXPECT_STREQ(sc.symbol->GetName().GetCString(), "inner_function");
}

namespace {

// Pause only the first construction of a selected file. A second caller can
// finish construction and publish while the first caller is still paused.
class ModuleConstructionGate {
public:
  explicit ModuleConstructionGate(const FileSpec &file,
                                  std::function<void()> on_first_visit = {})
      : m_file(file), m_on_first_visit(std::move(on_first_visit)) {}

  void Visit(const FileSpec &file) {
    if (file != m_file)
      return;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_entered)
      return;
    m_entered = true;
    m_cv.notify_all();
    m_cv.wait(lock, [&] { return m_released; });
    lock.unlock();
    if (m_on_first_visit)
      m_on_first_visit();
  }

  bool WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, std::chrono::seconds(10),
                         [&] { return m_entered; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_released = true;
    m_cv.notify_all();
  }

private:
  FileSpec m_file;
  std::function<void()> m_on_first_visit;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_entered = false;
  bool m_released = false;
};

class ModuleConstructionObserver {
public:
  // Set before starting workers, and clear only after joining them.
  static ModuleConstructionGate *gate;

  static void Initialize() {
    PluginManager::RegisterPlugin("module-construction-observer", "test only",
                                  CreateInstance, nullptr,
                                  GetModuleSpecifications);
  }

  static void Terminate() { PluginManager::UnregisterPlugin(CreateInstance); }

private:
  static ObjectFile *CreateInstance(const ModuleSP &, DataExtractorSP,
                                    lldb::offset_t, const FileSpec *,
                                    lldb::offset_t, lldb::offset_t) {
    return nullptr;
  }

  static size_t GetModuleSpecifications(const FileSpec &file, DataExtractorSP &,
                                        lldb::offset_t, lldb::offset_t,
                                        lldb::offset_t, ModuleSpecList &) {
    if (gate)
      gate->Visit(file);
    return 0;
  }
};

ModuleConstructionGate *ModuleConstructionObserver::gate = nullptr;

class SharedModuleListTest : public testing::Test {
  SubsystemRAII<FileSystem, HostInfo, ModuleConstructionObserver,
                ObjectFileMachO>
      m_subsystems;
  std::vector<llvm::sys::fs::TempFile> m_files;

protected:
  void TearDown() override {
    ModuleConstructionObserver::gate = nullptr;
    ModuleList::RemoveOrphanSharedModules(true);
    for (auto &file : m_files)
      EXPECT_THAT_ERROR(file.discard(), llvm::Succeeded());
  }

  llvm::Expected<ModuleSpec>
  CreateObject(llvm::StringRef uuid = "00112233-4455-6677-8899-AABBCCDDEEFF",
               uint32_t type = llvm::MachO::MH_DYLIB) {
    auto object = TestFile::fromYaml(llvm::formatv(R"(
--- !mach-o
FileHeader:
  magic: 0xFEEDFACF
  cputype: 0x0100000C
  cpusubtype: 0
  filetype: {1}
  ncmds: 2
  sizeofcmds: 56
  flags: 0
  reserved: 0
LoadCommands:
  - cmd: LC_UUID
    cmdsize: 24
    uuid: {0}
  - cmd: LC_ID_DYLIB
    cmdsize: 32
    dylib:
      name: 24
      timestamp: 0
      current_version: 0
      compatibility_version: 0
    Content: module
    ZeroPadBytes: 2
)",
                                                   uuid, type)
                                         .str());
    if (!object)
      return object.takeError();
    auto file = object->writeToTemporaryFile();
    if (!file)
      return file.takeError();
    ModuleSpec spec(FileSpec(file->TmpName));
    spec.GetUUID().SetFromStringRef(uuid);
    m_files.push_back(std::move(*file));
    return spec;
  }

  struct LoadResult {
    ModuleSP module;
    Status error;
    bool created = false;
    llvm::SmallVector<ModuleSP, 1> old_modules;
  };

  static LoadResult Load(const ModuleSpec &spec, bool always_create = false) {
    LoadResult result;
    result.error = ModuleList::GetSharedModule(spec, result.module, nullptr,
                                               &result.old_modules,
                                               &result.created, always_create);
    return result;
  }

  int GetFileDescriptor(size_t index) { return m_files[index].FD; }
};

} // namespace

TEST_F(SharedModuleListTest, UnrelatedModulesConstructConcurrently) {
  auto a_spec = CreateObject();
  auto b_spec = CreateObject("10112233-4455-6677-8899-AABBCCDDEEFF");
  ASSERT_THAT_EXPECTED(a_spec, llvm::Succeeded());
  ASSERT_THAT_EXPECTED(b_spec, llvm::Succeeded());
  ModuleConstructionGate gate(a_spec->GetFileSpec());
  ModuleConstructionObserver::gate = &gate;
  auto a = std::async(std::launch::async, [&] { return Load(*a_spec); });
  EXPECT_TRUE(gate.WaitUntilEntered());
  auto b = std::async(std::launch::async, [&] { return Load(*b_spec); });
  EXPECT_EQ(b.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  // Release before joining either worker, including when an expectation fails.
  gate.Release();
  auto a_result = a.get();
  auto b_result = b.get();
  EXPECT_TRUE(a_result.error.Success());
  EXPECT_TRUE(b_result.error.Success());
  EXPECT_TRUE(a_result.module);
  EXPECT_TRUE(b_result.module);
  EXPECT_NE(a_result.module, b_result.module);
  EXPECT_TRUE(a_result.created);
  EXPECT_TRUE(b_result.created);
}

TEST_F(SharedModuleListTest, ConcurrentLoadsReturnOneCanonicalModule) {
  auto spec = CreateObject();
  ASSERT_THAT_EXPECTED(spec, llvm::Succeeded());
  ModuleConstructionGate gate(spec->GetFileSpec());
  ModuleConstructionObserver::gate = &gate;
  auto a = std::async(std::launch::async, [&] { return Load(*spec); });
  EXPECT_TRUE(gate.WaitUntilEntered());
  auto b = std::async(std::launch::async, [&] { return Load(*spec); });
  EXPECT_EQ(b.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  gate.Release();
  auto a_result = a.get();
  auto b_result = b.get();
  EXPECT_TRUE(a_result.error.Success());
  EXPECT_TRUE(b_result.error.Success());
  ASSERT_TRUE(b_result.module);
  EXPECT_EQ(a_result.module, b_result.module);
  EXPECT_FALSE(a_result.created);
  EXPECT_TRUE(b_result.created);
  EXPECT_TRUE(a_result.old_modules.empty());
  EXPECT_TRUE(b_result.old_modules.empty());
}

TEST_F(SharedModuleListTest, ConcurrentUUIDAliasesReturnCanonicalModule) {
  auto a_spec = CreateObject();
  auto b_spec = CreateObject();
  ASSERT_THAT_EXPECTED(a_spec, llvm::Succeeded());
  ASSERT_THAT_EXPECTED(b_spec, llvm::Succeeded());
  ASSERT_NE(a_spec->GetFileSpec(), b_spec->GetFileSpec());
  ModuleConstructionGate gate(a_spec->GetFileSpec());
  ModuleConstructionObserver::gate = &gate;
  auto a = std::async(std::launch::async, [&] { return Load(*a_spec); });
  EXPECT_TRUE(gate.WaitUntilEntered());
  auto b = std::async(std::launch::async, [&] { return Load(*b_spec); });
  EXPECT_EQ(b.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  gate.Release();
  auto a_result = a.get();
  auto b_result = b.get();
  EXPECT_TRUE(a_result.error.Success());
  EXPECT_TRUE(b_result.error.Success());
  ASSERT_TRUE(b_result.module);
  EXPECT_EQ(a_result.module, b_result.module);
  EXPECT_EQ(a_result.module->GetFileSpec(), b_spec->GetFileSpec());
  EXPECT_FALSE(a_result.created);
  EXPECT_TRUE(b_result.created);
}

TEST_F(SharedModuleListTest, RecheckCacheAfterFailedConstruction) {
  auto spec = CreateObject();
  ASSERT_THAT_EXPECTED(spec, llvm::Succeeded());
  ModuleSpec invalid_arch_spec(*spec);
  invalid_arch_spec.GetArchitecture() = ArchSpec("x86_64-apple-macosx");
  ModuleConstructionGate gate(spec->GetFileSpec());
  ModuleConstructionObserver::gate = &gate;
  auto a =
      std::async(std::launch::async, [&] { return Load(invalid_arch_spec); });
  EXPECT_TRUE(gate.WaitUntilEntered());
  auto b = std::async(std::launch::async, [&] { return Load(*spec); });
  EXPECT_EQ(b.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  gate.Release();
  auto a_result = a.get();
  auto b_result = b.get();
  // Cached UUID lookup has always accepted aliases independently of the
  // requested path/architecture. A failed local candidate must still recheck
  // it.
  EXPECT_TRUE(a_result.error.Success());
  EXPECT_TRUE(b_result.error.Success());
  ASSERT_TRUE(b_result.module);
  EXPECT_EQ(a_result.module, b_result.module);
  EXPECT_FALSE(a_result.created);
  EXPECT_TRUE(b_result.created);
}

TEST_F(SharedModuleListTest, StaleModuleIsReplacedAndReported) {
  auto spec = CreateObject();
  ASSERT_THAT_EXPECTED(spec, llvm::Succeeded());
  auto first = Load(*spec);
  ASSERT_TRUE(first.module);
  ASSERT_TRUE(first.created);
  ASSERT_NO_ERROR(llvm::sys::fs::setLastAccessAndModificationTime(
      GetFileDescriptor(0),
      first.module->GetModificationTime() + std::chrono::seconds(2)));
  auto second = Load(*spec);
  EXPECT_TRUE(second.error.Success());
  ASSERT_TRUE(second.module);
  EXPECT_NE(first.module, second.module);
  EXPECT_TRUE(second.created);
  ASSERT_EQ(second.old_modules.size(), 1u);
  EXPECT_EQ(second.old_modules.front(), first.module);
  EXPECT_FALSE(ModuleList::ModuleIsInCache(first.module.get()));
  EXPECT_TRUE(ModuleList::ModuleIsInCache(second.module.get()));
}

TEST_F(SharedModuleListTest, InvalidCandidatesAreNotPublished) {
  auto spec = CreateObject();
  ASSERT_THAT_EXPECTED(spec, llvm::Succeeded());
  ModuleSpec invalid_uuid(*spec);
  invalid_uuid.GetUUID().SetFromStringRef(
      "10112233-4455-6677-8899-AABBCCDDEEFF");
  auto uuid_result = Load(invalid_uuid);
  EXPECT_FALSE(uuid_result.module);
  EXPECT_FALSE(uuid_result.created);
  EXPECT_FALSE(ModuleList::FindSharedModule(invalid_uuid.GetUUID()));
  ModuleSpec invalid_arch(*spec);
  invalid_arch.GetArchitecture() = ArchSpec("x86_64-apple-macosx");
  auto arch_result = Load(invalid_arch);
  EXPECT_FALSE(arch_result.module);
  EXPECT_FALSE(arch_result.created);
  EXPECT_FALSE(ModuleList::FindSharedModule(spec->GetUUID()));
  auto stub = CreateObject("20112233-4455-6677-8899-AABBCCDDEEFF",
                           llvm::MachO::MH_DYLIB_STUB);
  ASSERT_THAT_EXPECTED(stub, llvm::Succeeded());
  auto stub_result = Load(*stub);
  EXPECT_FALSE(stub_result.module);
  EXPECT_FALSE(stub_result.created);
  EXPECT_FALSE(ModuleList::FindSharedModule(stub->GetUUID()));
}

TEST_F(SharedModuleListTest, AlwaysCreateReplacesCanonicalModule) {
  auto spec = CreateObject();
  ASSERT_THAT_EXPECTED(spec, llvm::Succeeded());
  auto first = Load(*spec);
  ASSERT_TRUE(first.module);
  auto second = Load(*spec, true);
  EXPECT_TRUE(second.error.Success());
  ASSERT_TRUE(second.module);
  EXPECT_NE(first.module, second.module);
  EXPECT_TRUE(first.created);
  EXPECT_TRUE(second.created);
  ASSERT_EQ(second.old_modules.size(), 1u);
  EXPECT_EQ(second.old_modules.front(), first.module);
  auto cached = Load(*spec);
  EXPECT_EQ(cached.module, second.module);
  EXPECT_FALSE(cached.created);
}

TEST_F(SharedModuleListTest, RecheckEvictsModuleChangedDuringConstruction) {
  auto spec = CreateObject();
  ASSERT_THAT_EXPECTED(spec, llvm::Succeeded());
  ModuleConstructionGate gate(spec->GetFileSpec());
  ModuleConstructionObserver::gate = &gate;
  auto a = std::async(std::launch::async, [&] { return Load(*spec); });
  EXPECT_TRUE(gate.WaitUntilEntered());
  auto b = std::async(std::launch::async, [&] { return Load(*spec); });
  bool b_ready =
      b.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  EXPECT_TRUE(b_ready);
  // On failure, unblock before joining so the test cannot hang on old code.
  if (!b_ready)
    gate.Release();
  auto b_result = b.get();
  EXPECT_TRUE(b_result.module);
  if (b_result.module)
    EXPECT_FALSE(llvm::sys::fs::setLastAccessAndModificationTime(
        GetFileDescriptor(0),
        b_result.module->GetModificationTime() + std::chrono::seconds(2)));
  gate.Release();
  auto a_result = a.get();
  EXPECT_TRUE(a_result.error.Success());
  ASSERT_TRUE(a_result.module);
  EXPECT_NE(a_result.module, b_result.module);
  EXPECT_TRUE(a_result.created);
  ASSERT_EQ(a_result.old_modules.size(), 1u);
  EXPECT_EQ(a_result.old_modules.front(), b_result.module);
}

TEST_F(SharedModuleListTest, RecursiveConstructionReturnsPublishedModule) {
  auto spec = CreateObject();
  ASSERT_THAT_EXPECTED(spec, llvm::Succeeded());
  std::optional<LoadResult> nested;
  ModuleConstructionGate gate(spec->GetFileSpec(),
                              [&] { nested.emplace(Load(*spec)); });
  gate.Release();
  ModuleConstructionObserver::gate = &gate;
  auto outer = Load(*spec);
  ASSERT_TRUE(nested);
  EXPECT_TRUE(outer.error.Success());
  EXPECT_TRUE(nested->error.Success());
  ASSERT_TRUE(nested->module);
  EXPECT_EQ(outer.module, nested->module);
  EXPECT_FALSE(outer.created);
  EXPECT_TRUE(nested->created);
  EXPECT_TRUE(outer.old_modules.empty());
}
