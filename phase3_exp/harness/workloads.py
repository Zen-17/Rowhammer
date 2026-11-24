"""Expand locked workload JSON into the text job file the simulator reads."""
from __future__ import annotations

import json
from pathlib import Path

from .paths import WORKLOADS


def load_workloads() -> list:
    order = ["W1", "W2", "W4", "GEMV", "DEC"]
    jobs = []
    for name in order:
        jobs.append(json.loads((WORKLOADS / f"{name}.json").read_text()))
    return jobs


def _kernel_line(spec: dict) -> str:
    return f"kernel {spec['op']} {spec['in0']} {spec['result']} {spec['in1']}"


def expand_job(job: dict) -> str:
    lines = [f"begin {job['name']} {job['kind']}"]
    if "n_tiles" in job:
        lines.append(f"n_tiles {job['n_tiles']}")
    lines.append(f"host_bw_share {job['host_bw_share']}")
    kind = job["kind"]
    if kind == "eltwise":
        for spec in job["kernels"]:
            lines.append(_kernel_line(spec))
    elif kind == "fanout":
        if int(job["fanout"]) != 16:
            raise ValueError("W4 fan-out is locked at 16")
        lines.append(_kernel_line(job["add"]))
        stride = int(job["row_stride"])
        base = int(job["mul_base_row"])
        result0 = base + 16 * stride
        for index in range(16):
            in1 = base + index * stride
            result = result0 + index * stride
            lines.append(f"kernel MUL 256 {result} {in1}")
            relu_result = result0 + 16 * stride + index * stride
            lines.append(f"kernel RELU {result} {relu_result} 0")
    elif kind == "gemv":
        lines.append(f"weight {job['weight_shape'][0]} {job['weight_shape'][1]}")
        lines.append(f"input {job['input_shape'][0]} {job['input_shape'][1]}")
    elif kind == "dec":
        if int(job["gemv_count"]) != 3:
            raise ValueError("DEC is locked at 3 GEMV steps")
        lines.append(f"gemv_count {job['gemv_count']}")
        lines.append(f"weight {job['weight_shape'][0]} {job['weight_shape'][1]}")
        lines.append(f"input {job['input_shape'][0]} {job['input_shape'][1]}")
        lines.append(_kernel_line(job["residual"]))
        lines.append(_kernel_line(job["relu"]))
    else:
        raise ValueError(f"unknown workload kind {kind}")
    lines.append("end")
    return "\n".join(lines)


def write_jobs(path: Path) -> str:
    text = "\n".join(expand_job(job) for job in load_workloads()) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    w4 = expand_job(next(job for job in load_workloads() if job["name"] == "W4"))
    mul = w4.count("kernel MUL")
    if mul != 16:
        raise RuntimeError(f"expected 16 MUL kernels for W4, found {mul}")
    return text
