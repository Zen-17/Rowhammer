"""Read-only interface freeze for M0.1.

The manifest records the symbols Phase 3 is allowed to call.  It also records
the two places where the simulator does not implement a playbook assumption.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path

from .paths import CONFIGS, P1, P2, REPO, load_json


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _find(path: Path, pattern: str) -> dict:
    text = path.read_text(errors="replace")
    match = re.search(pattern, text)
    if match is None:
        raise SystemExit(f"interface missing in {path}: {pattern}")
    line = text[: match.start()].count("\n") + 1
    return {"path": str(path.relative_to(REPO)), "line": line, "signature": match.group(0)}


def _ini_value(text: str, key: str) -> str:
    match = re.search(rf"^{key}=(\S+)", text, re.M)
    if match is None:
        raise SystemExit(f"ini missing {key}")
    return match.group(1)


def _git() -> str:
    try:
        return subprocess.check_output(["git", "-C", str(REPO), "rev-parse", "HEAD"], text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unavailable"


def build_manifest() -> dict:
    ini_path = P1 / "simulator/ini/HBM2_samsung_2M_16B_x64.ini"
    ini = ini_path.read_text()
    locked = load_json(CONFIGS / "locked_params.json")
    measured = {
        "NUM_BANKS": int(_ini_value(ini, "NUM_BANKS")),
        "NUM_ROWS": int(_ini_value(ini, "NUM_ROWS")),
        "tRC": int(_ini_value(ini, "tRC")),
        "tREFI": int(_ini_value(ini, "tREFI")),
        "tCK": int(_ini_value(ini, "tCK")),
    }
    expected = {
        "NUM_BANKS": locked["platform"]["num_banks"],
        "NUM_ROWS": locked["platform"]["num_rows"],
        "tRC": locked["platform"]["tRC_ns"],
        "tREFI": locked["platform"]["tREFI_ns"],
        "tCK": locked["platform"]["tCK_ns"],
    }
    if measured != expected:
        raise SystemExit(f"device ini does not match the locked platform: {measured} vs {expected}")
    rank_cpp = P1 / "simulator/src/Rank.cpp"
    interfaces = {
        "activate_and_ab_mode": _find(rank_cpp, r"void Rank::updateState\(BusPacket\* packet\)"),
        "bank_update": _find(rank_cpp, r"void Rank::updateBank\(BusPacketType type, int bank, int row, bool targetBank, bool targetBankgroup\)"),
        "refresh_command": _find(rank_cpp, r"case REF:"),
        "inject_data": _find(P1 / "simulator/src/Phase1Storage.h", r"void injectDataBit\(const Phase1StorageKey& key, unsigned bit\)"),
        "inject_parity": _find(P1 / "simulator/src/Phase1Storage.h", r"void injectParityBit\(const Phase1StorageKey& key, unsigned bit\)"),
        "report_queue": _find(P1 / "simulator/src/Phase1Reports.h", r"class Phase1Reports"),
        "b2_recovery": _find(P2 / "overlay/src/Phase2Engine.h", r"Decision analyze\(const Phase1PublicReport& report, uint64_t t_ack,"),
        "plot_input": _find(P1 / "scripts/plot_paper.py", r"def read_csv\(path: Path\)"),
    }
    return {
        "git_commit": _git(),
        "device_ini_sha256": _sha256(ini_path),
        "locked_params_sha256": _sha256(CONFIGS / "locked_params.json"),
        "ini_check": "PASS",
        "ini": measured,
        "interfaces": interfaces,
        "differences": [
            {
                "item": "refresh_row_pointer",
                "mark": "ASSUMED",
                "fact": "Rank::updateState on REF only advances nextActivate by tRFC for every bank. refreshCount increments in Rank::receiveFromBus. No row pointer exists.",
                "phase3": "ActivationObserver keeps a side pointer and retires two rows per observed REF. It does not change the controller.",
            },
            {
                "item": "W4_fanout",
                "mark": "ASSUMED",
                "fact": "The playbook names W4 as fan-out and does not lock the factor. Phase 2 measured W4_F16.",
                "phase3": "W4 is locked at fan-out 16.",
            },
        ],
    }


def write_manifest(path: Path | None = None) -> dict:
    payload = build_manifest()
    destination = path or (CONFIGS / "interface_manifest.json")
    destination.write_text(json.dumps(payload, indent=2) + "\n")
    return payload
