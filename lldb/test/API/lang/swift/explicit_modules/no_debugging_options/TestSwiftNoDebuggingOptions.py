import os

import lldb
from lldbsuite.test.decorators import skipEmbeddedSwift, skipUnlessDarwin, swiftTest
from lldbsuite.test.lldbtest import TestBase
from lldbsuite.test import lldbutil


class TestSwiftNoDebuggingOptions(TestBase):
    def start(self, collision=None, dictionary=None):
        self.build(dictionary=dictionary)
        if collision:
            directory = self.getBuildArtifact("filesystem")
            self.runCmd(
                f'settings set target.swift-module-search-paths "{directory}"'
            )
            if collision == "interface":
                os.remove(os.path.join(directory, "Unrelated.swiftmodule"))
            else:
                os.remove(os.path.join(directory, "Unrelated.swiftinterface"))
                os.remove(
                    os.path.join(directory, "Unrelated.private.swiftinterface")
                )
        target, process, thread, breakpoint = lldbutil.run_to_source_breakpoint(
            self, "Set breakpoint here", lldb.SBFileSpec("main.swift")
        )
        self.log = self.getBuildArtifact("types.log")
        self.runCmd(f'log enable lldb types -f "{self.log}"')
        self.expect("expression c.value", substrs=["42"])
        # The maps from this implicit build must not disable SDK imports.
        self.expect("expression date.timeIntervalSince1970", substrs=["0"])
        return thread.GetFrameAtIndex(0)

    def read_log(self):
        self.runCmd("log disable lldb types")
        with open(self.log) as log:
            return log.read()

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test_current_image(self):
        self.start()
        log = self.read_log()
        self.assertIn("Discovered main module a from image", log)
        self.assertIn('parsed module "Unrelated"', log)

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test_all_compiler_flags(self):
        self.runCmd("settings set target.use-all-compiler-flags true")
        self.start()
        log = self.read_log()
        self.assertIn('parsed module "Unrelated"', log)

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test_partial_module_map(self):
        frame = self.start(dictionary={"PARTIAL_MODULE_MAP": "YES"})
        # A partial Swift/modulemap-only map must keep implicit SDK imports.
        for expression, expected in [("Int(shim)", 7), ("Int(1) + 1", 2)]:
            result = frame.EvaluateExpression(expression)
            self.assertSuccess(result.GetError())
            self.assertEqual(result.GetValueAsUnsigned(), expected)
        self.read_log()

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test_other_image(self):
        self.start()
        # This module is loaded in the process, but is neither a dependency of
        # the current module nor on its module search path. Only its image
        # registers the binary AST in hidden/Unrelated.swiftmodule.
        self.expect("expression -- import Unrelated")
        self.expect("expression Unrelated.HiddenValue().number", substrs=["73"])
        log = self.read_log()
        self.assertIn('parsed module "Unrelated"', log)

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test_binary_collision(self):
        self.start(collision="binary")
        self.expect("expression -- import Unrelated")
        # The registered image's Int8 alias must beat the filesystem Int64.
        self.expect(
            "expression MemoryLayout<Unrelated.SelectedType>.size", substrs=["= 1"]
        )

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test_interface_collision(self):
        self.start(collision="interface")
        self.expect("expression -- import Unrelated")
        # Module-interface loading precedes the implicit serialized loader.
        self.expect(
            "expression MemoryLayout<Unrelated.SelectedType>.size", substrs=["= 1"]
        )

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test_can_import(self):
        frame = self.start()
        result = frame.EvaluateExpression(
            """({ () -> Bool in
            #if canImport(Unrelated)
              return true
            #else
              return false
            #endif
            })()"""
        )
        self.assertSuccess(result.GetError())
        self.assertEqual(result.GetValueAsUnsigned(), 1)
        self.expect("expression -- import Unrelated")
        self.expect("expression Unrelated.HiddenValue().number", substrs=["73"])
        self.read_log()

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test_failed_import(self):
        frame = self.start()
        # The DWARF importer can provide an empty module for an unknown name.
        # A member reference forces a failed lookup before testing recovery.
        result = frame.EvaluateExpression("import DoesNotExist\nDoesNotExist.value")
        self.assertFalse(result.GetError().Success())
        self.assertIn(
            "DoesNotExist",
            result.GetError().GetCString(),
        )
        self.expect("expression -- import Unrelated")
        self.expect("expression Unrelated.HiddenValue().number", substrs=["73"])
        self.read_log()
