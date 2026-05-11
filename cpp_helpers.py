"""Helpers for generating SystemVerilog pi/po mapping code."""

import re
from sv_path import path_to_sv_slice


def _pi_path_to_sv(path, idx=None):
    """Convert a pi_to_simulator C++ path into the framework SV naming style."""
    from sv_path import cpp_path_to_sv
    sv = cpp_path_to_sv(path)
    if idx is not None:
        sv = sv.replace("[i]", f"[{idx}]")
    return sv


def generate_pi_sv(inputs, max_width=None, module_info=None):
    """Generate SV input extraction assignments from pi[] into struct variables."""
    lines = []
    for inp in inputs:
        width = int(inp["width"])
        for idx in range(inp["count"]):
            sv_path = _pi_path_to_sv(inp["path"], idx if inp["count"] > 1 else None)
            sv_lhs = path_to_sv_slice(module_info, sv_path, width_hint=width) or sv_path
            bit_start = inp["offset"] + idx * width
            emit_width = width
            if max_width is not None:
                limit = int(max_width)
                if bit_start >= limit:
                    continue
                if bit_start + emit_width > limit:
                    emit_width = limit - bit_start
                    if emit_width <= 0:
                        continue
            if emit_width == 1:
                lines.append(f"    {sv_lhs} = pi[{bit_start}];")
            else:
                lines.append(f"    {sv_lhs} = pi[{bit_start + emit_width - 1}:{bit_start}];")
    return lines


def _po_path_to_sv(path):
    """Convert a simulator_to_po C++ path into the framework SV naming style."""
    from sv_path import cpp_path_to_sv
    # Only strip prefixes from known IO struct names (C++ wrapper local variables)
    # that are NOT actual SV declared variables like br_latch_1, tag_vec_1, etc.
    io_struct_prefixes = re.compile(
        r"^(rob_bcast|dec_bcast|dec2ren|idu_consume|issue|ren2dec|exu2id"
        r"|rob_commit|dis_rob|ren_dis|rob_dis|prf_dis"
        r"|csr_(?:req|resp|rob|int_io)"
        r"|ftq_pc_(?:req|resp)"
        r")\.", re.IGNORECASE)
    path = io_struct_prefixes.sub("", path, count=1)
    return cpp_path_to_sv(path)


def generate_po_sv(output_signals, max_width=None, module_info=None):
    """Generate SV output packing assignments from struct variables to po[]."""
    lines = []
    offset = 0
    for out in output_signals:
        sv_path = _po_path_to_sv(out["path"])
        width = int(out["width"])
        sv_rhs = path_to_sv_slice(module_info, sv_path, width_hint=width) or sv_path
        if max_width is not None and offset >= int(max_width):
            break
        emit_width = width
        if max_width is not None and offset + emit_width > int(max_width):
            emit_width = int(max_width) - offset
            if emit_width <= 0:
                break
        if emit_width == 1:
            lines.append(f"    po[{offset}] = {sv_rhs};")
        else:
            lines.append(f"    po[{offset + emit_width - 1}:{offset}] = {sv_rhs};")
        offset += width
    return lines
