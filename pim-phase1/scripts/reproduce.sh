#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:---smoke}"
if [[ "$MODE" != "--smoke" && "$MODE" != "--paper" ]]; then
  echo "usage: $0 [--smoke|--paper]" >&2
  exit 2
fi

cd "$ROOT"
(cd simulator && scons NO_STORAGE=0 NO_LIBRARY=1)

.venv/bin/python scripts/calibrate_g8.py
.venv/bin/python scripts/expand_suite.py --suite configs/suite_paper.yaml

if [[ "$MODE" == "--paper" ]]; then
  .venv/bin/python scripts/run_suite.py \
    --manifest configs/generated/paper-manifest.json --jobs 8
else
  .venv/bin/python - <<'PY'
import json
from pathlib import Path
root = Path('.').resolve()
manifest = json.loads((root / 'configs/generated/paper-manifest.json').read_text())
selected = []
seen = set()
for case in manifest['cases']:
    key = case['group']
    if key != 'P1-E' and key not in seen:
        selected.append(case)
        seen.add(key)
    if key == 'P1-E' and case['case'].get('kind') in ('positive', 'status_negative'):
        subkey = 'P1-E-' + case['case']['kind']
        if subkey not in seen:
            selected.append(case)
            seen.add(subkey)
smoke = dict(manifest)
smoke['suite'] = 'smoke-selected'
smoke['expected_total'] = len(selected)
smoke['expected_negative'] = sum(1 for case in selected if case['expected_negative'])
smoke['expected_by_group'] = {}
for case in selected:
    smoke['expected_by_group'][case['group']] = smoke['expected_by_group'].get(case['group'], 0) + 1
smoke['cases'] = selected
out = root / 'configs/generated/smoke-selected-manifest.json'
out.write_text(json.dumps(smoke, indent=2) + '\n')
print(f"wrote {out} with {len(selected)} cases")
PY
  .venv/bin/python scripts/run_suite.py \
    --manifest configs/generated/smoke-selected-manifest.json --jobs 1
fi

.venv/bin/python scripts/aggregate.py --runs runs/paper --out results
.venv/bin/python scripts/plot_paper.py --results results --out figures
.venv/bin/python scripts/package_artifact.py
