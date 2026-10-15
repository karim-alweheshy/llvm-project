import binascii
from pathlib import Path

from lldbsuite.test.gdbclientutils import MockGDBServerResponder
from lldbsuite.test.lldbgdbclient import GDBRemoteTestBase


class TestqSymbol(GDBRemoteTestBase):
    class Responder(MockGDBServerResponder):
        def __init__(self):
            super().__init__()
            self.answers = {}

        def qOffsets(self):
            return "Text=0;Data=0"

        def qSymbol(self, symbol_args):
            if symbol_args == ":":
                name = "dispatch_queue_offsets"
            else:
                value, encoded_name = symbol_args.split(":")
                name = binascii.unhexlify(encoded_name).decode()
                self.answers[name] = int(value, 16) if value else None
                if name == "other_wire_request":
                    return "OK"
                name = "other_wire_request"
            return "qSymbol:" + binascii.hexlify(name.encode()).decode()

    def check_symbols(self, has_dispatch_data):
        self.runCmd("settings set target.preload-symbols false")
        fixture = Path(self.getSourceDir()) / "qsymbol.yaml"
        if not has_dispatch_data:
            missing = Path(self.getBuildArtifact("qsymbol-missing.yaml"))
            missing.write_text(
                fixture.read_text().replace(
                    "Name: dispatch_queue_offsets", "Name: unrelated_data"
                )
            )
            fixture = missing
        responder = self.Responder()
        self.server.responder = responder
        object_path = self.getBuildArtifact("qsymbol-object")
        self.yaml2obj(str(fixture), object_path)
        target = self.dbg.CreateTarget(object_path)
        self.connect(target)
        # The known dispatch ABI is data, not the C++ function basename at
        # 0x1000. Other remote requests retain their C++ basename matching.
        self.assertEqual(
            responder.answers,
            {
                "dispatch_queue_offsets": 0x2000 if has_dispatch_data else None,
                "other_wire_request": 0x1001,
            },
        )

    def test_c_runtime_data_and_generic_cxx_basename(self):
        self.check_symbols(has_dispatch_data=True)

    def test_absent_c_runtime_data_rejects_cxx_basename(self):
        self.check_symbols(has_dispatch_data=False)
