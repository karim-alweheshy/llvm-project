import json
import os

import lldb
from lldbsuite.test.decorators import skipIf, skipIfWindows, skipUnlessDarwin
from lldbsuite.test.lldbtest import TestBase


@skipIfWindows
class TestBoundsSafetyRuntimeDetection(TestBase):
    def start(
        self,
        runtime_location=None,
        with_string=False,
        static=False,
        without_debug_info=False,
        late=False,
        strip_runtime_symbols=False,
        unrelated_dsym=False,
    ):
        flags = []
        if with_string:
            flags.append("-DWITH_STRING_TRAP")
        if static:
            flags.append("-DSTATIC_RUNTIME")
        if runtime_location in ("main", "both"):
            flags.append("-DCALL_MAIN_RUNTIME")
        if runtime_location in ("dylib", "cxx", "both"):
            flags.append("-DCALL_LIBRARY_RUNTIME")
        if late:
            self.build(
                dictionary={
                    "DYLIB_ONLY": "YES",
                    "DYLIB_NAME": "Late",
                    "DYLIB_CXX_SOURCES": "late_runtime.cpp",
                }
            )
            flags.append("-DLATE_LOAD")
        dictionary = {
            "RUNTIME_LOCATION": runtime_location or "none",
            "CFLAGS_EXTRAS": " ".join(flags),
        }
        if without_debug_info:
            dictionary["DEBUG_INFO_FLAG"] = "-g0"
        self.build(dictionary=dictionary)
        if unrelated_dsym and self.platformIsDarwin():
            library = self.getBuildArtifact(
                self.platformContext.getFullLibName("Unrelated")
            )
            self.runBuildCommand(
                ["xcrun", "dsymutil", "-o", library + ".dSYM", library]
            )
        if strip_runtime_symbols:
            library = self.getBuildArtifact(
                self.platformContext.getFullLibName("Unrelated")
            )
            self.runBuildCommand(["xcrun", "strip", "-x", library])
            self.runBuildCommand(["codesign", "-f", "-s", "-", library])
        self.runCmd("settings set target.preload-symbols false")
        self.runCmd("settings set target.load-script-from-symbol-file false")
        self.log = self.getBuildArtifact("instrumentation.log")
        self.runCmd(f'log enable lldb instrumentation-runtime -f "{self.log}"')
        target = self.createTestTarget(load_dependent_modules=False)
        breakpoint = target.BreakpointCreateByName(
            "main", target.GetExecutable().GetFilename()
        )
        self.assertEqual(breakpoint.GetNumLocations(), 1)
        arguments = (
            [self.getBuildArtifact(self.platformContext.getFullLibName("Late"))]
            if late else None
        )
        process = target.LaunchSimple(
            arguments, None, self.get_process_working_directory()
        )
        self.assertState(process.GetState(), lldb.eStateStopped)
        if runtime_location is not None:
            self.assertTrue(
                process.IsInstrumentationRuntimePresent(
                    lldb.eInstrumentationRuntimeTypeBoundsSafety
                )
            )
        return target

    def check_trap(self, target, module_name, with_string=False, raw_symbol=True):
        process = target.GetProcess()
        self.assertSuccess(process.Continue())
        self.assertState(process.GetState(), lldb.eStateStopped)
        thread = process.GetSelectedThread()
        self.assertEqual(thread.GetStopReason(), lldb.eStopReasonInstrumentation)
        frame = thread.GetFrameAtIndex(0)
        expected_name = (
            "__bounds_safety_soft_trap_s" if with_string
            else "__bounds_safety_soft_trap"
        )
        if raw_symbol:
            self.assertEqual(frame.GetSymbol().GetName(), expected_name)
        else:
            self.assertIn(
                frame.GetFunctionName(),
                (expected_name, expected_name + "()", "::" + expected_name + "()"),
            )
        self.assertIn(module_name, frame.GetModule().GetFileSpec().GetFilename())

    def test_runtime_detection_does_not_index_unrelated_symbols(self):
        # A complete dSYM preserves DWARF function/inline searches without
        # requiring debug-map relocation through locally available OSOs.
        target = self.start(unrelated_dsym=True)
        self.runCmd("log disable lldb instrumentation-runtime")
        with open(self.log) as log:
            self.assertIn(
                "did not find BoundsSafety soft trap functions in module", log.read()
            )

        def unrelated_statistics():
            options = lldb.SBStatisticsOptions()
            options.SetReportAllAvailableDebugInfo(False)
            stream = lldb.SBStream()
            target.GetStatistics(options).GetAsJSON(stream)
            return next(
                m
                for m in json.loads(stream.GetData())["modules"]
                if "Unrelated" in os.path.basename(m["path"])
            )

        unrelated = unrelated_statistics()
        if self.platformIsDarwin():
            library = self.getBuildArtifact(
                self.platformContext.getFullLibName("Unrelated")
            )
            module = target.FindModule(lldb.SBFileSpec(library))
            self.assertTrue(module.IsValid())
            dwarf_file = os.path.join(
                library + ".dSYM",
                "Contents",
                "Resources",
                "DWARF",
                os.path.basename(library),
            )
            # GetSymbolFileSpec may select a provider. Verify it does not hide
            # symbol-table materialization before making the zero assertions.
            symbol_file = module.GetSymbolFileSpec()
            selected_file = os.path.join(
                symbol_file.GetDirectory(), symbol_file.GetFilename()
            )
            # Selecting this dSYM also requires its UUID to match the module.
            self.assertEqual(
                os.path.realpath(selected_file), os.path.realpath(dwarf_file)
            )
            after_selection = unrelated_statistics()
            counters = (
                "symbolTableSymbolCount",
                "symbolTableParseTime",
                "symbolTableIndexTime",
            )
            self.assertEqual(
                {key: unrelated[key] for key in counters},
                {key: after_selection[key] for key in counters},
            )
            self.assertEqual(unrelated["symbolTableSymbolCount"], 0)
        else:
            self.assertGreater(unrelated["symbolTableSymbolCount"], 0)
        self.assertEqual(unrelated["symbolTableIndexTime"], 0)

    def test_hidden_minimal_runtime_in_executable(self):
        target = self.start("main")
        self.check_trap(target, target.GetExecutable().GetFilename())

    def test_hidden_string_runtime_in_executable(self):
        target = self.start("main", with_string=True)
        self.check_trap(target, target.GetExecutable().GetFilename(), with_string=True)

    def test_hidden_minimal_runtime_in_unrelated_library(self):
        target = self.start("dylib")
        self.check_trap(target, "Unrelated")

    def test_hidden_string_runtime_in_unrelated_library(self):
        target = self.start("dylib", with_string=True)
        self.check_trap(target, "Unrelated", with_string=True)

    def test_extern_c_runtime_in_cxx_library(self):
        target = self.start("cxx")
        self.check_trap(target, "Unrelated")

    def test_static_runtime_copies_in_multiple_images(self):
        target = self.start("both", static=True)
        self.check_trap(target, target.GetExecutable().GetFilename())
        self.check_trap(target, "Unrelated")

    def test_runtime_without_dwarf(self):
        target = self.start("dylib", without_debug_info=True)
        self.check_trap(target, "Unrelated")

    def test_late_loaded_runtime_copy(self):
        target = self.start("main", late=True)
        self.check_trap(target, target.GetExecutable().GetFilename())
        self.check_trap(target, "Late")

    @skipUnlessDarwin
    @skipIf(debug_info="dwarf")
    def test_extern_c_runtime_with_dwarf_only(self):
        target = self.start("cxx", strip_runtime_symbols=True)
        self.check_trap(target, "Unrelated", raw_symbol=False)
