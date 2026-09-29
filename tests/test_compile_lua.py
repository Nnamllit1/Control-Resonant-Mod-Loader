"""Offline compiler failures must not corrupt source or previous bytecode."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import compile_lua
from test_binlua import chunk, prototype, ins


class CompileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiler = self.root / "compiler with spaces.exe"
        self.compiler.write_bytes(b"test executable placeholder")
        self.source = self.root / "my script.luau"
        self.source.write_text("return 42\n", encoding="utf-8")
        self.output = self.root / "script.bytecode"
        self.output.write_bytes(b"previous output")
        self.code = chunk([prototype([ins("LOADN", d=42), ins("RETURN", b=2)])])[1:]

    def compile(self, **kwargs):
        return compile_lua.compile_file(self.compiler, self.source, self.output, **kwargs)

    def result(self, code=None, status=0):
        return subprocess.CompletedProcess([], status, self.code if code is None else code, b"syntax error on line 2")

    def test_raw_output_and_read_only_check(self):
        with patch.object(compile_lua.subprocess, "run", return_value=self.result()) as run:
            self.assertEqual(self.compile(), self.code)
            self.assertEqual(self.output.read_bytes(), self.code)
            before = self.output.stat().st_mtime_ns
            self.compile(check=True)
            self.assertEqual(self.output.stat().st_mtime_ns, before)
            self.assertEqual(run.call_args.args[0], [str(self.compiler.resolve()), "--binary", "-O0", "-g2", str(self.source.resolve())])
        self.assertEqual(self.source.read_text(), "return 42\n")

    def test_compiler_error_timeout_and_wrong_profile_preserve_output(self):
        for result in (self.result(status=1), self.result(b"\x07\x03"), self.result(b"\0compiler error"), self.result(self.code[:-1])):
            with self.subTest(result=result), patch.object(compile_lua.subprocess, "run", return_value=result):
                with self.assertRaises(ValueError):
                    self.compile()
                self.assertEqual(self.output.read_bytes(), b"previous output")
        with patch.object(compile_lua.subprocess, "run", side_effect=subprocess.TimeoutExpired("compiler", 60)):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.compile()
        self.assertEqual(self.output.read_bytes(), b"previous output")

    def test_changed_source_rejected(self):
        def changing(*args, **kwargs):
            self.source.write_text("return 99\n", encoding="utf-8")
            return self.result()
        with patch.object(compile_lua.subprocess, "run", side_effect=changing):
            with self.assertRaisesRegex(ValueError, "changed during"):
                self.compile()
        self.assertEqual(self.output.read_bytes(), b"previous output")

    def test_output_cannot_replace_source_or_compiler(self):
        for target in (self.source, self.compiler):
            with self.subTest(target=target), patch.object(compile_lua.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "overwrite"):
                    compile_lua.compile_file(self.compiler, self.source, target)
                run.assert_not_called()
        alias = self.root / "alias.bytecode"
        alias.hardlink_to(self.source)
        with self.assertRaisesRegex(ValueError, "overwrite"):
            compile_lua.compile_file(self.compiler, self.source, alias)

    def test_check_never_replaces_mismatch(self):
        with patch.object(compile_lua.subprocess, "run", return_value=self.result()):
            with self.assertRaisesRegex(ValueError, "differs"):
                self.compile(check=True)
        self.assertEqual(self.output.read_bytes(), b"previous output")

    def test_source_encoding_and_size_checked_before_compiler(self):
        for data in (b"\xff", b"x" * 33):
            self.source.write_bytes(data)
            with patch.object(compile_lua, "MAX_BYTES", 32), patch.object(compile_lua.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    self.compile()
                run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
