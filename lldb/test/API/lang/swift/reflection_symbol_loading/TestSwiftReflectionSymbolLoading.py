import json

import lldb
from lldbsuite.test.decorators import skipEmbeddedSwift, skipUnlessDarwin, swiftTest
from lldbsuite.test.lldbtest import TestBase
from lldbsuite.test import lldbutil


class TestSwiftReflectionSymbolLoading(TestBase):
    def launch_fixture(self, file_cache=True):
        self.build()
        self.runCmd("settings set target.preload-symbols false")
        self.runCmd("settings set target.load-script-from-symbol-file false")
        # Argument formatting would reflect the values before the test's first
        # counter snapshot. Keep this control's first consumer explicit.
        self.runCmd('settings set frame-format "frame #${frame.index}: ${frame.pc}\\n"')
        self.runCmd(
            "settings set target.experimental.swift-read-metadata-from-file-cache "
            + ("true" if file_cache else "false")
        )
        target, process, thread, _ = lldbutil.run_to_source_breakpoint(
            self,
            "Set breakpoint here",
            lldb.SBFileSpec("main.swift"),
            extra_images=["ReflectionProbe"],
        )
        return target, thread.frames[0]

    def library_stats(self, target):
        options = lldb.SBStatisticsOptions()
        options.SetSummaryOnly(False)
        options.SetIncludeModules(True)
        options.SetReportAllAvailableDebugInfo(False)
        stream = lldb.SBStream()
        self.assertSuccess(target.GetStatistics(options).GetAsJSON(stream))
        statistics = json.loads(stream.GetData())
        matches = [
            module
            for module in statistics["modules"]
            if module["path"].endswith("libReflectionProbe.dylib")
        ]
        self.assertEqual(len(matches), 1)
        return matches[0]

    def check_payloads(self, frame):
        payload = frame.FindVariable("payload")
        self.assertSuccess(payload.GetError())
        integer = payload.GetChildMemberWithName("integer")
        self.assertSuccess(integer.GetError())
        self.assertEqual(integer.GetValueAsSigned(), 23)
        flag = payload.GetChildMemberWithName("flag")
        self.assertSuccess(flag.GetError())
        self.assertEqual(flag.GetValueAsUnsigned(), 1)
        holder = frame.FindVariable("holder").GetDynamicValue(
            lldb.eDynamicCanRunTarget
        )
        self.assertSuccess(holder.GetError())
        integer = holder.GetChildMemberWithName("payload").GetChildMemberWithName(
            "integer"
        )
        self.assertSuccess(integer.GetError())
        self.assertEqual(integer.GetValueAsSigned(), 23)

    def check_cold_symbols(self, file_cache):
        target, frame = self.launch_fixture(file_cache)
        before = self.library_stats(target)
        self.assertEqual(before["symbolTableSymbolCount"], 0)
        self.check_payloads(frame)
        after = self.library_stats(target)
        self.assertEqual(after["symbolTableSymbolCount"], 0)
        self.assertEqual(after["symbolTableParseTime"], before["symbolTableParseTime"])
        self.assertEqual(after["symbolTableIndexTime"], before["symbolTableIndexTime"])

    @skipUnlessDarwin
    @skipEmbeddedSwift
    @swiftTest
    def test_cold_file_cache(self):
        self.check_cold_symbols(True)

    @skipUnlessDarwin
    @skipEmbeddedSwift
    @swiftTest
    def test_cold_live_memory(self):
        self.check_cold_symbols(False)

    @skipUnlessDarwin
    @skipEmbeddedSwift
    @swiftTest
    def test_warm_symbols(self):
        target, frame = self.launch_fixture()
        module = target.FindModule(lldb.SBFileSpec("libReflectionProbe.dylib"))
        self.assertTrue(module.IsValid())
        self.assertTrue(module.FindSymbol("reflection_probe_marker").IsValid())
        self.assertGreater(self.library_stats(target)["symbolTableSymbolCount"], 0)
        self.check_payloads(frame)

    @skipUnlessDarwin
    @skipEmbeddedSwift
    @swiftTest
    def test_objc_and_opaque(self):
        _, frame = self.launch_fixture()
        opaque = frame.EvaluateExpression("opaque.integer")
        self.assertSuccess(opaque.GetError())
        self.assertEqual(opaque.GetValueAsSigned(), 23)
        number = frame.EvaluateExpression("number.intValue")
        self.assertSuccess(number.GetError())
        self.assertEqual(number.GetValueAsSigned(), 7)
        length = frame.EvaluateExpression("text.length")
        self.assertSuccess(length.GetError())
        self.assertEqual(length.GetValueAsUnsigned(), 5)
