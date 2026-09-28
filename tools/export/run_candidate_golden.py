"""Run the pinned Contract 1.1.0 Golden suite with explicit candidate bindings.

This adapter rebinds paths and corrects reopen window selection, not historical
audio/marker predicates. Additional execution gates cannot convert a historical FAIL.
"""
import argparse
import copy
import ctypes
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import types

ROOT = Path("D:/rvc/c++/DragonianVoice")
HISTORICAL = ROOT / "AveMediaBridge_test_laboratory_v1_certification/tools/run_golden_suite.py"
MANIFEST = HISTORICAL.parent.parent / "contracts/1.1.0/GOLDEN_SUITE_MANIFEST.json"
DEEP = {"GOLDEN-06", "GOLDEN-10", "GOLDEN-28"}
RUNNER_SHA = "e6ccffc629732d181476fda360af698c41d70f98724545a068c346233bc4a72e"
MANIFEST_SHA = "83b7632b5c01f3691658d81d9285fb19b3e514ace630fb71da1c410df56dbaed"
CANDIDATE = ROOT / "ffmpeglab/research/audio_export_foundation_20260928/candidate-002/install"
RUNTIME = {
    "avcodec-61.dll": "befcc050c78990de3ea828abcc5ed27628bd8a84af9677fc08f4bf6f3a646802",
    "avformat-61.dll": "530d88de7d36a330cb3d0e24efc3d9ac41dfdb77eda63210b412e1319b46bbf7",
    "avutil-59.dll": "757f0a7c58f6b0bc30763dd25ccf68bf35d560173e425032f39c03b0cce0b38b",
    "swresample-5.dll": "343b16190a48adabdc2641062e06cabb25a03f1b4b25d3845b63ab8e08e476e1",
}
BRIDGE_TARGETS = ["AveMediaBridgeAudioPresentationSliceTests", "AveMediaBridgeAudioPresentationEvidenceScanTests",
                  "AveMediaBridgeMatroskaCodecDelaySkipTests", "AveMediaBridgeMp4Mp3LoadingReadyPresentationTests"]


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def verify_identity(path, expected):
    require(re.fullmatch("[0-9a-fA-F]{64}", expected) is not None, "invalid SHA256 pin")
    require(path.is_file() and digest(path) == expected.lower(), f"identity mismatch: {path}")


def validate_source(state, expected):
    require(re.fullmatch("[0-9a-f]{40}", expected) is not None, "exact commit required")
    require(state["head"] == expected and not state["statusShort"] and state["diffCheck"] == "PASS",
            "source must be clean at the selected exact commit")


def validate_manifest(original, derived, bridge, voice):
    require(derived == derive_manifest(original, bridge, voice), "derived manifest changed frozen oracle")


def validate_output(path, protected):
    require(path.is_absolute() and not path.exists(), "output must be absent and absolute")
    resolved = path.resolve()
    require(len(resolved.parts) > 1, "volume root is not an output")
    for item in protected:
        item = item.resolve()
        require(not resolved.is_relative_to(item) and not item.is_relative_to(resolved), "protected output location")


def validate_registration(data, name, expected):
    rows = [row for row in data.get("tests", []) if row.get("name") == name]
    require(len(rows) == 1 and bool(rows[0].get("command")), f"missing/duplicate concrete CTest command: {name}")
    actual = rows[0]["command"]
    require(len(actual) == len(expected), f"CTest argument count: {name}")
    require(Path(actual[0]).is_absolute() and Path(actual[0]).is_file(), f"missing CTest executable: {name}")
    for left, right in zip(actual, expected):
        require(Path(left).resolve() == Path(right).resolve() if Path(right).is_absolute() else left == right,
                f"wrong CTest binding: {name}: {left}")
    for prop in rows[0].get("properties", []):
        require(prop["name"] not in {"ENVIRONMENT", "ENVIRONMENT_MODIFICATION"}, f"CTest overrides isolation: {name}")
        require(not (prop["name"] == "DISABLED" and prop["value"]), f"disabled CTest: {name}")
    return rows[0]


def validate_real(rows, manifest):
    require([row.get("id") for row in rows] == [item["id"] for item in manifest["realFiles"]], "missing/duplicate/reordered real rows")
    for row in rows:
        bridge, voice = row.get("bridge", {}), row.get("voice", {})
        reopen = voice.get("reopen", {})
        require(row.get("passed") is True and row.get("checks") and all(v is True for v in row["checks"].values()), f"historical real oracle failed: {row['id']}")
        require(bridge.get("probeResult") == 0 and bridge.get("importResult") == 0, f"Bridge execution failed: {row['id']}")
        require(voice.get("fullOverviewExitCode") == 0 and voice.get("fullOverviewMarkerFound") is True
                and voice.get("workflowMarkerFound") is True, f"full overview execution failed: {row['id']}")
        require(reopen.get("exitCode") == 0 and reopen.get("timedOut") is False and reopen.get("markerFound") is True,
                f"reopen execution failed: {row['id']}")
        if row["id"] in DEEP:
            require(voice.get("deepRight", {}).get("exitCode") == 0 and voice["deepRight"].get("markerFound") is True,
                    f"deep-right execution failed: {row['id']}")


def validate_synthetic(rows, executions, manifest):
    names = manifest["syntheticTests"]
    require([row.get("testName") for row in rows] == names and [row.get("name") for row in executions] == names,
            "missing/duplicate/reordered synthetic evidence")
    for row, execution in zip(rows, executions):
        require(row.get("passed") is True and row.get("exitCode") == 0, f"historical synthetic oracle failed: {row['testName']}")
        text = execution["output"]
        require(execution["exitCode"] == 0 and execution["timedOut"] is False
                and re.search(r"1/1 Test\s+#\d+: " + re.escape(row["testName"]) + r"\s+\.+\s+Passed\b", text)
                and "100% tests passed, 0 tests failed out of 1" in text
                and not re.search(r"Skipped|Not Run|No tests were found", text), f"synthetic not actually passed: {row['testName']}")

def derive_manifest(original, bridge, voice):
    value = copy.deepcopy(original)
    value.update(bridgeGoldenHead=bridge, voiceGoldenHead=voice)
    return value
def adapt_source(source):
    require(hashlib.sha256(source).hexdigest() == RUNNER_SHA, "historical runner hash drift")
    text = source.decode("utf-8")
    # Each replacement is a path expression. No predicate/marker/function body is otherwise changed.
    for old, new, count in [('BRIDGE / "build"', 'BRIDGE_BUILD', 4), ('VOICE / "build"', 'VOICE_BUILD', 5),
                            ('VOICE_BUILD / "Release" / "AveVoiceTests.exe"', 'VOICE_EXE', 1),
                            ('BRIDGE_BUILD / "Release" / "AveMediaBridge.dll"', 'BRIDGE_DLL', 2)]:
        require(text.count(old) == count, f"historical binding site drift: {old}")
        text = text.replace(old, new)
    return text


def cache_value(build, key):
    lines = (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines()
    values = [line.split("=", 1)[1] for line in lines if line.startswith(key + ":")]
    require(len(values) == 1, f"missing/ambiguous cache binding: {key}")
    return values[0]


def commands(manifest, bridge_build, voice_exe):
    real = ROOT / "AveMediaBridge_tests_40min_speech"
    bridge = [[str(bridge_build / "Release" / (target + ".exe"))] for target in BRIDGE_TARGETS]
    bridge[2] += [str(real / "10_mkv_h264_aac_stereo_48000.mkv"),
                  str(real / "_source/speech_40min_master_48000_stereo_f32.wav"), "115200000", "1024", "115201008"]
    bridge[3] += [str(real / "28_mp4_h264_mp3_stereo_44100.mp4")]
    voice = [[str(voice_exe), *args.split()] for args in ["workflow progressive-coordinator",
             "waveform provisional-right-tail-diagnostics", "waveform detail-source-handoff", "waveform monotonic-presentation-domain",
             "waveform source-frame-cap", "waveform viewport-source-activation", "waveform viewport-runtime-pump"]]
    require(len(manifest["syntheticTests"]) == 11, "frozen synthetic cardinality")
    return dict(zip(manifest["syntheticTests"], bridge + voice))


def reference_snapshot(module, manifest):
    paths = {module.media_path(item) for item in manifest["realFiles"]}
    paths.add(module.REAL / "_source/speech_40min_master_48000_stereo_f32.wav")
    # Hash all sidecars next to selected media, including existing non-numbered peak files.
    for parent in {path.parent for path in paths}:
        paths.update(path for path in parent.iterdir() if path.is_file() and path.suffix.lower() in {".pk", ".pkf"})
    paths.update(path for path in module.CONTRACT.rglob("*") if path.is_file())
    paths.add(HISTORICAL)
    return {str(path): {"bytes": path.stat().st_size, "sha256": digest(path)} for path in sorted(paths)}


def loaded_modules(bridge_dll):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetModuleHandleW.argtypes, kernel.GetModuleHandleW.restype = [ctypes.c_wchar_p], ctypes.c_void_p
    kernel.GetModuleFileNameW.argtypes, kernel.GetModuleFileNameW.restype = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32], ctypes.c_uint32
    result = {}
    for name, expected in {bridge_dll.name: digest(bridge_dll), **RUNTIME}.items():
        handle = kernel.GetModuleHandleW(name)
        require(bool(handle), f"direct C ABI runtime not loaded: {name}")
        buffer = ctypes.create_unicode_buffer(32768)
        require(0 < kernel.GetModuleFileNameW(handle, buffer, len(buffer)) < len(buffer), "loaded module path unavailable")
        path = Path(buffer.value)
        require(path.resolve() == (bridge_dll.parent / name).resolve(), f"wrong loaded module location: {path}")
        verify_identity(path, expected)
        result[name] = {"path": str(path), "sha256": digest(path)}
    return result


def select_reopen_viewports(pid, handles, describe, evidence):
    def eligible(value, hwnd):
        return (value is not None and value["hwnd"] == hwnd and value["pid"] == pid
                and value["windowClass"] == "AveVoiceWaveformViewportShellWindow" and value["visible"] is True)

    for hwnd in handles:
        if not eligible(describe(hwnd), hwnd):
            continue
        # Enumeration is stale-able: recheck ownership, class and visibility at return.
        current = describe(hwnd)
        if eligible(current, hwnd):
            evidence.append(current)
            return [hwnd]
    return []


def describe_window(hwnd):
    user = ctypes.WinDLL("user32")
    user.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    user.GetWindowThreadProcessId.restype = ctypes.c_uint32
    for name in ("GetClassNameW", "GetWindowTextW"):
        function = getattr(user, name)
        function.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
        function.restype = ctypes.c_int
    user.IsWindowVisible.argtypes, user.IsWindowVisible.restype = [ctypes.c_void_p], ctypes.c_int
    first_pid, last_pid = ctypes.c_uint32(), ctypes.c_uint32()
    first_thread = user.GetWindowThreadProcessId(hwnd, ctypes.byref(first_pid))
    kind, title = ctypes.create_unicode_buffer(512), ctypes.create_unicode_buffer(512)
    if not first_thread or not user.GetClassNameW(hwnd, kind, len(kind)):
        return None
    user.GetWindowTextW(hwnd, title, len(title))
    visible = bool(user.IsWindowVisible(hwnd))
    last_thread = user.GetWindowThreadProcessId(hwnd, ctypes.byref(last_pid))
    if not last_thread or (first_thread, first_pid.value) != (last_thread, last_pid.value):
        return None
    return {"hwnd": hwnd, "pid": last_pid.value, "windowClass": kind.value, "title": title.value, "visible": visible}


@contextmanager
def owned_reopen_windows(module, evidence):
    original = module.process_windows
    module.process_windows = lambda pid: select_reopen_viewports(pid, original(pid), describe_window, evidence)
    try:
        # Scope only the inherited reopen helper. Its 30s discovery/60s exit waits stay unchanged.
        # HWND validation and PostMessage cannot be atomic: an irreducible race remains after return.
        yield
    finally:
        module.process_windows = original


def candidate_metadata(historical, result, derived_sha, original_contract):
    return {**result, "manifestKind": "derived-candidate", "derivedManifestSha256": derived_sha,
            "originalContractReference": {"aggregateSha256": original_contract, "identity": "reference-only"},
            "historicalPredicateStatus": historical.get("status", "NOT_RUN"),
            "metadataPathsRelativeTo": "candidate-output-root",
            "historicalMetadataRawFile": "golden/HISTORICAL_RUN_METADATA.raw.json"}


def main():
    require(sys.version_info >= (3, 10), "frozen runner requires Python 3.10 or newer")
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("bridge-repo", "voice-repo", "bridge-build", "voice-build", "bridge-dll", "voice-exe", "runtime-root", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("bridge-head", "voice-head"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    protected = [ROOT, args.bridge_repo, args.voice_repo, args.bridge_build, args.voice_build, args.runtime_root]
    validate_output(args.output, protected)
    for key, value in vars(args).items():
        if isinstance(value, Path):
            require(value.is_absolute(), f"absolute selected path required: {key}")
    require(args.runtime_root.resolve() == CANDIDATE.resolve(), "not reviewed candidate runtime root")
    require(args.bridge_dll.resolve() == (args.bridge_build / "Release/AveMediaBridge.dll").resolve(), "normal selected Bridge target required")
    require(args.voice_exe.resolve() == (args.voice_build / "Release/AveVoiceTests.exe").resolve(), "selected Voice test target required")
    verify_identity(HISTORICAL, RUNNER_SHA)
    verify_identity(MANIFEST, MANIFEST_SHA)
    adapted = adapt_source(HISTORICAL.read_bytes())
    module = types.ModuleType("bound_historical_golden")
    exec(compile(adapted, str(HISTORICAL), "exec"), module.__dict__)
    original_runtime = module.EXPECTED_RUNTIME.copy()
    module.BRIDGE, module.VOICE = args.bridge_repo, args.voice_repo
    module.BRIDGE_BUILD, module.VOICE_BUILD = args.bridge_build, args.voice_build
    module.BRIDGE_DLL, module.VOICE_EXE = args.bridge_dll, args.voice_exe
    module.EXPECTED_RUNTIME = RUNTIME.copy()
    original = module.read_json(MANIFEST)
    derived = derive_manifest(original, args.bridge_head, args.voice_head)
    validate_manifest(original, derived, args.bridge_head, args.voice_head)
    for repo, head, build in [(args.bridge_repo, args.bridge_head, args.bridge_build), (args.voice_repo, args.voice_head, args.voice_build)]:
        validate_source(module.git_state(repo), head)
        require(Path(cache_value(build, "CMAKE_HOME_DIRECTORY")).resolve() == repo.resolve(), "wrong CMake source tree")
    require(Path(cache_value(args.bridge_build, "AVEMEDIABRIDGE_FFMPEG_ROOT")).resolve() == args.runtime_root.resolve(), "wrong build FFmpeg root")
    for name, expected in RUNTIME.items():
        verify_identity(args.runtime_root / "bin" / name, expected)
    args.output.mkdir()
    save = module.write_json
    save(args.output / "selected_arguments.json", {key: str(value) for key, value in vars(args).items()})
    (args.output / "bound_historical_runner.py").write_text(adapted, encoding="utf-8", newline="\n")
    module.MANIFEST = args.output / "derived_manifest.json"
    save(module.MANIFEST, derived)
    provenance = {"adapterSha256": digest(Path(__file__)), "historicalRunnerSha256": RUNNER_SHA,
                  "adaptedRunnerSha256": digest(args.output / "bound_historical_runner.py"),
                  "originalManifestSha256": MANIFEST_SHA, "derivedManifestSha256": digest(module.MANIFEST),
                  "originalContractHash": module.read_json(module.CONTRACT / "contract_hash.json")["aggregateSha256"],
                  "historicalRuntimePinsNotCandidateExpectations": original_runtime, "candidateRuntimePins": RUNTIME,
                  "oracleChange": "Frozen audio/case/marker predicates unchanged; derived manifest changes only two HEAD values",
                  "stagedSourceChange": "Exact guarded build/DLL/EXE path expression replacements only",
                  "reopenOrchestration": "Adapter filters inherited process_windows to rechecked owned-PID visible AveVoiceWaveformViewportShellWindow; discovery30s/exit60s and success predicates unchanged",
                  "childRuntimeLoadObservation": "NOT_OBSERVED; staged files pinned, not proof of child load", "builds": []}
    save(args.output / "provenance.json", provenance)
    # Only the adapter process environment is changed; child selectors cannot escape this owned run.
    for name in list(os.environ):
        if name.startswith(("AVEVOICE_", "AVEMEDIABRIDGE_", "CTEST_")):
            del os.environ[name]
    temp = args.output / "tmp"
    temp.mkdir()
    os.environ.update(TEMP=str(temp), TMP=str(temp), AVEVOICE_DATA_ROOT=str(args.output / "parent-data"))
    os.environ["PATH"] = str(args.bridge_dll.parent) + os.pathsep + os.environ.get("PATH", "")
    before = reference_snapshot(module, original)
    save(args.output / "inputs_before.json", before)
    result = {"status": "RUNNING", "releaseQualified": False, "manualNativeAcceptance": "NOT_VERIFIED"}
    historical_metadata = {}
    candidate_metadata_path = args.output / "golden/RUN_METADATA.json"

    def bound_write_json(path, value):
        nonlocal historical_metadata
        if path == candidate_metadata_path:
            historical_metadata = copy.deepcopy(value)
            save(args.output / "golden/HISTORICAL_RUN_METADATA.raw.json", value)
            save(path, candidate_metadata(value, result, provenance["derivedManifestSha256"], provenance["originalContractHash"]))
        else:
            save(path, value)

    module.write_json = bound_write_json
    executions = []
    binaries = {}
    try:
        for build, targets in [(args.bridge_build, ["AveMediaBridge", *BRIDGE_TARGETS]), (args.voice_build, ["AveVoiceTests"])]:
            command = ["cmake", "--build", str(build), "--config", "Release", "--target", *targets]
            record = module.run(command, build, timeout=900)
            provenance["builds"].append(record)
            save(args.output / "provenance.json", provenance)
            require(record["exitCode"] == 0 and not record["timedOut"], f"selected build failed: {build}")
        for repo, head in [(args.bridge_repo, args.bridge_head), (args.voice_repo, args.voice_head)]:
            validate_source(module.git_state(repo), head)
        # Staging is confined to the explicitly selected isolated Voice build.
        deployed = args.voice_exe.parent / "Modules/AveMediaBridge.dll"
        deployed.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.bridge_dll, deployed)
        runtime_dir = args.voice_exe.parent / "Lib/ffmpeg"
        runtime_dir.mkdir(parents=True, exist_ok=True)
        for name in RUNTIME:
            shutil.copy2(args.runtime_root / "bin" / name, runtime_dir / name)
        runtime = module.runtime_state()
        save(args.output / "runtime_preflight.json", runtime)
        require(runtime["allExpectedFfmpegHashesMatch"] and runtime["deployedBridge"]["matchesBridgeBuild"], "selected runtime/Bridge staging mismatch")
        expected_commands = commands(original, args.bridge_build, args.voice_exe)
        registrations = {}
        for owner, build in [("bridge", args.bridge_build), ("voice", args.voice_build)]:
            record = module.run(["ctest", "--test-dir", str(build), "-C", "Release", "--show-only=json-v1"], build)
            save(args.output / (owner + "_ctest_discovery.json"), record)
            require(record["exitCode"] == 0, "CTest discovery failed")
            data = json.loads(record["output"])
            for name, command in expected_commands.items():
                if name.startswith("AveMediaBridge") == (owner == "bridge"):
                    registrations[name] = validate_registration(data, name, command)
        save(args.output / "registrations.json", registrations)
        binaries = {str(path): digest(path) for path in {args.bridge_dll, deployed, args.voice_exe,
                    *(Path(command[0]) for command in expected_commands.values()),
                    *(args.bridge_dll.parent / name for name in RUNTIME), *(runtime_dir / name for name in RUNTIME)}}
        save(args.output / "binaries_before.json", binaries)
        original_run = module.run

        def isolated_run(command, cwd, env=None, timeout=300):
            number = len(executions) + 1
            child = args.output / "children" / f"{number:02d}"
            child.mkdir(parents=True)
            child_env = os.environ.copy() if env is None else env.copy()
            child_env.update(TEMP=str(child), TMP=str(child))
            if env is None:
                child_env["AVEVOICE_DATA_ROOT"] = str(child / "data")
            record = original_run(command, cwd, child_env, timeout)
            record["environment"] = {key: child_env[key] for key in ("TEMP", "TMP", "AVEVOICE_DATA_ROOT")}
            if command[0] == "ctest":
                pattern = command[command.index("-R") + 1]
                matches = [name for name in original["syntheticTests"] if pattern == "^" + re.escape(name) + "$"]
                require(len(matches) == 1, "unexpected synthetic selector")
                record["name"] = matches[0]
            executions.append(record)
            save(args.output / "executions.json", executions)
            return record

        module.run = isolated_run
        original_import = module.bridge_import

        def bound_import(dll, item, work):
            save(args.output / "direct_loaded_modules.json", loaded_modules(args.bridge_dll))
            return original_import(dll, item, work)

        module.bridge_import = bound_import
        original_reopen = module.reopen_current_session
        reopen_observations = []

        def isolated_reopen(exe, data_root, log_path):
            child = args.output / "reopen-temp" / f"{len(reopen_observations) + 1:02d}"
            child.mkdir(parents=True)
            old_temp, old_tmp = os.environ["TEMP"], os.environ["TMP"]
            os.environ.update(TEMP=str(child), TMP=str(child))
            selected_windows = []
            try:
                with owned_reopen_windows(module, selected_windows):
                    observation = original_reopen(exe, data_root, log_path)
            finally:
                os.environ.update(TEMP=old_temp, TMP=old_tmp)
            reopen_observations.append({"exe": str(exe), "dataRoot": str(data_root), "temp": str(child),
                                        "selectedWindows": selected_windows, **observation})
            save(args.output / "reopen_executions.json", reopen_observations)
            return observation

        module.reopen_current_session = isolated_reopen
        # Keep the DLL-directory handle alive throughout the historical direct C ABI calls.
        with os.add_dll_directory(str(args.bridge_dll.parent)):
            previous_argv = sys.argv
            try:
                sys.argv = [str(HISTORICAL), "--run-dir", str(args.output / "golden")]
                historical_exit = module.main()
            finally:
                sys.argv = previous_argv
        result["historicalExitCode"] = historical_exit
        golden = args.output / "golden"
        rows = lambda name: [json.loads(line) for line in (golden / name).read_text(encoding="utf-8").splitlines() if line]
        real, synthetic = rows("real_file_results.jsonl"), rows("synthetic_test_results.jsonl")
        result["historicalSummary"] = module.read_json(golden / "summary.json")
        errors = []
        for validate, parameters in [(validate_real, (real, original)),
                                      (validate_synthetic, (synthetic, [e for e in executions if "name" in e], original))]:
            try:
                validate(*parameters)
            except ValueError as error:
                errors.append(str(error))
        require(historical_exit == 0 and not errors, "Golden failed: " + "; ".join(errors))
        for path, expected in binaries.items():
            verify_identity(Path(path), expected)
        for repo, head in [(args.bridge_repo, args.bridge_head), (args.voice_repo, args.voice_head)]:
            validate_source(module.git_state(repo), head)
        result["status"] = "PASS_CURRENT_GOLDEN"
    except Exception as error:
        result.update(status="FAILED_GATE", error=f"{type(error).__name__}: {error}")
    finally:
        final_binaries = {path: digest(Path(path)) if Path(path).is_file() else None for path in binaries}
        save(args.output / "binaries_after.json", final_binaries)
        result["selectedBinariesUnchanged"] = bool(binaries) and final_binaries == binaries
        if binaries and final_binaries != binaries:
            result.update(status="FAILED_GATE", error="selected binary changed during run")
        after = reference_snapshot(module, original)
        save(args.output / "inputs_after.json", after)
        result["referenceInputsUnchanged"] = before == after
        if before != after:
            result.update(status="FAILED_GATE", error="reference input/sidecar mutation")
        result = candidate_metadata(historical_metadata, result, provenance["derivedManifestSha256"], provenance["originalContractHash"])
        if historical_metadata:
            save(candidate_metadata_path, result)
        save(args.output / "candidate_result.json", result)
    print(json.dumps(result, sort_keys=True), flush=True)
    return 0 if result["status"] == "PASS_CURRENT_GOLDEN" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as error:
        print(f"FAILED_PREFLIGHT: {error}", file=sys.stderr)
        raise SystemExit(2)
