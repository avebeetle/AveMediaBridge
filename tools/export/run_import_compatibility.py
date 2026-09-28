"""Serial read-only-corpus import compatibility against two pinned private runtimes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from compare_import_reports import (CASE_IDS, SPECIAL_NAME, PROBE_TYPES, IMPORT_TYPES, baseline_rows,
    compare, fields, load_json, require)

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def save(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)


def inventory(root, baseline):
    selected = {}
    for path in root.iterdir():
        if not path.is_file():
            continue
        # Adobe/Cool Edit peak sidecars are reference-only nonmedia, even when numbered.
        if path.suffix.lower() in (".pk", ".pkf"):
            continue
        match = re.match(r"^(\d{2})_", path.name)
        if not match and path.name != SPECIAL_NAME:
            continue  # unrelated top-level assets are hashed but are not members of this 70-case contract
        index = int(match.group(1)) if match else 7006
        require(index in CASE_IDS and index not in selected, f"ambiguous/foreign media case {index}: {path.name}")
        require(path.name == baseline[index]["fileName"], f"intended case filename mismatch: {index}")
        require(not path.is_symlink() and path.resolve().parent == root.resolve(), "redirected corpus media refused")
        selected[index] = path
    require(set(selected) == CASE_IDS, "corpus inventory must contain exactly intended IDs 1..69 and 7006")
    return selected


def corpus_snapshot(root, baseline):
    # Include ignored sidecars and unrelated top-level assets, not only selected inputs.
    result = {}
    for path in sorted(root.iterdir()):
        if path.is_file():
            require(not path.is_symlink() and path.resolve().parent == root.resolve(), "redirected corpus file refused")
            result[path.name] = {"bytes": path.stat().st_size, "sha256": digest(path)}
    result["historical-baseline"] = {"bytes": baseline.stat().st_size, "sha256": digest(baseline)}
    return result


def git(*args):
    return subprocess.check_output(["git", "-C", str(REPO), *args], text=True, encoding="utf-8").strip()


def source_identity():
    paths = ("src", "include", "app", "cmake", "CMakeLists.txt")
    require(not git("diff", "--name-only", "HEAD", "--", *paths), "dirty production/build source refused")
    require(not git("ls-files", "--others", "--exclude-standard", "--", *paths), "untracked production/build source refused")
    result = hashlib.sha256()
    entries = []
    for name in sorted(git("ls-files", "--", *paths).splitlines()):
        sha = digest(REPO / name); entries.append({"path": name, "sha256": sha})
        result.update((name + "\0" + sha + "\n").encode("utf-8"))
    return {"sourceHead": git("rev-parse", "HEAD"), "sourceSha256": result.hexdigest(), "files": entries}


def cache_value(build, key):
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith(key + ":"):
            return line.split("=", 1)[1]
    raise ValueError(f"missing CMake cache field: {key}")


def build_identity(build, expected_hashes, label, output):
    require(Path(cache_value(build, "CMAKE_HOME_DIRECTORY")).resolve() == REPO, "foreign build source tree")
    install = Path(cache_value(build, "AVEMEDIABRIDGE_FFMPEG_ROOT")).resolve()
    require(set(expected_hashes) == {"avcodec-61.dll", "avformat-61.dll", "avutil-59.dll", "swresample-5.dll"}, "four runtime pins required")
    # Check install identity before rebuilding, then all two runtime layouts afterward.
    for name, expected in expected_hashes.items():
        require(digest(install / "bin" / name).lower() == expected.lower(), f"{label}: pinned install runtime mismatch {name}")
    with (output / f"{label}-build.log").open("xb") as log:
        result = subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--target", "AveMediaBridgeLabApp"],
            stdout=log, stderr=subprocess.STDOUT)
    require(result.returncode == 0, f"{label}: selected source build failed")
    release = build / "Release"
    binaries = {}
    for name in ("AveMediaBridgeLabApp.exe", "AveMediaBridge.dll"):
        binaries[name] = digest(release / name)
    copies = {}
    for directory in (release, release / "Lib" / "ffmpeg"):
        for name, expected in expected_hashes.items():
            sha = digest(directory / name)
            require(sha.lower() == expected.lower(), f"{label}: pinned adjacent runtime mismatch {directory / name}")
            copies[str(directory / name)] = sha
    return {"build": str(build), "install": str(install), "binaries": binaries,
        "expectedRuntime": expected_hashes, "observedRuntimeCopies": copies}


def child(command, directory, environment):
    start = time.monotonic()
    result = subprocess.run(command, cwd=directory, env=environment, capture_output=True,
        timeout=300, text=True, encoding="utf-8", errors="replace")
    return result, round(time.monotonic() - start, 3)


def observe(version, executable, index, media, source_hash, root):
    evidence = root / "cases" / version / str(index); evidence.mkdir(parents=True)
    scratch = root / "work" / version / str(index); scratch.mkdir(parents=True)
    environment = os.environ.copy()
    environment["AVEVOICE_DATA_ROOT"] = str(scratch / "data")
    environment["TEMP"] = environment["TMP"] = str(scratch / "temp")
    (scratch / "temp").mkdir(); (scratch / "data").mkdir()
    probe_path = evidence / "probe.json"
    probe, probe_seconds = child([str(executable), "smoke-dll-probe", str(media), str(probe_path)], executable.parent, environment)
    (evidence / "probe.log").write_text(probe.stdout + "\n" + probe.stderr, encoding="utf-8")
    require(probe_path.is_file(), f"case {index}: current probe JSON missing, including failure evidence")
    probe_document = load_json(probe_path)
    fields(probe_document, PROBE_TYPES, f"case {index}: observed probe authority")
    session = scratch / "session"
    imported, import_seconds = child([str(executable), "smoke-dll-import-session", str(media), str(session)], executable.parent, environment)
    (evidence / "import.log").write_text(imported.stdout + "\n" + imported.stderr, encoding="utf-8")
    info = load_json(session / "audio_info.json") if (session / "audio_info.json").is_file() else None
    metadata = load_json(session / "metadata.json") if (session / "metadata.json").is_file() else None
    for name in ("audio_info.json", "metadata.json"):
        if (session / name).is_file(): shutil.copyfile(session / name, evidence / name)
    succeeded = imported.returncode == 0
    authority = None
    if info is not None:
        fields(info, {"frames": int, "sampleRate": int, "channels": int}, f"case {index}: actual audio_info")
    if metadata is not None:
        require("presentationBudget" in metadata, f"case {index}: actual import authority absent")
        fields(metadata["presentationBudget"], IMPORT_TYPES, f"case {index}: actual import authority")
        authority = {key: metadata["presentationBudget"][key] for key in IMPORT_TYPES}
    if succeeded:
        require(info is not None and authority is not None, f"case {index}: successful import missing actual artifacts")
        require(info["sampleFormat"] == "float32" and info["sampleLayout"] == "interleaved" and info["dataFile"] == "original_f32.bin",
            f"case {index}: unexpected PCM format/layout")
        require((session / "original_f32.bin").stat().st_size == info["frames"] * info["channels"] * 4,
            f"case {index}: final PCM byte/frame extent mismatch")
    row = {"index": index, "fileName": media.name, "mediaBytes": source_hash["bytes"], "mediaSha256": source_hash["sha256"],
        "succeeded": succeeded, "returnCode": imported.returncode, "probeReturnCode": probe.returncode,
        "frames": info["frames"] if info else 0, "sampleRate": info["sampleRate"] if info else 0, "channels": info["channels"] if info else 0,
        "probeAuthority": {key: probe_document[key] for key in PROBE_TYPES}, "importAuthority": authority,
        "error": "" if succeeded else (imported.stdout + "\n" + imported.stderr).strip(),
        "probeSeconds": probe_seconds, "importSeconds": import_seconds}
    save(evidence / "observation.json", row)
    # Only this freshly created owned case is removed; no corpus or historical path is eligible.
    require(scratch.resolve() == scratch.absolute() and scratch.resolve().is_relative_to((root / "work").resolve()), "unsafe scratch cleanup target")
    shutil.rmtree(scratch)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--stable-build", type=Path, required=True)
    parser.add_argument("--candidate-build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    try:
        corpus = args.corpus.resolve(strict=True); baseline_path = args.baseline.resolve(strict=True)
        require(args.output.is_absolute() and not output.exists() and output.parent.is_dir(), "fresh absent absolute output root required")
        require(not output.resolve().is_relative_to(corpus) and not output.resolve().is_relative_to(REPO), "corpus/repository output target refused")
        baseline = baseline_rows(load_json(baseline_path)); cases = inventory(corpus, baseline)
        output.mkdir()
        source = source_identity(); save(output / "source-identity.json", source)
        stable_manifest = load_json(REPO / "third_party/ffmpeg/build-manifest/ffmpeg-7.1.4-matroska-codec-delay-backport.json")
        candidate_manifest = load_json(HERE / "export-runtime-profile.json")
        stable_pins = {entry["name"]: entry["sha256"] for entry in stable_manifest["runtimeDlls"]}
        builds = {}
        for name, build, pins in (("stable", args.stable_build.resolve(strict=True), stable_pins),
                ("candidate", args.candidate_build.resolve(strict=True), candidate_manifest["candidateDllSha256"])):
            builds[name] = build_identity(build, pins, name, output)
        require(source_identity() == source, "source changed during builds")
        save(output / "runtime-identities.json", builds)
        print("Hashing all top-level corpus files, sidecars and baseline before run", flush=True)
        before = corpus_snapshot(corpus, baseline_path); save(output / "corpus-before.json", before)
        reports = {}
        try:
            for version in ("stable", "candidate"):
                report = {"schemaVersion": 1, "complete": False, "version": version, "caseCount": 0,
                    "sourceHead": source["sourceHead"], "sourceSha256": source["sourceSha256"], "files": []}
                executable = Path(builds[version]["build"]) / "Release" / "AveMediaBridgeLabApp.exe"
                for ordinal, index in enumerate(sorted(CASE_IDS), 1):
                    print(f"[{version}] {ordinal}/70 {cases[index].name}", flush=True)
                    row = observe(version, executable, index, cases[index], before[cases[index].name], output)
                    report["files"].append(row)
                    with (output / f"{version}-observations.jsonl").open("a", encoding="utf-8") as stream:
                        stream.write(json.dumps(row, ensure_ascii=False) + "\n")
                report.update(complete=True, caseCount=len(report["files"]))
                save(output / f"{version}.json", report); reports[version] = report
        finally:
            print("Hashing corpus after run", flush=True)
            after = corpus_snapshot(corpus, baseline_path); save(output / "corpus-after.json", after)
            require(before == after, "corpus/sidecars/baseline changed during qualification")
        require(source_identity() == source, "source changed during corpus run")
        result = compare(reports["stable"], reports["candidate"], load_json(baseline_path))
        save(output / "comparison.json", result)
        print(json.dumps(result, ensure_ascii=True), flush=True)
        return 0
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"IMPORT_COMPATIBILITY_FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
