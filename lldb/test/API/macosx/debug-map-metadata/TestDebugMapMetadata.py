"""Preserve debug-map DWARF consumers after format-only OSO discovery."""

import os

import lldb
from lldbsuite.test.decorators import skipIfRemote, skipUnlessDarwin
from lldbsuite.test.lldbtest import TestBase


class DebugMapMetadataTest(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def check_function(self, target, name, source, type_name):
        contexts = target.FindFunctions(name, lldb.eFunctionNameTypeFull)
        functions = [contexts.GetContextAtIndex(i).GetFunction()
                     for i in range(contexts.GetSize())]
        functions = [function for function in functions if function.IsValid()]
        self.assertEqual(len(functions), 1)
        function = functions[0]
        address = function.GetStartAddress()
        resolved = address.GetSymbolContext(lldb.eSymbolContextEverything)
        self.assertEqual(resolved.GetFunction(), function)
        self.assertEqual(resolved.GetCompileUnit().GetFileSpec().GetFilename(), source)
        line = resolved.GetLineEntry()
        self.assertTrue(line.IsValid())
        self.assertEqual(line.GetFileSpec().GetFilename(), source)
        self.assertTrue(target.FindFirstType(type_name).IsValid())

    @skipIfRemote
    @skipUnlessDarwin
    def test_available_objects_and_archive_hydrate_ranges_and_uids(self):
        self.build(debug_info="dwarf")
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target.IsValid())
        # An ordinary object and an archive member both need full symbol ranges
        # before their DWARF can resolve source lines, types, UIDs and addresses.
        self.check_function(target, "ordinary", "ordinary.c", "OrdinaryType")
        self.check_function(target, "archived", "member.c", "ArchivedType")
        self.check_function(target, "ordinary", "ordinary.c", "OrdinaryType")

    @skipIfRemote
    @skipUnlessDarwin
    def test_missing_object_and_archive_keep_missing_debug_info_behavior(self):
        self.build(debug_info="dwarf")
        for path in ("main.o", "ordinary.o", "libmember.a"):
            os.unlink(self.getBuildArtifact(path))
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target.IsValid())
        for name in ("ordinary", "archived"):
            contexts = target.FindFunctions(name, lldb.eFunctionNameTypeFull)
            for i in range(contexts.GetSize()):
                self.assertFalse(contexts.GetContextAtIndex(i).GetFunction().IsValid())
        self.assertFalse(target.FindFirstType("OrdinaryType").IsValid())
        self.assertFalse(target.FindFirstType("ArchivedType").IsValid())
