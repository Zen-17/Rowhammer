import copy
import json
from pathlib import Path

import jsonschema
import pytest

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = json.loads((ROOT / "configs/schema.json").read_text())

BASE = {
    "schema_version": 1,
    "experiment": "G2",
    "workload": "W1",
    "mode": "feeder",
    "trace_mode": "ext_on",
    "channels": 1,
    "ranks": 1,
    "capacity_mb": 256,
    "n_tiles": 1,
    "seed": 0,
    "feeder_outstanding_limit": 8,
    "feeder_issue_per_cycle": 1,
    "watchdog_cycles": 100000,
    "device_ini": "ini/HBM2_samsung_2M_16B_x64.ini",
    "system_ini": "configs/phase1_system_1ch.ini",
    "output_dir": "/tmp/phase1-run",
    "dim_bursts": 128,
    "elements": 2048,
}


def test_valid_config():
    jsonschema.validate(BASE, SCHEMA)


@pytest.mark.parametrize(
    "mutation",
    [
        lambda c: c.update({"unknown_key": 1}),
        lambda c: c.update({"schema_version": 2}),
        lambda c: c.update({"channels": 3}),
        lambda c: c.update({"ranks": 2}),
        lambda c: c.update({"trace_mode": "on"}),
        lambda c: c.update({"feeder_outstanding_limit": 7}),
        lambda c: c.update({"feeder_issue_per_cycle": 2}),
        lambda c: c.update({"watchdog_cycles": -1}),
        lambda c: c.pop("output_dir"),
    ],
)
def test_invalid_config_rejected(mutation):
    config = copy.deepcopy(BASE)
    mutation(config)
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(config, SCHEMA)
