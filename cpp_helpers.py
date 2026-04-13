"""Helpers for generating SystemVerilog pi/po mapping code."""

import re


def _pi_path_to_sv(path, idx=None):
    """Convert a pi_to_simulator C++ path into the framework SV naming style."""
    from sv_path import cpp_path_to_sv
    sv = cpp_path_to_sv(path)
    if idx is not None:
        sv = sv.replace("[i]", f"[{idx}]")
    return sv


def generate_pi_sv(inputs):
    """Generate SV input extraction assignments from pi[] into struct variables."""
    lines = []
    for inp in inputs:
        width = inp["width"]
        for idx in range(inp["count"]):
            sv_path = _pi_path_to_sv(inp["path"], idx if inp["count"] > 1 else None)
            bit_start = inp["offset"] + idx * width
            if width == 1:
                lines.append(f"    {sv_path} = pi[{bit_start}];")
            else:
                lines.append(f"    {sv_path} = pi[{bit_start + width - 1}:{bit_start}];")
    return lines


def _po_path_to_sv(path):
    """Convert a simulator_to_po C++ path into the framework SV naming style."""
    from sv_path import cpp_path_to_sv
    path = re.sub(r"^(?!out\.|in\.)\w+\.", "", path, count=1)
    return cpp_path_to_sv(path)


def generate_po_sv(output_signals):
    """Generate SV output packing assignments from struct variables to po[]."""
    lines = []
    offset = 0
    for out in output_signals:
        sv_path = _po_path_to_sv(out["path"])
        width = out["width"]
        if width == 1:
            lines.append(f"    po[{offset}] = {sv_path};")
        else:
            lines.append(f"    po[{offset + width - 1}:{offset}] = {sv_path};")
        offset += width
    return lines
