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


def _find_matching_brace_end(text, open_idx):
    depth = 1
    i = open_idx + 1
    in_line_comment = False
    in_block_comment = False
    in_string = None
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
            i += 1
            continue

        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if ch == "\\":
                i += 2
                continue
            if ch == in_string:
                in_string = None
            i += 1
            continue

        if ch == "/" and nxt == "/":
            in_line_comment = True
            i += 2
            continue
        if ch == "/" and nxt == "*":
            in_block_comment = True
            i += 2
            continue
        if ch in ('"', "'"):
            in_string = ch
            i += 1
            continue

        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


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


def parse_module_member_helpers(cpp_source, module_type):
    # 除了 util.h 里的 inline/helper 以外，项目里还有不少定义在
    # `<Module>_cpp.h` 里的类成员辅助函数，例如 `get_latency()` /
    # `apply_wakeup_to_uop()`。这些函数不会进入 translated method 列表，
    # 但 prompt/read-set 推导仍然需要它们的源码。
    helpers = {}
    pattern = re.compile(
        rf"((?:inline\s+)?[\w:<>~*&,\s]+\b{re.escape(module_type)}::(\w+)\s*\([^)]*\)\s*(?:const\s*)?\{{)",
        re.DOTALL,
    )
    for match in pattern.finditer(cpp_source):
        name = match.group(2)
        if (
            name == module_type
            or name in ("seq", "out_initial_detect")
            or name.startswith("pi_to_simulator")
            or name.startswith("simulator_to_po")
            or name == "simulator_with_bsd"
            or name == "init"
            or name.startswith("comb_")
        ):
            continue
        body = _extract_braced_body(cpp_source, match.end())
        helpers[name] = match.group(1) + "\n" + body.rstrip() + "\n}"
    return helpers


def parse_header_inline_helpers(header_name):
    path = os.path.join(SIMULATOR_INCLUDE, header_name)
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        content = f.read()

    helpers = {}
    record_defs = {}
    for record in re.finditer(r"(struct|class)\s+(\w+)\s*\{", content):
        name = record.group(2)
        open_idx = content.find("{", record.start(), record.end())
        close_idx = _find_matching_brace_end(content, open_idx)
        if close_idx < 0:
            continue
        record_defs[name] = content[open_idx + 1:close_idx]

    method_pat = re.compile(
        r"((?:inline\s+)?[\w:<>~*&,\s]+\b(\w+)::(\w+)\s*\([^)]*\)\s*(?:const\s*)?\{)",
        re.DOTALL,
    )
    for match in method_pat.finditer(content):
        owner = match.group(2)
        name = match.group(3)
        body = _extract_braced_body(content, match.end())
        helpers[name] = match.group(1) + "\n" + body.rstrip() + "\n}"

    for owner, body in record_defs.items():
        pos = 0
        while pos < len(body):
            open_idx = body.find("{", pos)
            if open_idx < 0:
                break
            header = body[pos:open_idx].strip()
            semi_idx = header.rfind(";")
            if semi_idx >= 0:
                header = header[semi_idx + 1:].strip()
            colon_idx = header.rfind("public:")
            if colon_idx >= 0:
                header = header[colon_idx + len("public:"):].strip()
            colon_idx = header.rfind("private:")
            if colon_idx >= 0:
                header = header[colon_idx + len("private:"):].strip()
            colon_idx = header.rfind("protected:")
            if colon_idx >= 0:
                header = header[colon_idx + len("protected:"):].strip()

            name_match = re.search(r"([~]?\w+)\s*\([^()]*\)\s*(?:const\s*)?(?::[\s\S]*)?$", header, re.DOTALL)
            if not name_match:
                pos = open_idx + 1
                continue
            name = name_match.group(1)
            if name == owner or name.startswith("~"):
                close_idx = _find_matching_brace_end(body, open_idx)
                pos = close_idx + 1 if close_idx >= 0 else open_idx + 1
                continue
            prefix = header[:name_match.start(1)].strip()
            if not prefix and name == owner:
                close_idx = _find_matching_brace_end(body, open_idx)
                pos = close_idx + 1 if close_idx >= 0 else open_idx + 1
                continue
            close_idx = _find_matching_brace_end(body, open_idx)
            if close_idx < 0:
                break
            method_body = body[open_idx + 1:close_idx]
            helpers.setdefault(name, header + " {\n" + method_body.rstrip() + "\n}")
            pos = close_idx + 1
    return helpers


def build_helper_db(module_info):
    helpers = parse_helper_functions()
    helpers.update(parse_module_member_helpers(
        module_info.get("logic_source", ""),
        module_info["module_type"],
    ))
    if module_info.get("module_type") == "Isu":
        helpers.update(parse_header_inline_helpers("IssueQueue.h"))
    return helpers


def project_context_for_logic(logic_text):
    # 只在 method 实际依赖项目级 constexpr 配置表时，把 config.h 片段送进上下文。
    # 这样既避免 prompt 膨胀，也能覆盖 Isu::init 这类强依赖静态配置的函数。
    logic_text = logic_text or ""
    wants_iq = "GLOBAL_IQ_CONFIG" in logic_text
    wants_ports = (
        "GLOBAL_ISSUE_PORT_CONFIG" in logic_text
        or "PORT_CFG" in logic_text
        or "count_ports_with_mask" in logic_text
        or "find_first_port_with_mask" in logic_text
    )
    if not (wants_iq or wants_ports):
        return ""

    config_path = os.path.join(SIMULATOR_INCLUDE, "config.h")
    if not os.path.exists(config_path):
        return ""

    with open(config_path) as f:
        content = f.read()

    blocks = []
    if wants_ports:
        macro = re.search(r"^\s*#define\s+PORT_CFG\(mask\).*$", content, re.MULTILINE)
        if macro:
            blocks.append(macro.group(0).rstrip())
        port_cfg = re.search(
            r"constexpr\s+IssuePortConfigInfo\s+GLOBAL_ISSUE_PORT_CONFIG\s*\[\]\s*=\s*\{[\s\S]*?\};",
            content,
        )
        if port_cfg:
            blocks.append(port_cfg.group(0).strip())
    if wants_iq:
        iq_cfg = re.search(
            r"constexpr\s+IQStaticConfig\s+GLOBAL_IQ_CONFIG\s*\[\]\s*=\s*\{[\s\S]*?\};",
            content,
        )
        if iq_cfg:
            blocks.append(iq_cfg.group(0).strip())
    return "\n\n".join(block for block in blocks if block).strip()


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
