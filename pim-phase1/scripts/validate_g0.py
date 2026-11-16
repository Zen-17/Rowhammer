#!/usr/bin/env python3
"""Independent G0 acceptance checks over the saved artifacts.

Checks the fixed commit, clean worktree, config/binary/test hashes, storage
build mode and the recorded upstream performance-test anomaly.  It does not
modify simulator state.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM = ROOT / "simulator"
COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    checks: list[dict] = []

    def check(name: str, passed: bool, detail) -> None:
        checks.append({"name": name, "pass": bool(passed), "detail": detail})

    lock = json.loads((ROOT / "environment/upstream.lock.json").read_text())
    env = json.loads((ROOT / "environment/environment.json").read_text())

    head = subprocess.check_output(["git", "-C", str(SIM), "rev-parse", "HEAD"], text=True).strip()
    check("fixed_commit", head == COMMIT, {"actual": head, "expected": COMMIT})
    check("tree", lock["repository"]["git_tree"] ==
          subprocess.check_output(["git", "-C", str(SIM), "rev-parse", "HEAD^{tree}"], text=True).strip(),
          lock["repository"]["git_tree"])
    status = subprocess.check_output(["git", "-C", str(SIM), "status", "--porcelain"], text=True)
    check("worktree_clean", status == "", status or "empty")

    for item in lock["original_configs"]:
        actual = sha256(SIM / item["path"])
        check(f"config_hash:{item['path']}", actual == item["sha256"], actual)

    binary = lock["build"]["binary"]
    actual_bin = sha256(ROOT / binary["path"])
    check("binary_hash", actual_bin == binary["sha256"], actual_bin)
    check("binary_size", (ROOT / binary["path"]).stat().st_size == binary["bytes"], binary["bytes"])

    for log in lock["build"]["build_logs"]:
        check(f"build_log_hash:{log['path']}", sha256(ROOT / log["path"]) == log["sha256"], log["sha256"])
        text = (ROOT / log["path"]).read_text(errors="replace")
        check(f"no_no_storage_define:{log['path']}", "-DNO_STORAGE" not in text,
              "no -DNO_STORAGE compile option")

    required = lock["tests"]["required"]
    check("required_test_hash", sha256(ROOT / required["xml"]) == required["sha256"], required["sha256"])
    req_root = ET.parse(ROOT / required["xml"]).getroot()
    check("required_tests_all_pass",
          int(req_root.attrib["tests"]) == 5 and int(req_root.attrib["failures"]) == 0
          and int(req_root.attrib["errors"]) == 0,
          dict(req_root.attrib))

    optional = lock["tests"].get("all_original_optional")
    if optional:
        opt_root = ET.parse(ROOT / optional["xml"]).getroot()
        failures = [tc.attrib.get("name") for tc in opt_root.iter("testcase")
                    if tc.find("failure") is not None]
        check("optional_all_tests_recorded",
              int(opt_root.attrib["tests"]) == 11 and int(opt_root.attrib["failures"]) == 1
              and failures == ["add"],
              {"tests": opt_root.attrib["tests"], "failures": opt_root.attrib["failures"],
               "failed_names": failures})

    core = env["environment_hash_inputs"]
    recomputed = hashlib.sha256(json.dumps(core, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    check("environment_hash", recomputed == env["environment_hash"], recomputed)
    check("handoff_hash", sha256(ROOT / "HANDOFF.md") == lock["handoff"]["sha256"], lock["handoff"]["sha256"])
    check("source_patch_absent_for_g0", not (ROOT / "environment/source.patch").exists(),
          "no source changes at G0")
    check("storage_enabled", lock["build"]["storage_enabled"] is True
          and lock["build"]["no_storage_argument"] == 0
          and lock["build"]["no_storage_define_present"] is False,
          lock["build"]["command"])

    ok = all(c["pass"] for c in checks)
    result = {"gate": "G0", "pass": ok, "checks": checks}
    out = ROOT / "environment/g0_validation.json"
    out.write_text(json.dumps(result, indent=2) + "\n")
    for c in checks:
        print(("PASS" if c["pass"] else "FAIL"), c["name"], c["detail"])
    print(f"G0_VALIDATION={'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
