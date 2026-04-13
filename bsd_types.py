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
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[start:i - 1].strip()


def _try_eval_const_expr(expr, known_constants):
    expr = expr.strip()
    if not expr:
        return None
    if re.fullmatch(r"\d+", expr):
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


def _load_known_constants():
    # 当前版本只从 config.h 读取全局常量。
    # 这是实现层面的取舍：足够支撑 PRF/ROB，但并不是完整的 C/C++ 常量系统。
    config_path = os.path.join(SIMULATOR_INCLUDE, "config.h")
    if not os.path.exists(config_path):
        return {}

    with open(config_path) as f:
        content = f.read()

    known = {}
    raw_defs = {}
    for m in re.finditer(r'#define\s+([A-Z_]\w*)\s+(.+)', content):
        name = m.group(1)
        expr = m.group(2).split("//")[0].strip()
        if not expr or "(" in name:
            continue
        if any(t in expr for t in ['"', "'", "{", "}"]):
            continue
        raw_defs[name] = expr

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


def _parse_file_structs(content, existing_type_widths=None):
    # 先解析 primitive typedef 宽度，再解析 struct/class 字段。
    # 顺序很重要：后续字段如果引用了前面定义的 alias，需要先把位宽表建起来。
    type_widths = dict(existing_type_widths or {})
    structs = {}
    struct_sources = {}

    prim_widths = {
        "bool": 1,
        "uint8_t": 8,
        "uint16_t": 16,
        "uint32_t": 32,
        "uint64_t": 64,
    }
    for m in re.finditer(r"typedef\s+(\w+)\s+(\w+)\s*;", content):
        base_type, alias = m.group(1), m.group(2)
        if base_type in prim_widths:
            wm = re.match(r"(?:wire|reg)(\d+)_t", alias)
            type_widths[alias] = int(wm.group(1)) if wm else prim_widths[base_type]
        elif base_type in type_widths:
            type_widths[alias] = type_widths[base_type]

    struct_pat = re.compile(r"typedef\s+struct\s*(?:\w+)?\s*\{([^}]*)\}\s*(\w+)\s*;", re.DOTALL)
    for m in struct_pat.finditer(content):
        body, name = m.group(1), m.group(2)
        fields = _parse_struct_fields(body, type_widths)
        structs[name] = fields
        struct_sources[name] = m.group(0).strip()

    class_pat = re.compile(r"class\s+(\w+)\s*\{([\s\S]*?)\}\s*;", re.DOTALL)
    for m in class_pat.finditer(content):
        name, body = m.group(1), m.group(2)
        body = re.sub(r"public:|private:|protected:", "", body)
        lines = [line for line in body.split("\n") if "(" not in line and line.strip()]
        cleaned = "\n".join(lines)
        fields = _parse_struct_fields(cleaned, type_widths)
        if fields:
            structs[name] = fields
            struct_sources[name] = m.group(0).strip()

    return type_widths, structs, struct_sources


def _parse_struct_fields(body, type_widths):
    # 字段解析只覆盖当前项目里常见的“简单声明 + 数组维度”模式。
    # 复杂模板、函数指针、宏展开类型等都不在这个解析器的目标范围内。
    fields = []
    for raw in body.split("\n"):
        line = raw.strip()
        if not line or line.startswith("//") or line.startswith("#"):
            continue

        m = re.match(r"(\w+)\s+(.+);", line)
        if not m:
            continue
        type_name = m.group(1)
        rest = m.group(2).strip()

        if type_name in ("int64_t", "int", "int32_t", "uint64_t", "uint32_t", "uint16_t", "uint8_t", "char"):
            if type_name not in type_widths:
                continue

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
            width = type_widths.get(type_name)
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
        parts = [re.sub(r"\[\w+\]", "", part) for part in clean.split(".")]
        leaf = parts[-1]
        if leaf not in all_field_names and leaf not in type_widths:
            errors.append(f"Unknown leaf field '{leaf}' in: {path}")
    if errors:
        for err in errors:
            logger.error(err)
        raise ValueError(
            f"{len(errors)} unresolved signal paths in {module_type}. "
            f"Check simulator_include/ for missing struct definitions.\n" + "\n".join(errors[:10])
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
    config_path = os.path.join(SIMULATOR_INCLUDE, "config.h")
    if not os.path.exists(config_path):
        return {}
    constants = {}
    with open(config_path) as f:
        for raw in f:
            line = raw.strip()
            if not line.startswith("#define "):
                continue
            parts = line.split(None, 2)
            if len(parts) < 3:
                continue
            name = parts[1]
            expr = parts[2].split("//")[0].strip()
            if not expr or "(" in name:
                continue
            constants[name] = expr
    return constants


def _cpp_type_to_sv(type_name, width, structs):
    if type_name in structs:
        return f"{type_name}_t"
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


def generate_cpp_type_sources(struct_sources, ordered_structs):
    parts = []
    for sname in ordered_structs:
        src = struct_sources.get(sname, "")
        if src:
            parts.extend([src, ""])
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
            packed_dims = _format_sv_packed_dims(field.get("array_dims"))
            field_lines.append(f"    {sv_type}{packed_dims} {fname};")
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


def generate_sv_var_declarations(structs, module_type):
    # 顶层变量声明分成三组返回：
    # - 输入根信号
    # - 输出根信号
    # - 模块内部状态
    # 后续 prompt、snippet wrapper、combine 都按这个分组使用。
    in_class = f"{module_type}_IN"
    out_class = f"{module_type}_OUT"

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

    for field in structs.get(f"{module_type}_IN", []):
        add_var(f"in_{escape_sv_keyword(field['name'])}", field)
    for field in structs.get(f"{module_type}_OUT", []):
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
