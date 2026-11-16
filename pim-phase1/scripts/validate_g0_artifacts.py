#!/usr/bin/env python3
"""Validate the immutable G0 baseline artifacts after G1 has modified the tree.

This does not rerun the G0 tests against the G1 source.  It verifies the saved
G0 binary, XML, configuration and environment evidence instead.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM = ROOT / "simulator"
COMMIT = "3703d1f19c8f027360cc33a3243eb271e3bb6898"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    lock = json.loads((ROOT / "environment/upstream.lock.json").read_text())
    checks = []

    def check(name, passed, detail):
        checks.append({"name": name, "pass": bool(passed), "detail": detail})

    head = subprocess.check_output(["git", "-C", str(SIM), "rev-parse", "HEAD"], text=True).strip()
    check("upstream_commit", head == COMMIT, head)

    g0_binary = ROOT / "runs/baseline/sim-g0"
    expected_binary = lock["build"]["binary"]["sha256"]
    check("g0_binary_preserved", g0_binary.exists() and sha256(g0_binary) == expected_binary,
          expected_binary)
    check("g0_binary_record", expected_binary in (ROOT / "environment/binary.sha256").read_text(),
          expected_binary)

    g0_xml = ROOT / "runs/baseline/upstream-tests.xml"
    check("g0_xml_hash", sha256(g0_xml) == lock["tests"]["required"]["sha256"],
          lock["tests"]["required"]["sha256"])
    root = ET.parse(g0_xml).getroot()
    check("g0_xml_counts", int(root.attrib["tests"]) == 5 and int(root.attrib["failures"]) == 0
          and int(root.attrib["errors"]) == 0, dict(root.attrib))

    optional = ROOT / "runs/baseline/upstream-all-tests.xml"
    opt = ET.parse(optional).getroot()
    failed = [tc.attrib.get("name") for tc in opt.iter("testcase") if tc.find("failure") is not None]
    check("g0_optional_failure_preserved",
          int(opt.attrib["tests"]) == 11 and failed == ["add"],
          {"tests": opt.attrib["tests"], "failures": opt.attrib["failures"],
           "failed_names": failed})

    for item in lock["original_configs"]:
        check(f"g0_config:{item['path']}", sha256(SIM / item["path"]) == item["sha256"],
              item["sha256"])

    env = json.loads((ROOT / "environment/environment.json").read_text())
    core = env["environment_hash_inputs"]
    recomputed = hashlib.sha256(json.dumps(core, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    check("g0_environment_hash", recomputed == env["environment_hash"], recomputed)

    ok = all(c["pass"] for c in checks)
    result = {"gate": "G0", "mode": "saved-artifacts", "pass": ok, "checks": checks}
    (ROOT / "environment/g0_artifact_validation.json").write_text(json.dumps(result, indent=2) + "\n")
    for c in checks:
        print(("PASS" if c["pass"] else "FAIL"), c["name"], c["detail"])
    print("G0_ARTIFACT_VALIDATION=" + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
