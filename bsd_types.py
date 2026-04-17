"""Struct/type parsing and SV type-generation helpers for sim2v."""

import logging
import os
import re

from sv_path import cpp_path_to_sv, escape_sv_keyword

logger = logging.getLogger(__name__)

SIMULATOR_INCLUDE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "io_generator", "simulator_include",
)


def _extract_braced_body(text, start):
    close_idx = _find_matching_brace_end(text, start - 1)
    if close_idx < 0:
        return text[start:].strip()
    return text[start:close_idx].strip()


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


def _try_eval_const_expr(expr, known_constants):
    expr = _normalize_const_expr(expr)
    if not expr:
        return None
    if re.fullmatch(r"-?\d+", expr):
        return int(expr)
    try:
        val = eval(expr, {"__builtins__": {}}, known_constants)
    except Exception:
        return None
    if isinstance(val, int):
        return int(val)
    if isinstance(val, float) and val.is_integer():
        return int(val)
    return None


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*", "", text)


VECTOR_FIELD_DIM_HINTS = {
    ("Isu", "iqs"): ["IQ_NUM"],
    ("Isu", "configs"): ["IQ_NUM"],
    ("Isu", "latency_pipe"): ["DIV_MAX_LATENCY"],
    ("Isu", "latency_pipe_1"): ["DIV_MAX_LATENCY"],
    ("Isu", "committed_indices_buf"): ["ISSUE_WIDTH"],
    ("IssueQueueConfig", "ports"): ["ISSUE_WIDTH"],
    ("IssueQueue", "ports"): ["ISSUE_WIDTH"],
    ("IssueQueue", "entry"): ["MAX_IQ_SIZE"],
    ("IssueQueue", "entry_1"): ["MAX_IQ_SIZE"],
    ("IssueQueue", "wake_matrix_src1"): ["PRF_NUM"],
    ("IssueQueue", "wake_matrix_src2"): ["PRF_NUM"],
}


def _normalize_const_expr(expr):
    expr = expr.strip()
    if not expr:
        return expr
    expr = re.sub(r"\b(?:static_cast|reinterpret_cast|const_cast|dynamic_cast)\s*<[^>]+>\s*\(([^()]+)\)", r"(\1)", expr)
    expr = re.sub(r"\b\w+::", "", expr)
    expr = re.sub(r"\btrue\b", "True", expr)
    expr = re.sub(r"\bfalse\b", "False", expr)
    expr = expr.replace("&&", " and ")
    expr = expr.replace("||", " or ")
    expr = re.sub(r"(?<![<>=!])!(?!=)", " not ", expr)
    expr = re.sub(
        r"\b(0[xX][0-9a-fA-F]+|0[bB][01]+|\d+)([uU](?:[lL]{1,2})?|[lL]{1,2}[uU]?)\b",
        r"\1",
        expr,
    )
    return " ".join(expr.split())


def _split_top_level(text, sep=","):
    parts = []
    buf = []
    depth_paren = 0
    depth_brace = 0
    depth_bracket = 0
    depth_angle = 0
    for ch in text:
        if ch == "(":
            depth_paren += 1
        elif ch == ")":
            depth_paren = max(0, depth_paren - 1)
        elif ch == "{":
            depth_brace += 1
        elif ch == "}":
            depth_brace = max(0, depth_brace - 1)
        elif ch == "[":
            depth_bracket += 1
        elif ch == "]":
            depth_bracket = max(0, depth_bracket - 1)
        elif ch == "<":
            depth_angle += 1
        elif ch == ">":
            depth_angle = max(0, depth_angle - 1)

        if ch == sep and all(d == 0 for d in (depth_paren, depth_brace, depth_bracket, depth_angle)):
            part = "".join(buf).strip()
            if part:
                parts.append(part)
            buf = []
            continue
        buf.append(ch)

    tail = "".join(buf).strip()
    if tail:
        parts.append(tail)
    return parts


def _extract_brace_groups(text):
    groups = []
    depth = 0
    start = None
    for i, ch in enumerate(text):
        if ch == "{":
            if depth == 0:
                start = i + 1
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None:
                groups.append(text[start:i].strip())
                start = None
    return groups


def _parse_enum_constants(text, known):
    for m in re.finditer(r"enum(?:\s+class)?(?:\s+\w+)?(?:\s*:\s*\w+)?\s*\{(.*?)\}\s*;", text, re.DOTALL):
        current = -1
        for item in _split_top_level(m.group(1)):
            if not item:
                continue
            if "=" in item:
                name, expr = item.split("=", 1)
                name = name.strip()
                val = _try_eval_const_expr(expr.strip(), known)
                if val is None:
                    continue
                current = val
                known[name] = val
            else:
                name = item.strip()
                if not re.fullmatch(r"[A-Za-z_]\w*", name):
                    continue
                current += 1
                known[name] = current


def _parse_object_like_macros(text):
    raw_defs = {}
    for m in re.finditer(r"^\s*#define\s+([A-Z_]\w*)\s*(.*)$", text, re.MULTILINE):
        name = m.group(1)
        expr = m.group(2).strip()
        if not expr or "(" in name:
            continue
        if any(t in expr for t in ['"', "'", "{", "}"]):
            continue
        raw_defs[name] = expr
    return raw_defs


def _parse_constexpr_scalars(text):
    raw_defs = {}
    pat = re.compile(
        r"constexpr\s+[^;{}=]+?\s+([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*=\s*(.*?);",
        re.DOTALL,
    )
    for m in pat.finditer(text):
        name = m.group(1)
        array_suffix = m.group(2)
        expr = m.group(3).strip()
        if array_suffix:
            continue
        if any(t in expr for t in ['"', "'", "{", "}"]):
            continue
        raw_defs[name] = expr
    return raw_defs


def _resolve_raw_definitions(raw_defs, known):
    unresolved = dict(raw_defs)
    changed = True
    while unresolved and changed:
        changed = False
        for name, expr in list(unresolved.items()):
            val = _try_eval_const_expr(expr, known)
            if val is not None:
                known[name] = val
                unresolved.pop(name)
                changed = True
    return unresolved


def _extract_project_issue_port_configs(text, known):
    m = re.search(
        r"constexpr\s+IssuePortConfigInfo\s+GLOBAL_ISSUE_PORT_CONFIG\s*\[\]\s*=\s*\{(.*?)\};",
        text,
        re.DOTALL,
    )
    if not m:
        return []

    configs = []
    for idx, item in enumerate(_split_top_level(m.group(1))):
        mask_expr = None
        port_cfg = re.fullmatch(r"PORT_CFG\s*\((.*)\)", item, re.DOTALL)
        if port_cfg:
            mask_expr = port_cfg.group(1).strip()
        else:
            fields = _split_top_level(item.strip().strip("{}"))
            if len(fields) >= 2:
                mask_expr = fields[1]
        if mask_expr is None:
            continue
        mask_val = _try_eval_const_expr(mask_expr, known)
        if mask_val is None:
            continue
        configs.append({"port_idx": idx, "support_mask": mask_val})
    return configs


def _extract_project_iq_configs(text, known):
    m = re.search(
        r"constexpr\s+IQStaticConfig\s+GLOBAL_IQ_CONFIG\s*\[\]\s*=\s*\{(.*?)\};",
        text,
        re.DOTALL,
    )
    if not m:
        return []

    configs = []
    for group in _extract_brace_groups(m.group(1)):
        fields = _split_top_level(group)
        if len(fields) != 6:
            continue
        values = []
        for field in fields:
            val = _try_eval_const_expr(field, known)
            if val is None:
                values = []
                break
            values.append(val)
        if values:
            configs.append({
                "id": values[0],
                "size": values[1],
                "dispatch_width": values[2],
                "supported_ops": values[3],
                "port_start_idx": values[4],
                "port_num": values[5],
            })
    return configs


def _vector_field_extra_dims(record_name, field_name, raw_type_name):
    if "vector<" not in raw_type_name.replace(" ", ""):
        return []
    hint_exprs = VECTOR_FIELD_DIM_HINTS.get((record_name, field_name), [])
    dims = []
    for expr in hint_exprs:
        val = _try_eval_const_expr(expr, KNOWN_CONSTANTS)
        if val is None:
            continue
        dims.append(int(val))
    return dims


def _load_known_constants():
    # 当前实现仍然不是完整的 C/C++ 常量求值器，但会覆盖 simulator_include
    # 里实际依赖到的 object-like #define、enum、constexpr 标量和少量配置表推导。
    header_names = ["base_types.h", "config.h", "types.h"]
    contents = []
    for fname in header_names:
        path = os.path.join(SIMULATOR_INCLUDE, fname)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            contents.append(_strip_comments(f.read()))
    if not contents:
        return {}

    combined = "\n".join(contents)

    def _clog2(n):
        res = 0
        while n > (1 << res):
            res += 1
        return res

    def _bit_width_for_count(count):
        width = 0
        max_value = count - 1 if count > 0 else 0
        while True:
            width += 1
            max_value >>= 1
            if max_value == 0:
                return width

    def _is_power_of_two_u64(n):
        return n != 0 and (n & (n - 1)) == 0

    known = {
        "clog2": _clog2,
        "bit_width_for_count": _bit_width_for_count,
        "is_power_of_two_u64": _is_power_of_two_u64,
    }

    _parse_enum_constants(combined, known)

    raw_defs = {}
    raw_defs.update(_parse_object_like_macros(combined))
    raw_defs.update(_parse_constexpr_scalars(combined))
    _resolve_raw_definitions(raw_defs, known)

    issue_port_configs = _extract_project_issue_port_configs(combined, known)
    if issue_port_configs:
        known["ISSUE_WIDTH"] = len(issue_port_configs)

        def _count_ports_with_mask(mask):
            return sum(1 for cfg in issue_port_configs if cfg["support_mask"] & mask)

        def _find_first_port_with_mask(mask):
            for cfg in issue_port_configs:
                if cfg["support_mask"] & mask:
                    return cfg["port_idx"]
            return -1

        known["count_ports_with_mask"] = _count_ports_with_mask
        known["find_first_port_with_mask"] = _find_first_port_with_mask
        _resolve_raw_definitions(raw_defs, known)

        major_masks = [
            known.get("OP_MASK_ALU"),
            known.get("OP_MASK_CSR"),
            known.get("OP_MASK_MUL"),
            known.get("OP_MASK_DIV"),
            known.get("OP_MASK_BR"),
            known.get("OP_MASK_LD"),
            known.get("OP_MASK_STA"),
            known.get("OP_MASK_STD"),
            known.get("OP_MASK_FP"),
        ]
        major_masks = [m for m in major_masks if isinstance(m, int)]
        if major_masks and "TOTAL_FU_COUNT" not in known:
            known["TOTAL_FU_COUNT"] = sum(
                sum(1 for mask in major_masks if cfg["support_mask"] & mask)
                for cfg in issue_port_configs
            )

    iq_configs = _extract_project_iq_configs(combined, known)
    if iq_configs and "MAX_IQ_SIZE" not in known:
        known["MAX_IQ_SIZE"] = max(cfg["size"] for cfg in iq_configs)
        _resolve_raw_definitions(raw_defs, known)

    return known


KNOWN_CONSTANTS = _load_known_constants()


def parse_all_structs():
    # 这里按头文件顺序把 simulator_include 中的 struct/class/typedef 扫一遍，
    # 拼成后续阶段共享的类型数据库。
    # 注意这是文本解析器，不是 clang AST。
    type_widths = {}
    structs = {}
    struct_sources = {}

    header_names = []
    for priority in ("config.h", "IO.h"):
        if os.path.exists(os.path.join(SIMULATOR_INCLUDE, priority)):
            header_names.append(priority)
    for fname in sorted(os.listdir(SIMULATOR_INCLUDE)):
        if fname.endswith(".h") and fname not in header_names:
            header_names.append(fname)

    for fname in header_names:
        path = os.path.join(SIMULATOR_INCLUDE, fname)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            content = f.read()
        tw, st, src = _parse_file_structs(content, type_widths)
        type_widths.update(tw)
        structs.update(st)
        struct_sources.update(src)

    return type_widths, structs, struct_sources


def _iter_record_defs(text):
    record_pat = re.compile(
        r"typedef\s+struct(?:\s+\w+)?\s*\{"
        r"|struct\s+(\w+)\s*\{"
        r"|class\s+(\w+)\s*\{",
        re.DOTALL,
    )
    pos = 0
    while True:
        m = record_pat.search(text, pos)
        if not m:
            break

        open_idx = text.find("{", m.start(), m.end())
        if open_idx < 0:
            pos = m.end()
            continue
        close_idx = _find_matching_brace_end(text, open_idx)
        if close_idx < 0:
            pos = m.end()
            continue

        if text[m.start():m.end()].lstrip().startswith("typedef struct"):
            alias_match = re.match(r"\s*(\w+)\s*;", text[close_idx + 1:])
            if not alias_match:
                pos = close_idx + 1
                continue
            name = alias_match.group(1)
            end_idx = close_idx + 1 + alias_match.end()
        else:
            name = m.group(1) or m.group(2)
            semi_match = re.match(r"\s*;", text[close_idx + 1:])
            end_idx = close_idx + 1 + semi_match.end() if semi_match else close_idx + 1

        body = text[open_idx + 1:close_idx]
        source = text[m.start():end_idx].strip()
        yield {
            "name": name,
            "body": body,
            "source": source,
        }
        pos = end_idx


def _remove_text_spans(text, spans):
    if not spans:
        return text
    parts = []
    cursor = 0
    for start, end in sorted(spans):
        parts.append(text[cursor:start])
        cursor = max(cursor, end)
    parts.append(text[cursor:])
    return "".join(parts)


def _strip_nested_record_defs(body):
    spans = []
    for record in _iter_record_defs(body):
        source = record["source"]
        start = body.find(source)
        if start >= 0:
            spans.append((start, start + len(source)))
    return _remove_text_spans(body, spans)


def _looks_like_method_start(line, record_name=None):
    line = line.strip()
    if not line or line.startswith("//"):
        return False
    if line in ("public:", "private:", "protected:"):
        return False
    if re.match(r"(?:if|for|while|switch)\s*\(", line):
        return False
    if record_name and re.match(rf"(?:explicit\s+)?~?{re.escape(record_name)}\s*\(", line):
        return True
    return bool(re.match(r"(?:static\s+)?(?:inline\s+)?[\w:<>~*&,\s]+\s+\w+\s*\(", line))


def _prepare_record_body_for_fields(body, record_name=None):
    body = _strip_nested_record_defs(body)
    lines = []
    for raw in body.split("\n"):
        line = raw.strip()
        if not line:
            continue
        if line in ("public:", "private:", "protected:"):
            continue
        if _looks_like_method_start(line, record_name):
            # Method declarations (`foo();`) may appear before data members in
            # some module headers (e.g. ROB). Skip declarations but stop at
            # inline method bodies to avoid parsing function code as fields.
            if line.endswith(";") and "{" not in line:
                continue
            break
        lines.append(line)
    return "\n".join(lines)


def _normalize_cpp_type_name(type_name):
    type_name = re.sub(r"\bconst\b", "", type_name)
    type_name = re.sub(r"\s+", " ", type_name).strip()
    vector_match = re.fullmatch(r"(?:std::)?vector\s*<\s*(.+)\s*>", type_name)
    if vector_match:
        return _normalize_cpp_type_name(vector_match.group(1))
    return type_name.split("::")[-1].strip()


def _candidate_struct_names(type_name):
    base = (type_name or "").strip()
    if not base:
        return []

    cands = [base]
    no_us = base.replace("_", "")
    if no_us and no_us not in cands:
        cands.append(no_us)

    if not base.endswith("IO"):
        cands.append(base + "IO")
    if no_us and not no_us.endswith("IO"):
        cands.append(no_us + "IO")

    if "_" in base:
        camel = "".join(part[:1].upper() + part[1:] for part in base.split("_") if part)
        if camel and camel not in cands:
            cands.append(camel)
        if camel and not camel.endswith("IO") and (camel + "IO") not in cands:
            cands.append(camel + "IO")

    seen = set()
    out = []
    for cand in cands:
        if cand and cand not in seen:
            seen.add(cand)
            out.append(cand)
    return out


def _resolve_struct_type_name(type_name, structs):
    for cand in _candidate_struct_names(type_name):
        if cand in structs:
            return cand
    return type_name


def _infer_field_width(type_name, type_widths):
    norm = _normalize_cpp_type_name(type_name)
    bit_type = re.fullmatch(r"(?:wire|reg)\s*<\s*([^>]+)\s*>", norm)
    if bit_type:
        return _try_eval_const_expr(bit_type.group(1), KNOWN_CONSTANTS)
    return type_widths.get(norm)


def _parse_file_structs(content, existing_type_widths=None):
    # 先解析 primitive typedef 宽度，再解析 struct/class 字段。
    # 顺序很重要：后续字段如果引用了前面定义的 alias，需要先把位宽表建起来。
    type_widths = dict(existing_type_widths or {})
    structs = {}
    struct_sources = {}

    prim_widths = {
        "bool": 1,
        "char": 8,
        "int": 32,
        "int32_t": 32,
        "int64_t": 64,
        "uint8_t": 8,
        "uint16_t": 16,
        "uint32_t": 32,
        "uint64_t": 64,
    }
    for name, width in prim_widths.items():
        type_widths.setdefault(name, width)

    for m in re.finditer(r"typedef\s+(\w+)\s+(\w+)\s*;", content):
        base_type, alias = m.group(1), m.group(2)
        if base_type in prim_widths:
            wm = re.match(r"(?:wire|reg)(\d+)_t", alias)
            type_widths[alias] = int(wm.group(1)) if wm else prim_widths[base_type]
        elif base_type in type_widths:
            type_widths[alias] = type_widths[base_type]

    for record in _iter_record_defs(content):
        nested_tw, nested_structs, nested_sources = _parse_file_structs(record["body"], type_widths)
        type_widths.update(nested_tw)
        structs.update(nested_structs)
        struct_sources.update(nested_sources)

        cleaned = _prepare_record_body_for_fields(record["body"], record["name"])
        fields = _parse_struct_fields(cleaned, type_widths, record["name"])
        if fields:
            structs[record["name"]] = fields
            struct_sources[record["name"]] = record["source"]

    return type_widths, structs, struct_sources


def _parse_struct_fields(body, type_widths, record_name=None):
    # 字段解析只覆盖当前项目里常见的“简单声明 + 数组维度”模式。
    # 复杂模板、函数指针、宏展开类型等都不在这个解析器的目标范围内。
    fields = []
    for raw in body.split("\n"):
        line = raw.strip()
        if not line or line.startswith("//") or line.startswith("#"):
            continue
        if line.startswith(("return ", "if ", "for ", "while ", "switch ", "else", "break", "continue")):
            continue

        m = re.match(r"([~\w:<>]+(?:\s*<[^;]+?>)?)\s+(.+);", line)
        if not m:
            continue
        raw_type_name = m.group(1).strip()
        type_name = _normalize_cpp_type_name(raw_type_name)
        rest = m.group(2).strip()

        for part in rest.split(","):
            part = re.sub(r"^\*\s*", "", part.strip())
            if not part:
                continue
            arr_m = re.match(r"(\w+)((?:\s*\[[^\]]+\])*)", part)
            if not arr_m:
                continue
            fname = arr_m.group(1)
            dims_str = arr_m.group(2).strip()
            if dims_str:
                dim_vals = []
                for dm in re.finditer(r"\[([^\]]+)\]", dims_str):
                    val = _try_eval_const_expr(dm.group(1).strip(), KNOWN_CONSTANTS)
                    if val is None:
                        raise ValueError(f"Unresolved array dimension expression: {dm.group(1).strip()}")
                    dim_vals.append(val)
                array_dims = dim_vals
            else:
                if not re.match(r"^\w+$", fname):
                    continue
                array_dims = None
            extra_dims = _vector_field_extra_dims(record_name, fname, raw_type_name)
            if extra_dims:
                array_dims = (array_dims or []) + extra_dims
            width = _infer_field_width(raw_type_name, type_widths)
            fields.append({
                "name": fname,
                "type": type_name,
                "width": width,
                "array_size": array_dims[0] if array_dims and len(array_dims) == 1 else None,
                "array_dims": array_dims,
            })
    return fields


def _validate_signal_paths(outputs, structs, type_widths, module_type):
    all_field_names = {f["name"] for fields in structs.values() for f in fields}
    errors = []
    for out in outputs:
        path = out["path"]
        clean = re.sub(r"^\w+\.(out|in)\.", "", path).replace("->", ".")
        parts = [re.sub(r"\[[^\]]+\]", "", part) for part in clean.split(".")]
        leaf = parts[-1]
        if leaf not in all_field_names and leaf not in type_widths:
            errors.append(f"Unknown leaf field '{leaf}' in: {path}")
    if errors:
        for err in errors:
            logger.warning(err)
        logger.warning(
            "%d unresolved signal paths in %s; continue with best-effort mapping.",
            len(errors),
            module_type,
        )


def parse_helper_functions():
    # helper 目前统一从 util.h 提取。
    # 这是 prompt/snippet 两边共享的“辅助语义上下文”来源。
    util_path = os.path.join(SIMULATOR_INCLUDE, "util.h")
    with open(util_path) as f:
        content = f.read()

    helpers = {}
    for m in re.finditer(r'(#define\s+(\w+)\([^)]*\)\s+.+)', content):
        helpers[m.group(2)] = m.group(1)

    func_pat = re.compile(r'(inline\s+\w+\s+(\w+)\s*\([^)]*\)\s*\{)')
    for m in func_pat.finditer(content):
        name = m.group(2)
        body = _extract_braced_body(content, m.end())
        helpers[name] = m.group(1) + "\n" + body.rstrip() + "\n}"
    return helpers


def extract_method_helpers(method_body, all_helpers):
    # helper 闭包提取：
    # 先找当前 method 直接调用的 helper，再递归补齐 helper 调 helper 的依赖。
    used = {}
    for name, source in all_helpers.items():
        if re.search(rf'\b{re.escape(name)}\s*\(', method_body):
            used[name] = source

    changed = True
    while changed:
        changed = False
        for name, source in all_helpers.items():
            if name in used:
                continue
            for usource in used.values():
                if re.search(rf'\b{re.escape(name)}\s*\(', usource):
                    used[name] = source
                    changed = True
                    break
    return used


def parse_all_constants():
    constants = {}
    for name, value in KNOWN_CONSTANTS.items():
        if isinstance(value, bool):
            constants[name] = "1" if value else "0"
        elif isinstance(value, int):
            constants[name] = str(value)
    return constants


def _cpp_type_to_sv(type_name, width, structs):
    resolved_type = _resolve_struct_type_name(type_name, structs)
    if resolved_type in structs:
        return f"{resolved_type}_t"
    if width is not None:
        return "logic" if width == 1 else f"logic [{width - 1}:0]"
    if type_name == "bool":
        return "logic"
    return None


def _topo_sort_structs(structs, relevant):
    # SV typedef 生成时必须保证“被依赖类型先定义”。
    # 这里对相关 struct 做一个轻量拓扑排序，避免前向引用带来的声明问题。
    deps = {}
    for sname in relevant:
        if sname not in structs:
            continue
        deps[sname] = {
            f["type"] for f in structs[sname]
            if f["type"] in structs and f["type"] in relevant
        }

    ordered = []
    visited = set()

    def visit(name):
        if name in visited:
            return
        visited.add(name)
        for dep in deps.get(name, ()): 
            visit(dep)
        ordered.append(name)

    for name in sorted(deps):
        visit(name)
    return ordered


def _method_referenced_structs(structs, method_body):
    if not method_body:
        return set()
    refs = set()
    for sname in structs:
        if re.search(rf"\b{re.escape(sname)}\b", method_body):
            refs.add(sname)
    return refs


def _struct_roots_for_method(structs, module_type, method_body=None):
    roots = {module_type} if module_type in structs else set()
    roots.update(_method_referenced_structs(structs, method_body))
    return roots


def _collect_structs_by_depth(structs, roots, max_depth):
    if not roots:
        return set()
    if max_depth is None:
        max_depth = 2
    if max_depth >= 0:
        max_depth = max(max_depth - 1, 0)

    selected = set()
    queue = [(root, 0) for root in sorted(roots)]
    seen = set()
    while queue:
        name, depth = queue.pop(0)
        if name in seen:
            continue
        seen.add(name)
        if name not in structs:
            continue
        selected.add(name)
        if max_depth >= 0 and depth >= max_depth:
            continue
        for field in structs.get(name, []):
            dep = field.get("type")
            if dep in structs and dep not in seen:
                queue.append((dep, depth + 1))
    return selected


def get_struct_order_for_method(structs, module_type, method_body=None, expand_depth=1):
    # 给 prompt 做类型裁剪时，不必总是把整个 simulator_include 全塞进去。
    # 这里按 root type + method 实际引用，截取一个足够小的 struct 子图。
    if expand_depth == 0:
        return []
    roots = _struct_roots_for_method(structs, module_type, method_body)
    selected = _collect_structs_by_depth(structs, roots, expand_depth)
    return _topo_sort_structs(structs, selected)


def _compact_record_source(source):
    records = list(_iter_record_defs(source or ""))
    if len(records) != 1:
        return (source or "").strip()

    record = records[0]
    body = _prepare_record_body_for_fields(record["body"], record["name"]).strip()
    body_lines = []
    if body:
        for line in body.splitlines():
            body_lines.append(f"  {line.rstrip()}")
    body_text = "\n".join(body_lines)

    stripped = (source or "").lstrip()
    if stripped.startswith("typedef struct"):
        return f"typedef struct {{\n{body_text}\n}} {record['name']};".strip()
    if stripped.startswith("class"):
        class_body = "public:\n" + body_text if body_text else "public:"
        return f"class {record['name']} {{\n{class_body}\n}};".strip()
    return f"struct {record['name']} {{\n{body_text}\n}};".strip()


def generate_cpp_type_sources(struct_sources, ordered_structs):
    parts = []
    for sname in ordered_structs:
        src = struct_sources.get(sname, "")
        if src:
            parts.extend([_compact_record_source(src), ""])
    return "\n".join(parts).strip()


def generate_sv_typedefs(structs, type_widths, module_type=None, method_body=None, expand_depth=-1, ordered_structs=None):
    # 这是 C++ 类型数据库到 SV typedef 的单向投影。
    # 当前策略偏保守：只生成后续 prompt/combine/snippet 真正会用到的 packed struct 定义。
    if ordered_structs is None:
        if module_type:
            ordered = get_struct_order_for_method(
                structs,
                module_type,
                method_body=method_body,
                expand_depth=expand_depth,
            )
            if not ordered:
                ordered = _topo_sort_structs(structs, set(structs.keys()))
        else:
            ordered = _topo_sort_structs(structs, set(structs.keys()))
    else:
        ordered = list(ordered_structs)

    lines = []
    for sname in ordered:
        fields = structs[sname]
        field_lines = []
        for field in fields:
            sv_type = _cpp_type_to_sv(field["type"], field["width"], structs)
            if sv_type is None:
                continue
            fname = escape_sv_keyword(field["name"])
            field_lines.append(
                f"    {_attach_packed_dims_to_sv_type(sv_type, field.get('array_dims'))} {fname};"
            )
        if field_lines:
            lines.append("typedef struct packed {")
            lines.extend(field_lines)
            lines.append(f"}} {sname}_t;")
            lines.append("")
    return "\n".join(lines)


def _format_sv_dims(dims):
    if not dims:
        return ""
    return "".join(f" [0:{dim - 1}]" for dim in dims)


def _format_sv_packed_dims(dims):
    if not dims:
        return ""
    return "".join(f" [{dim - 1}:0]" for dim in dims)


def _attach_packed_dims_to_sv_type(sv_type, dims):
    if not dims:
        return sv_type
    packed_dims = _format_sv_packed_dims(dims)
    if sv_type.startswith("logic"):
        suffix = sv_type[len("logic"):]
        return f"logic{packed_dims}{suffix}"
    return f"{sv_type}{packed_dims}"


def _module_interface_type(structs, module_type, field_name):
    for field in structs.get(module_type, []):
        if field["name"] == field_name:
            return field.get("type")
    legacy = f"{module_type}_{field_name.upper()}"
    if legacy in structs:
        return legacy
    camel = f"{module_type}{field_name.capitalize()}"
    if camel in structs:
        return camel
    return None


def generate_sv_var_declarations(structs, module_type):
    # 顶层变量声明分成三组返回：
    # - 输入根信号
    # - 输出根信号
    # - 模块内部状态
    # 后续 prompt、snippet wrapper、combine 都按这个分组使用。
    in_class = _module_interface_type(structs, module_type, "in")
    out_class = _module_interface_type(structs, module_type, "out")

    def decls_for_class(cls_name, prefix):
        decls = []
        if cls_name not in structs:
            return decls
        for field in structs[cls_name]:
            sv = _cpp_type_to_sv(field["type"], field["width"], structs)
            if sv is None:
                continue
            dims = field.get("array_dims")
            decls.append(f"{sv} {prefix}{escape_sv_keyword(field['name'])}{_format_sv_dims(dims)};")
        return decls

    input_decls = decls_for_class(in_class, "in_")
    output_decls = decls_for_class(out_class, "out_")

    internal_decls = []
    if module_type in structs:
        for field in structs[module_type]:
            if field["name"] in ("in", "out"):
                continue
            sv = _cpp_type_to_sv(field["type"], field["width"], structs)
            if sv is None:
                continue
            internal_decls.append(
                f"{sv} {escape_sv_keyword(field['name'])}{_format_sv_dims(field.get('array_dims'))};"
            )
    return input_decls, output_decls, internal_decls


def _build_root_var_type_map(structs, module_type):
    roots = {}

    def add_var(name, finfo):
        roots[name] = {"type": finfo.get("type"), "width": finfo.get("width")}

    for field in structs.get(_module_interface_type(structs, module_type, "in"), []):
        add_var(f"in_{escape_sv_keyword(field['name'])}", field)
    for field in structs.get(_module_interface_type(structs, module_type, "out"), []):
        add_var(f"out_{escape_sv_keyword(field['name'])}", field)
    for field in structs.get(module_type, []):
        if field["name"] not in ("in", "out"):
            add_var(escape_sv_keyword(field["name"]), field)
    return roots


def _strip_indices(token):
    return re.sub(r"\[[^\]]+\]", "", token)


def resolve_sv_path_width(path, structs, module_type, type_widths):
    roots = _build_root_var_type_map(structs, module_type)
    parts = path.split(".")
    if not parts:
        return None

    cur = roots.get(_strip_indices(parts[0]))
    if not cur:
        return None

    cur_type = cur.get("type")
    cur_width = cur.get("width")
    for token in parts[1:]:
        if cur_type not in structs:
            return None
        fname = _strip_indices(token)
        found = None
        for field in structs.get(cur_type, []):
            if escape_sv_keyword(field["name"]) == fname:
                found = field
                break
        if not found:
            return None
        cur_type = found.get("type")
        cur_width = found.get("width")

    if cur_width is not None:
        return int(cur_width)
    if cur_type in type_widths:
        return int(type_widths[cur_type])
    return None


def extract_cpp_signal_paths(text, structs, module_type):
    roots = _build_root_var_type_map(structs, module_type)
    if not text:
        return []

    pat = re.compile(
        r"\b(?:in\.\w+->[\w\[\]\.]+|out\.\w+->[\w\[\]\.]+|"
        r"[A-Za-z_]\w*(?:\[[^\]]+\])*(?:\.[A-Za-z_]\w*(?:\[[^\]]+\])*)+)"
    )
    banned = {"if", "for", "while", "case", "else", "return", "switch", "true", "false"}
    out = []
    seen = set()
    for match in pat.finditer(text):
        raw = match.group(0)
        if raw in banned:
            continue
        sv = cpp_path_to_sv(raw)
        root = _strip_indices(sv.split(".")[0]) if sv else ""
        if root not in roots:
            continue
        if sv not in seen:
            seen.add(sv)
            out.append(sv)
    return out


def get_method_signal_width_hints(method_body, helpers, structs, module_type, type_widths):
    helper_text = "\n".join(helpers.values()) if helpers else ""
    text = (method_body or "") + "\n" + helper_text
    hints = []
    for path in extract_cpp_signal_paths(text, structs, module_type):
        width = resolve_sv_path_width(path, structs, module_type, type_widths)
        if width is not None:
            hints.append((path, width))
    return hints
