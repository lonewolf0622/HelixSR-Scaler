#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare a private D3D12 NR cache from a user-supplied DLL, without executing it."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
MODEL_SHA256 = "6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927"
REFERENCE_COMMIT = "9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1"


def run(command):
    subprocess.run([str(part) for part in command], check=True)


def tool(name, requested, expected):
    resolved = shutil.which(requested)
    if not resolved:
        raise ValueError(f"Cannot find {name}: {requested}")
    result = subprocess.run([resolved, "--version"], capture_output=True, text=True, check=True)
    version = (result.stdout + result.stderr).strip()
    if expected not in version:
        raise ValueError(f"Unsupported {name} version: {version}; expected {expected}")
    return str(Path(resolved).resolve()), version


def extract(source, destination):
    # The parser is vendored unchanged with its own MIT license and source pin.
    spec = importlib.util.spec_from_file_location(
        "inspect_nr", HERE / "vendor/dlssnr-rdna3/inspect_nr.py")
    parser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parser)
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != MODEL_SHA256:
        raise ValueError("Unsupported NR DLL. This graph supports only the tested 310.8.SF.0 "
                         f"runtime (SHA256 {MODEL_SHA256}); received {digest}")
    resources = [item for item in parser.PE(data).resources() if "WEIGHTS_HT" in item["path"]]
    if len(resources) != 1:
        raise ValueError("Expected exactly one WEIGHTS_HT resource")
    resource = resources[0]
    weights = parser.checked(data, resource["file_offset"], resource["size"])
    records = parser.parse_weights(weights)
    stage, tensors, blocks = bytearray(), [], set()
    for record in records:
        match = re.fullmatch(r"block(\d+)\.layer(\d+)\.(\w+)", record["name"])
        if not match:
            raise ValueError(f"Unsupported tensor: {record['name']}")
        block, layer = int(match[1]), int(match[2])
        blocks.add(block)
        stage.extend(b"\0" * (-len(stage) % 4))
        offset = len(stage)
        payload = parser.checked(weights, record["payload_offset"], record["payload_size"])
        stage.extend(payload)
        tensors.append({"name": record["name"], "block": block, "layer": layer,
                        "parameter": match[3], "stage": "all", "stageOffset": offset,
                        "byteLength": len(payload)})
    if blocks != set(range(71)) or len(tensors) != 153:
        raise ValueError("Expected the complete 71-block, 153-tensor model")
    stage_digest = hashlib.sha256(stage).hexdigest()
    manifest = {"totals": {"blockCount": 71}, "tensors": tensors,
                "stages": [{"id": "all", "file": "weights.bin", "packedByteLength": len(stage),
                            "sha256": stage_digest}]}
    (destination / "model").mkdir(parents=True)
    (destination / "model/weights.bin").write_bytes(stage)
    (destination / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return {"dllSHA256": digest, "resourceSHA256": resource["sha256"],
            "stageSHA256": stage_digest, "blocks": 71, "tensors": 153}


def main():
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("--dll", type=Path, required=True, help="Local nvngx_dlssnr.dll")
    arguments.add_argument("--output", type=Path, required=True, help="New private cache directory")
    arguments.add_argument("--width", type=int, default=320, help="NR network width, not display width")
    arguments.add_argument("--height", type=int, default=320, help="NR network height, not display height")
    arguments.add_argument("--node", default="node", help="Node.js 24 executable")
    arguments.add_argument("--naga", default="naga", help="naga-cli 30.0.1 executable")
    arguments.add_argument("--dxc", default="dxc", help="DXC 1.9.2609.5 executable")
    arguments.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    arguments.add_argument("--math", choices=("accurate", "fast"), default="accurate",
                           help="Accurate preserves NR reductions; fast trades numerical equivalence for speed")
    arguments.add_argument("--field", choices=("native", "compact"), default="native",
                           help="Compact lowers the padded-field floor and changes model context")
    args = arguments.parse_args()
    if not (33 <= args.width <= 4096 and 33 <= args.height <= 4096):
        raise ValueError("Network dimensions must each be between 33 and 4096")
    if not 1 <= args.jobs <= 64:
        raise ValueError("--jobs must be between 1 and 64")
    source, output = args.dll.resolve(strict=True), args.output.resolve()
    if output.exists():
        raise ValueError("Output already exists; choose a new directory to preserve existing caches")
    if not output.parent.is_dir():
        raise ValueError("The output's parent directory must already exist")
    node, node_version = tool("Node.js", args.node, "v24.")
    naga, naga_version = tool("Naga", args.naga, "30.0.1")
    dxc, dxc_version = tool("DXC", args.dxc, "1.9.2609")
    # Failed preparation cannot publish a partially compiled cache or overwrite
    # an existing one. Temporary weight files are removed on every exit path.
    with tempfile.TemporaryDirectory(prefix=".helixsr-nr-", dir=output.parent) as temporary:
        staging = Path(temporary)
        provenance = extract(source, staging / "model")
        graph = staging / "graph"
        run([node, HERE / "export_graph.mjs", args.width, args.height, staging / "model", graph, args.field])
        run([sys.executable, HERE / "compile_graph.py", "--graph", graph,
             "--naga", naga, "--dxc", dxc, "--jobs", args.jobs, "--math", args.math])
        # Only the bounded execution plan, immutable data, and validated shader
        # binaries are runtime assets. Intermediate tensor layouts stay private.
        published = staging / "cache"
        published.mkdir()
        for name in ("runtime-plan.bin", "initial-data.bin", "cso"):
            shutil.move(str(graph / name), str(published / name))
        report = {"format": "helixsr-nr-cache-1", "width": args.width, "height": args.height,
                  "referenceCommit": REFERENCE_COMMIT, "model": provenance,
                  "tools": {"node": node_version, "naga": naga_version, "dxc": dxc_version},
                  "compilation": json.loads((graph / "compile-summary.json").read_text()),
                  "math": args.math, "field": args.field, "gpuExecutionValidated": False}
        (published / "preparation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        # rename fails instead of replacing a directory created by another process.
        published.rename(output)
    print(f"Private cache ready: {output}")
    print("Preparation does not establish GPU correctness. Run the BC250 validation probes before use.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        sys.exit(f"Preparation failed: {error}")
