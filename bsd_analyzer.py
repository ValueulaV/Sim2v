"""Analyze *_bsd modules and expose the metadata needed by sim2v."""

import logging
import os
import re

import io_mapping
from bsd_types import (
    KNOWN_CONSTANTS,
    _try_eval_const_expr,
    _validate_signal_paths,
    extract_method_helpers,
    generate_cpp_type_sources,
    generate_sv_typedefs,
    generate_sv_var_declarations,
    get_method_signal_width_hints,
    get_struct_order_for_method,
    parse_all_constants,
    parse_all_structs,
    parse_helper_functions,
)

logger = logging.getLogger(__name__)

SIMULATOR_INCLUDE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "io_generator", "simulator_include",
)


def analyze_module(bsd_dir, module_type=None, mapping_provider=None):
    # analyze_module 是“模块静态信息收集”的总入口。
    # 目标不是做完整 C++ 语义分析，而是给后续阶段提供一份足够稳定的中间表示：
    # - module_type
    # - method bodies
    # - pi/po 映射
    # - typedef/struct 信息
    # prompt/snippet/combine 都依赖它。
    if not module_type:
        module_type = _detect_module_type(bsd_dir)

    cpp_source_file = os.path.join(SIMULATOR_INCLUDE, f"{module_type}_cpp.h")
    with open(cpp_source_file) as f:
        cpp_source = f.read()

    provider = mapping_provider or io_mapping.get_mapping_provider()
    inputs = provider.parse_inputs(cpp_source, module_type)
    bsd_files, outputs = provider.collect_outputs(bsd_dir, module_type, cpp_source)
    methods = extract_methods(cpp_source, module_type)

    first_bsd = os.path.join(bsd_dir, bsd_files[0]["filename"])
    with open(first_bsd) as f:
        content = f.read()
    pi_width = int(re.search(r"PI_WIDTH\s*=\s*(\d+)", content).group(1))

    type_widths, structs, struct_sources = parse_all_structs()
    _validate_signal_paths(outputs, structs, type_widths, module_type)

    return {
        "module_type": module_type,
        "pi_width": pi_width,
        "inputs": inputs,
        "outputs": outputs,
        "methods": methods,
        "logic_source": cpp_source,
        "bsd_files": bsd_files,
        "type_widths": type_widths,
        "structs": structs,
        "struct_sources": struct_sources,
    }


def _detect_module_type(bsd_dir):
    # 当前实现是基于 wrapper include 的启发式识别，
    # 适合 PRF/ROB 这类结构稳定的 case，但不是通用模块发现器。
    ignore = {"IO", "config", "util", "cstdint", "cassert", "cstdio", "stdio", "stdint", "assert"}
    for fname in sorted(os.listdir(bsd_dir)):
        if not fname.endswith(".h"):
            continue
        with open(os.path.join(bsd_dir, fname)) as fh:
            content = fh.read()
        for match in re.finditer(r'#include\s*<([A-Za-z_]\w*)\.h>', content):
            name = match.group(1)
            cpp_source = os.path.join(SIMULATOR_INCLUDE, f"{name}_cpp.h")
            if name not in ignore and os.path.exists(cpp_source):
                return name
        if "<PRF.h>" in content:
            return "PRF"
        if "<ROB.h>" in content:
            return "ROB"
    raise ValueError(f"Cannot detect module type from include headers in {bsd_dir}")


def extract_methods(cpp_source, module_type):
    # method 提取采用“正则命中函数头 + 花括号配对截 body”的轻量方案。
    # 它不是 AST，因此默认假设 simulator 代码风格比较规整。
    methods = []
    pattern = re.compile(rf"void\s+{re.escape(module_type)}::(\w+)\s*\(\s*\)\s*\{{")
    for match in pattern.finditer(cpp_source):
        name = match.group(1)
        if name in ("pi_to_simulator", "simulator_to_po", "out_initial_detect", "seq", "simulator_with_bsd"):
            continue
        body = _extract_braced_body(cpp_source, match.end())
        methods.append({"name": name, "body": body})
    return methods


def _extract_braced_body(text, start):
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[start:i - 1].strip()
