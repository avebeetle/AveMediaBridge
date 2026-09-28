"""Strict 70-case stable/candidate parity. Historical classifications are informational."""
import argparse
import json
from pathlib import Path
import re
import sys

CASE_IDS = set(range(1, 70)) | {7006}
SPECIAL_NAME = "6. Практика (Сцена 3).mp4"
PROBE_TYPES = {"decodedSampleFrames": int, "decodedSampleFramesKind": str, "decodedSampleFramesTrust": str,
    "decodedSampleFramesSource": str, "mediaOpenAuthoritySource": str, "mediaOpenAuthorityTrust": str,
    "mediaOpenAuthorityDomain": str, "mediaOpenDisposition": str, "sampleRate": int, "channels": int}
IMPORT_TYPES = {"known": bool, "frames": int, "source": str, "reason": str, "evidenceTrust": str,
    "evidenceSource": str, "sampleDomain": str, "validation": str}
INVALID_INPUT_REASON = ("Smoke DLL import session\n\nResult: FAIL\n"
    "AveMediaBridge_ImportAudioToSession returned 2\n"
    "DLL last error: avformat_open_input failed: Invalid data found when processing input")


def require(value, message):
    if not value:
        raise ValueError(message)


def load_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding="utf-8-sig"), object_pairs_hook=unique)


def fields(value, schema, context):
    require(type(value) is dict, f"{context}: object required")
    for key, kind in schema.items():
        require(key in value and type(value[key]) is kind, f"{context}.{key}: exact {kind.__name__} required")
        if kind is int:
            require(0 <= value[key] <= (1 << 64) - 1, f"{context}.{key}: out of range")
        if kind is str:
            require(bool(value[key]), f"{context}.{key}: nonempty value required")


def by_id(rows, context):
    require(type(rows) is list and len(rows) == 70, f"{context}: exactly 70 rows required")
    result = {}
    for row in rows:
        fields(row, {"index": int, "fileName": str}, context)
        index = row["index"]
        require(index in CASE_IDS and index not in result, f"{context}: missing/duplicate/foreign case ID {index}")
        name = row["fileName"]
        require(Path(name).name == name and (name == SPECIAL_NAME if index == 7006 else name.startswith(f"{index:02}_")),
            f"{context}: invalid corresponding filename {index}")
        result[index] = row
    require(set(result) == CASE_IDS, f"{context}: incomplete case IDs")
    return result


def baseline_rows(payload):
    require(type(payload) is dict, "baseline: object required")
    result = by_id(payload.get("files"), "baseline")
    for row in result.values():
        fields(row, {"importSucceeded": bool, "finalWrittenFrames": int, "authorityClassification": str,
            "safetyClassification": str}, "historical baseline")
    return result


def report_rows(payload, version, baseline):
    fields(payload, {"schemaVersion": int, "complete": bool, "version": str, "caseCount": int,
        "sourceHead": str, "sourceSha256": str}, version)
    require(payload["schemaVersion"] == 1 and payload["complete"] is True and payload["version"] == version and
        payload["caseCount"] == 70, f"{version}: partial or wrong report")
    require(re.fullmatch(r"[0-9a-fA-F]{40}", payload["sourceHead"]) and
        re.fullmatch(r"[0-9a-fA-F]{64}", payload["sourceSha256"]), f"{version}: malformed source identity")
    rows = by_id(payload.get("files"), version)
    for index, row in rows.items():
        fields(row, {"mediaBytes": int, "mediaSha256": str, "succeeded": bool, "returnCode": int,
            "probeReturnCode": int, "frames": int, "sampleRate": int, "channels": int}, f"{version}/{index}")
        require(type(row.get("error")) is str, f"{version}/{index}: error text required")
        require(row["fileName"] == baseline[index]["fileName"], f"{version}/{index}: filename differs from intended case")
        require(re.fullmatch(r"[0-9a-fA-F]{64}", row["mediaSha256"]) and row["mediaBytes"] > 0,
            f"{version}/{index}: malformed input identity")
        require(row["succeeded"] == (row["returnCode"] == 0), f"{version}/{index}: success/exit contradiction")
        fields(row.get("probeAuthority"), PROBE_TYPES, f"{version}/{index}/current probe authority")
        require("importAuthority" in row, f"{version}/{index}: missing current import authority")
        if row["importAuthority"] is not None:
            fields(row["importAuthority"], IMPORT_TYPES, f"{version}/{index}/current import authority")
        if row["succeeded"]:
            require(row["frames"] > 0 and row["sampleRate"] > 0 and row["channels"] > 0 and
                row["probeReturnCode"] == 0 and row["importAuthority"] is not None,
                f"{version}/{index}: successful import lacks actual PCM/authority")
        else:
            require(bool(row["error"]), f"{version}/{index}: failed import lacks retained error")
    return rows


def failure_reason(error):
    lines = error.splitlines()
    # Only LabApp's two leading informational runtime paths vary between builds.
    # Never scrub paths, errno, or extra diagnostics from the actual failure body.
    if (len(lines) >= 3 and lines[0] == "Smoke DLL import session" and
            re.fullmatch(r"  DLL: [A-Za-z]:[\\/].*[\\/]AveMediaBridge\.dll", lines[1]) and
            re.fullmatch(r"  FFmpeg DLL dir: [A-Za-z]:[\\/].*[\\/]ffmpeg", lines[2])):
        lines = lines[:1] + lines[3:]
    return "\n".join(lines)


def compare(stable, candidate, historical):
    baseline = baseline_rows(historical)
    left = report_rows(stable, "stable", baseline); right = report_rows(candidate, "candidate", baseline)
    require(stable["sourceHead"] == candidate["sourceHead"] and stable["sourceSha256"] == candidate["sourceSha256"],
        "stable/candidate source identities differ")
    compared = ("fileName", "mediaBytes", "mediaSha256", "succeeded", "returnCode", "probeReturnCode", "frames",
        "sampleRate", "channels", "probeAuthority", "importAuthority")
    for index in sorted(CASE_IDS):
        for key in compared:
            require(left[index][key] == right[index][key], f"case {index}: stable/candidate {key} differs")
    failures = [i for i in sorted(CASE_IDS) if not left[i]["succeeded"]]
    reasons = {}
    for index in failures:
        reasons[index] = failure_reason(left[index]["error"])
        require(reasons[index] == failure_reason(right[index]["error"]),
            f"case {index}: stable/candidate failure reason differs")
    expected_unsupported = [i for i in failures if not baseline[i]["importSucceeded"] and
        reasons[i] == INVALID_INPUT_REASON]
    historical_changes = {}
    for name, rows in (("stable", left), ("candidate", right)):
        historical_changes[name] = {
            "successChanges": [i for i in sorted(CASE_IDS) if rows[i]["succeeded"] != baseline[i]["importSucceeded"]],
            "frameChanges": [{"index": i, "delta": rows[i]["frames"] - baseline[i]["finalWrittenFrames"]}
                for i in sorted(CASE_IDS) if rows[i]["succeeded"] and baseline[i]["importSucceeded"] and
                rows[i]["frames"] != baseline[i]["finalWrittenFrames"]]}
    unexpected = [i for i in failures if baseline[i]["importSucceeded"]]
    return {"status": "PASS_PAIRWISE_IMPORT_PARITY", "caseCount": 70, "succeeded": 70 - len(failures),
        "matchingFailures": failures, "expectedUnsupported": expected_unsupported,
        "otherMatchingFailures": [i for i in failures if i not in expected_unsupported],
        "unexpectedStableFailures": unexpected, "allFormatsSupported": not failures,
        "allHistoricallySupportedSucceeded": not unexpected, "historicalDeltasInformationalOnly": historical_changes,
        "failureOutcomes": [{"index": i, "fileName": left[i]["fileName"], "stableError": left[i]["error"],
            "candidateError": right[i]["error"], "returnCode": left[i]["returnCode"], "failureReason": reasons[i],
            "failureCategory": "invalid_input" if reasons[i] == INVALID_INPUT_REASON else "other"} for i in failures],
        "releaseQualified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stable-report", type=Path, required=True)
    parser.add_argument("--candidate-report", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        require(not args.output.exists(), "refusing existing comparison output")
        result = compare(load_json(args.stable_report), load_json(args.candidate_report), load_json(args.baseline))
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, ensure_ascii=False, indent=2)
        print(json.dumps(result, ensure_ascii=True))
        return 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"IMPORT_COMPARISON_FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
