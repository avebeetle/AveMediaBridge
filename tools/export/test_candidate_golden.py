"""Focused fail-closed controls; fixtures are not Golden qualification."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys

import run_candidate_golden as candidate


class GoldenBindingControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="golden-control-")
        self.root = Path(self.temp.name)
        self.manifest = json.loads(candidate.MANIFEST.read_text(encoding="utf-8-sig"))

    def tearDown(self):
        self.temp.cleanup()

    def reject(self, function, *args):
        with self.assertRaises(ValueError):
            function(*args)

    def test_identity_wrong_missing_and_correct(self):
        path = self.root / "module.dll"
        path.write_bytes(b"normal selected build")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        candidate.verify_identity(path, digest)
        self.reject(candidate.verify_identity, path, "0" * 64)
        self.reject(candidate.verify_identity, self.root / "missing.dll", digest)

    def test_public_entry_rejects_wrong_runtime_bridge_and_reused_output(self):
        command = [sys.executable, str(Path(candidate.__file__)),
                   "--bridge-repo", "D:/rvc/c++/DragonianVoice/AveMediaBridge_audio_export",
                   "--voice-repo", "C:/Users/USER/.codex/worktrees/audio-export/AveVoice",
                   "--bridge-build", "D:/ave-t7-bridge", "--voice-build", "D:/ave-t7-build",
                   "--bridge-dll", "D:/ave-t7-bridge/Release/AveMediaBridge.dll",
                   "--voice-exe", "D:/ave-t7-build/Release/AveVoiceTests.exe",
                   "--runtime-root", str(candidate.CANDIDATE), "--output", str(self.root / "fresh"),
                   "--bridge-head", "a" * 40, "--voice-head", "b" * 40]
        for option, value, message in [("--runtime-root", str(self.root), "not reviewed candidate"),
                                        ("--bridge-dll", "D:/ave-t7-bridge/Release/Fault.dll", "normal selected Bridge"),
                                        ("--output", str(self.root), "absent and absolute")]:
            changed = command.copy()
            changed[changed.index(option) + 1] = value
            process = subprocess.run(changed, capture_output=True, text=True)
            self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
            self.assertIn(message, process.stderr)
        self.assertFalse((self.root / "fresh").exists())

    def test_clean_exact_source(self):
        candidate.validate_source({"head": "a" * 40, "statusShort": [], "diffCheck": "PASS"}, "a" * 40)
        for field, value in (("head", "b" * 40), ("statusShort", [" M source.cpp"]), ("diffCheck", "FAIL")):
            state = {"head": "a" * 40, "statusShort": [], "diffCheck": "PASS"}
            state[field] = value
            self.reject(candidate.validate_source, state, "a" * 40)

    def test_only_two_manifest_head_substitutions(self):
        derived = candidate.derive_manifest(self.manifest, "a" * 40, "b" * 40)
        candidate.validate_manifest(self.manifest, derived, "a" * 40, "b" * 40)
        expected = copy.deepcopy(self.manifest)
        expected.update(bridgeGoldenHead="a" * 40, voiceGoldenHead="b" * 40)
        self.assertEqual(expected, derived)
        for mutate in (lambda x: x["realFiles"][0].update(acceptedFrames=1),
                       lambda x: x["syntheticTests"].reverse(),
                       lambda x: x.update(voiceGoldenHead="c" * 40)):
            changed = copy.deepcopy(derived)
            mutate(changed)
            self.reject(candidate.validate_manifest, self.manifest, changed, "a" * 40, "b" * 40)

    def test_runner_pin_and_binding_only(self):
        source = candidate.HISTORICAL.read_bytes()
        adapted = candidate.adapt_source(source)
        self.assertIn("exe = VOICE_EXE", adapted)
        self.assertIn("dll_path = BRIDGE_DLL", adapted)
        self.assertIn('build = BRIDGE_BUILD if name.startswith("AveMediaBridge") else VOICE_BUILD', adapted)
        self.assertNotIn('BRIDGE / "build"', adapted)
        self.assertNotIn('VOICE / "build"', adapted)
        self.reject(candidate.adapt_source, source + b"\n# drift\n")

    def test_output_fresh_absolute_and_not_protected(self):
        protected = self.root / "reference"
        protected.mkdir()
        candidate.validate_output(self.root / "fresh", [protected])
        for path in (self.root, Path("relative"), protected / "new", protected.parent):
            self.reject(candidate.validate_output, path, [protected])

    def test_registration_exact_unique_and_concrete(self):
        exe = self.root / "Release" / "test.exe"
        exe.parent.mkdir()
        exe.write_bytes(b"test")
        expected = [str(exe), "waveform", "fixture"]
        good = {"tests": [{"name": "Exact.name", "command": expected, "properties": []}]}
        candidate.validate_registration(good, "Exact.name", expected)
        for value in ({"tests": []}, {"tests": good["tests"] * 2},
                      {"tests": [{"name": "Exact.name"}]},
                      {"tests": [{"name": "Exact.name", "command": [str(exe), "wrong"]}]},
                      {"tests": [{"name": "Exact.name", "command": expected, "properties": [{"name": "DISABLED", "value": True}]}]}):
            self.reject(candidate.validate_registration, value, "Exact.name", expected)
        exe.unlink()
        self.reject(candidate.validate_registration, good, "Exact.name", expected)

    def real_fixture(self):
        return [{"id": item["id"], "passed": True, "checks": {"inputIntegrity": True},
                 "bridge": {"probeResult": 0, "importResult": 0},
                 "voice": {"fullOverviewExitCode": 0, "fullOverviewMarkerFound": True,
                           "workflowMarkerFound": True, "reopen": {"exitCode": 0, "timedOut": False, "markerFound": True},
                           "deepRight": {"exitCode": 0, "markerFound": True} if item["id"] in candidate.DEEP else {}}}
                for item in self.manifest["realFiles"]]

    def test_real_execution_missing_failed_and_nonzero(self):
        rows = self.real_fixture()
        candidate.validate_real(rows, self.manifest)
        self.reject(candidate.validate_real, rows[:-1], self.manifest)
        self.reject(candidate.validate_real, rows + rows[:1], self.manifest)
        for mutate in (lambda x: x[0].update(passed=False),
                       lambda x: x[0]["bridge"].update(importResult=2),
                       lambda x: x[0]["voice"].update(fullOverviewExitCode=1),
                       lambda x: x[0]["voice"].update(fullOverviewMarkerFound=False),
                       lambda x: x[0]["voice"].update(workflowMarkerFound=False),
                       lambda x: x[0]["voice"]["reopen"].update(exitCode=1),
                       lambda x: x[0]["voice"]["reopen"].update(timedOut=True),
                       lambda x: x[0]["voice"]["reopen"].update(markerFound=False),
                       lambda x: x[2]["voice"]["deepRight"].update(exitCode=1)):
            changed = copy.deepcopy(rows)
            mutate(changed)
            self.reject(candidate.validate_real, changed, self.manifest)

    def test_synthetic_requires_real_pass_not_zero_exit_only(self):
        names = self.manifest["syntheticTests"]
        rows = [{"testName": n, "passed": True, "exitCode": 0} for n in names]
        executions = [{"name": n, "exitCode": 0, "timedOut": False,
                       "output": f"1/1 Test #1: {n} .... Passed 0.1 sec\n100% tests passed, 0 tests failed out of 1"} for n in names]
        candidate.validate_synthetic(rows, executions, self.manifest)
        self.reject(candidate.validate_synthetic, rows[:-1], executions, self.manifest)
        self.reject(candidate.validate_synthetic, rows, executions[:-1], self.manifest)
        for output in ("No tests were found!!!", f"1/1 Test #1: {names[0]} .... ***Skipped 0.1 sec\n100% tests passed, 0 tests failed out of 1",
                       "100% tests passed, 0 tests failed out of 1"):
            changed = copy.deepcopy(executions)
            changed[0]["output"] = output
            self.reject(candidate.validate_synthetic, rows, changed, self.manifest)
        for field, value in (("exitCode", 1), ("timedOut", True)):
            changed = copy.deepcopy(executions)
            changed[0][field] = value
            self.reject(candidate.validate_synthetic, rows, changed, self.manifest)
        for field, value in (("exitCode", 1), ("passed", False)):
            changed = copy.deepcopy(rows)
            changed[0][field] = value
            self.reject(candidate.validate_synthetic, changed, executions, self.manifest)


if __name__ == "__main__":
    unittest.main(verbosity=2)
