import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "windows_exports", Path(__file__).resolve().parents[1] / "cmake/filter_windows_exports.py"
)
exports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exports)


class WindowsExportsTest(unittest.TestCase):
    def test_project_functions_data_and_resources(self):
        for symbol in [
            "?parse@CmarkGfmParser@muffin@@QEAAHXZ",
            "?font@labels@mermaid@muffin@@YA?AVQFont@@XZ",
            "??0PieceTable@muffin@@QEAA@XZ",
            "??_7DocumentSession@muffin@@6B@",
            "??_R0?AVDocumentSession@muffin@@@8",
            "?qInitResources_mermaid_fonts@@YAHXZ",
            "?qCleanupResources_mermaid_fonts@@YAHXZ",
        ]:
            with self.subTest(symbol=symbol):
                self.assertTrue(exports.project_symbol(symbol))

    def test_helpers_with_project_parameter_types_are_not_apis(self):
        for symbol in [
            "?append@?$QList@VQString@@@@QEAAXAEBVQString@@@Z",
            "??$move@AEAVPieceTable@muffin@@@std@@YAXXZ",
            "?invoke@std@@YAXPEAVDocumentSession@muffin@@@Z",
            "?method@?$QList@VDocumentSession@muffin@@@@QEAAXXZ",
            "??_R0?AV?$vector@VDocumentSession@muffin@@@std@@@8",
            "?thirdPartyFunction@@YAXXZ",
        ]:
            with self.subTest(symbol=symbol):
                self.assertFalse(exports.project_symbol(symbol))

    def test_definition_preserves_data_marker_and_rejects_empty_scan(self):
        definition, count = exports.filtered_definition(
            "EXPORTS\n\t?parse@Parser@muffin@@QEAAXXZ\n"
            "\t??_7Parser@muffin@@6B@ DATA\n\t?helper@std@@YAXXZ\n"
        )
        self.assertEqual(count, 2)
        self.assertIn("??_7Parser@muffin@@6B@ DATA", definition)
        self.assertNotIn("helper", definition)
        with self.assertRaises(ValueError):
            exports.filtered_definition("EXPORTS\n?helper@std@@YAXXZ\n")

    def test_unchanged_interfaces_preserve_link_input_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "exports.def"
            definition = "EXPORTS\n?parse@Parser@muffin@@QEAAXXZ\n"
            exports.write_definition(output, definition)
            os.utime(output, ns=(1_000_000_000, 1_000_000_000))
            exports.write_definition(output, definition)
            self.assertEqual(output.stat().st_mtime_ns, 1_000_000_000)
            exports.write_definition(output, definition + "?newApi@muffin@@YAXXZ\n")
            self.assertNotEqual(output.stat().st_mtime_ns, 1_000_000_000)


if __name__ == "__main__":
    unittest.main()
