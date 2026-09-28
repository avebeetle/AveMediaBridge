"""Behavioral controls for the public report comparator and qualification entry point."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
IDS = list(range(1, 70)) + [7006]


def failure_log(version, cause="Invalid data found when processing input"):
    return (f"Smoke DLL import session\n  DLL: D:\\owned-{version}\\Release\\AveMediaBridge.dll\n"
        f"  FFmpeg DLL dir: D:\\owned-{version}\\Release\\Lib\\ffmpeg\n\nResult: FAIL\n"
        f"AveMediaBridge_ImportAudioToSession returned 2\nDLL last error: avformat_open_input failed: {cause}")


def fixtures():
    baseline = {"files": [{"index": i, "fileName": f"{i:02}_sample.wav" if i != 7006 else "6. Практика (Сцена 3).mp4",
        "importSucceeded": i < 63 or i == 7006, "finalWrittenFrames": 12 if i < 63 or i == 7006 else 0,
        "authorityClassification": "historical-only", "safetyClassification": "historical-only"} for i in IDS]}
    reports = []
    for version in ("stable", "candidate"):
        rows = []
        for b in baseline["files"]:
            success = b["importSucceeded"]
            rows.append({"index": b["index"], "fileName": b["fileName"], "mediaBytes": 100, "mediaSha256": "a" * 64,
                "succeeded": success, "returnCode": 0 if success else 2, "probeReturnCode": 0 if success else 2,
                "frames": 12 if success else 0, "sampleRate": 48000 if success else 0, "channels": 2 if success else 0,
                "probeAuthority": {"decodedSampleFrames": 12 if success else 0, "decodedSampleFramesKind": "exact" if success else "unknown",
                    "decodedSampleFramesTrust": "authoritative" if success else "unknown", "decodedSampleFramesSource": "actual-probe",
                    "mediaOpenAuthoritySource": "actual-probe", "mediaOpenAuthorityTrust": "authoritative",
                    "mediaOpenAuthorityDomain": "presentation", "mediaOpenDisposition": "accept" if success else "reject",
                    "sampleRate": 48000 if success else 0, "channels": 2 if success else 0},
                "importAuthority": {"known": True, "frames": 12, "source": "actual-import", "reason": "accepted",
                    "evidenceTrust": "authoritative", "evidenceSource": "actual-import", "sampleDomain": "presentation",
                    "validation": "validated"} if success else None, "error": "" if success else failure_log(version)})
        reports.append({"schemaVersion": 1, "complete": True, "version": version, "caseCount": 70,
            "sourceHead": "b" * 40, "sourceSha256": "c" * 64, "files": rows})
    return baseline, reports


class ComparatorControls(unittest.TestCase):
    def invoke(self, reports, baseline=None, powershell=False, export_mode=False):
        baseline = fixtures()[0] if baseline is None else baseline
        with tempfile.TemporaryDirectory(prefix="comparison-") as folder:
            root = Path(folder)
            for name, value in zip(("baseline", "stable", "candidate"), (baseline, *reports)):
                (root / f"{name}.json").write_text(json.dumps(value), encoding="utf-8")
            if powershell:
                command = ["pwsh", "-NoProfile", "-File", str(HERE / "TestExportRuntime.ps1"),
                    "-OutputRoot", str(root / "result"), "-StableImportReport", str(root / "stable.json"),
                    "-CandidateImportReport", str(root / "candidate.json"), "-ImportBaseline", str(root / "baseline.json")]
                command += ["-CandidateRoot", str(root / "missing-candidate")] if export_mode else ["-CompareImportsOnly"]
            else:
                command = [sys.executable, str(HERE / "compare_import_reports.py"), "--baseline", str(root / "baseline.json"),
                    "--stable-report", str(root / "stable.json"), "--candidate-report", str(root / "candidate.json"),
                    "--output", str(root / "comparison.json")]
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
            output = root / ("result/import-comparison.json" if powershell else "comparison.json")
            return result, json.loads(output.read_text(encoding="utf-8")) if output.exists() else None

    def test_valid_and_expected_unsupported(self):
        result, report = self.invoke(fixtures()[1])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report["caseCount"], 70)
        self.assertEqual(report["matchingFailures"], list(range(63, 70)))
        self.assertEqual(report["expectedUnsupported"], list(range(63, 70)))
        self.assertEqual(report["unexpectedStableFailures"], [])
        self.assertFalse(report["allFormatsSupported"])

    def test_failure_cause_only_drift_rejected_on_both_sides_and_public_entry(self):
        # Removing reason comparison must let these same-status, same-exit failures pass incorrectly.
        for side in (0, 1):
            for cause in ("Permission denied (errno 13)", "Input/output error (errno 5)",
                    "Invalid data found when processing input (errno 99)"):
                for powershell, export_mode in ((False, False), (True, False), (True, True)):
                    with self.subTest(side=side, cause=cause, powershell=powershell, export_mode=export_mode):
                        reports = fixtures()[1]
                        reports[side]["files"][62]["error"] = failure_log(reports[side]["version"], cause)
                        result, report = self.invoke(reports, powershell=powershell, export_mode=export_mode)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("IMPORT_COMPARISON_FAILED", result.stderr)
                        self.assertIsNone(report)

    def test_matching_permission_failure_is_not_historically_inferred_unsupported(self):
        reports = fixtures()[1]
        for report in reports:
            report["files"][62]["error"] = failure_log(report["version"], "Permission denied (errno 13)")
        result, report = self.invoke(reports)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(63, report["expectedUnsupported"])
        self.assertEqual(report["otherMatchingFailures"], [63])
        self.assertFalse(report["allFormatsSupported"])
        self.assertFalse(report["releaseQualified"])
        outcome = report["failureOutcomes"][0]
        self.assertIn("Permission denied (errno 13)", outcome["failureReason"])
        self.assertEqual(outcome["stableError"], reports[0]["files"][62]["error"])
        self.assertEqual(outcome["candidateError"], reports[1]["files"][62]["error"])

    def test_failure_diagnostic_paths_and_errno_not_scrubbed(self):
        for side in (0, 1):
            for original, changed in (("Permission denied (errno 13)", "Permission denied (errno 5)"),
                    ("Cannot read D:/input/a.bin (errno 5)", "Cannot read D:/input/b.bin (errno 5)")):
                with self.subTest(side=side, original=original):
                    reports = fixtures()[1]
                    for report in reports:
                        report["files"][62]["error"] = failure_log(report["version"], original)
                    reports[side]["files"][62]["error"] = failure_log(reports[side]["version"], changed)
                    result, report = self.invoke(reports)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("failure reason differs", result.stderr)
                    self.assertIsNone(report)

    def test_invalid_reports_fail_closed(self):
        mutations = {
            "missing": lambda r: r["files"].pop(), "duplicate": lambda r: r["files"].append(copy.deepcopy(r["files"][0])),
            "wrong-id": lambda r: r["files"][0].update(index=99), "wrong-name": lambda r: r["files"][0].update(fileName="changed.wav"),
            "media-hash": lambda r: r["files"][0].update(mediaSha256="d" * 64),
            "media-size": lambda r: r["files"][0].update(mediaBytes=101),
            "success": lambda r: r["files"][0].update(succeeded=False),
            "exit": lambda r: r["files"][0].update(returnCode=2), "probe-exit": lambda r: r["files"][0].update(probeReturnCode=2),
            "frames": lambda r: r["files"][0].update(frames=13), "rate": lambda r: r["files"][0].update(sampleRate=44100),
            "channels": lambda r: r["files"][0].update(channels=1),
            "authority": lambda r: r["files"][0]["probeAuthority"].update(decodedSampleFramesSource="changed"),
            "import-authority": lambda r: r["files"][0]["importAuthority"].update(source="changed"),
            "string-bool": lambda r: r["files"][0].update(succeeded="false"),
            "bool-int": lambda r: r["files"][0].update(frames=True), "float-int": lambda r: r["files"][0].update(frames=12.0),
            "missing-field": lambda r: r["files"][0].pop("probeAuthority"),
            "missing-authority": lambda r: r["files"][0]["probeAuthority"].pop("decodedSampleFramesTrust"),
            "missing-import-authority": lambda r: r["files"][0].update(importAuthority=None),
            "partial": lambda r: r.update(complete=False), "wrong-count": lambda r: r.update(caseCount=69),
            "foreign-source": lambda r: r.update(sourceSha256="d" * 64),
        }
        for side in (0, 1):
            for name, mutate in mutations.items():
                with self.subTest(side=side, mutation=name):
                    reports = fixtures()[1]; mutate(reports[side])
                    result, _ = self.invoke(reports)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("IMPORT_COMPARISON_FAILED", result.stderr)

    def test_matching_unexpected_failure_is_parity_not_support(self):
        baseline, reports = fixtures()
        for report in reports:
            failure = copy.deepcopy(report["files"][-2]); failure.update(index=1, fileName=report["files"][0]["fileName"])
            report["files"][0] = failure
        result, report = self.invoke(reports, baseline)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report["unexpectedStableFailures"], [1])
        self.assertFalse(report["allHistoricallySupportedSucceeded"])

    def test_normal_powershell_entry_point(self):
        reports = fixtures()[1]
        result, report = self.invoke(reports, powershell=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report["status"], "PASS_PAIRWISE_IMPORT_PARITY")
        reports[1]["files"].pop()
        result, _ = self.invoke(reports, powershell=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("IMPORT_COMPARISON_FAILED", result.stderr)

    def test_runner_inventory_sidecars_and_ambiguous_media(self):
        from run_import_compatibility import inventory
        from compare_import_reports import baseline_rows
        expected = baseline_rows(fixtures()[0])
        with tempfile.TemporaryDirectory(prefix="inventory-") as folder:
            root = Path(folder)
            for row in expected.values():
                (root / row["fileName"]).write_bytes(b"owned fixture")
            (root / "01_sample.pk").write_bytes(b"owned sidecar")
            (root / "01_sample.pkf").write_bytes(b"owned sidecar")
            self.assertEqual(set(inventory(root, expected)), set(IDS))
            extra = root / "01_ambiguous.mp3"; extra.write_bytes(b"owned extra")
            with self.assertRaises(ValueError): inventory(root, expected)
            extra.unlink()
            missing = root / expected[1]["fileName"]; missing.unlink()
            with self.assertRaises(ValueError): inventory(root, expected)

    def test_export_entry_rejects_invalid_import_before_runtime_work(self):
        reports = fixtures()[1]; reports[0]["files"].append(copy.deepcopy(reports[0]["files"][0]))
        result, _ = self.invoke(reports, powershell=True, export_mode=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("IMPORT_COMPARISON_FAILED", result.stderr)

    def test_malformed_baseline_rejected(self):
        result, _ = self.invoke(fixtures()[1], baseline=[])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("IMPORT_COMPARISON_FAILED", result.stderr)


if __name__ == "__main__":
    unittest.main()
