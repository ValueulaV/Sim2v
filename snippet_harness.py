"""Snippet-stage harness construction, target planning, and debug prompt helpers."""

import json
import os
import re
from itertools import product

import bsd_analyzer
import combine_helpers
import signal_debug
from sv_path import escape_sv_keyword


SNIPPET_DEBUG_PROMPT = """\
You are fixing one SystemVerilog method-body snippet translated from C++.

Task:
- Fix ONLY `{module_type}::{method_name}`.
- Your output will be pasted inside:
  `always_comb begin`
  `    begin : {method_name}`
  `        // YOUR CODE`
  `    end`
  `end`
- Return ONLY one ```systemverilog``` code block.

Hard rules:
- Output statements only. No `module`, `always_*`, `typedef`, `function`, `assign`.
- Do NOT wrap the whole answer with a top-level `begin/end`.
- You MAY declare local variables, but initialize them unconditionally.
- If you declare locals, place all declarations before any executable statements in the block.
- Do not create extra named blocks such as `begin : helper_locals`.
- Do not declare unpacked-array locals inside procedural code.
- If a translated C++ local would otherwise be used after conditional assignment, give it an explicit safe default.
- In procedural code, declare loop variables separately as `integer` or `int` locals at the top of the block, then use them in `for (...)`.
  Never use `genvar`, and avoid header declarations like `for (int i = 0; ...)`.
- Fix compile/lint errors first. Prefer the smallest change that removes the current error before changing behavior.
- Never use `signal = (signal + 1) % 32`-style code on narrow pointers/counters when it can trigger width warnings.
  Use an if/else wraparound or an explicitly widened temporary.
- Use escaped SV field names exactly as declared, for example `type_v`, never `type`.
- Do NOT re-implement framework-owned default init or pi/po packing.
- In snippet-stage, compared outputs are already provided as free inputs when needed.
  Do not invent `*_1 = *` or whole-structure copy logic unless the C++ method
  explicitly does that.
- Do not invent carry/default relationships just from names. Treat similarly named signals
  as independent unless the C++ method explicitly connects them.
- Preserve update semantics exactly:
  - `x++`, `++x`, `x += y`, `x--`, `--x`, `x -= y` update `x` itself.
  - If the C++ writes `x_1++` or `x_1--`, update `x_1`, not `x`.
  - Example: C++ `count_1--;` means `count_1 = count_1 - 1;`, not `count_1 = count - 1;`.
- If the C++ does not assign a signal on some path, do not invent a replacement value from another signal
  with a similar name. Leave unrelated signals untouched unless the C++ explicitly writes them.
- Preserve the C++ behavior exactly.
- Keep the fix within a yosys-friendly synthesizable SystemVerilog subset. Avoid simulator-only or highly dynamic constructs.
- Do not use `automatic` locals.
- Do not use `break`, `continue`, or `disable` to exit loops. Rewrite with done-flags or gated execution.
- Do not use size-cast syntax like `4'(expr)` or `int'(expr)`. Use width-safe literals, concatenation, masks, or slices instead.
- Avoid direct dynamic field access on aggregate arrays such as `arr[idx].field` when `arr` is an array of structs or packed aggregates.
  Preferred yosys/verilator-safe patterns:
  - Read: `tmp = arr[idx]; x = tmp.field;`
  - Write: `tmp = arr[idx]; tmp.field = ...; arr[idx] = tmp;`
  - Or use constant-bound loops / case statements that select whole elements, then access fields on the selected temporary.
- Yosys requires procedural `for` loops to have compile-time-constant init/condition/step expressions.
  If the C++ loop start/end depends on a runtime signal, rewrite it as a constant-bound loop and
  guard the body with `if (...)` instead of putting the runtime expression directly in the loop header.
- Use only typedef names that already appear in the provided type context. Do not invent new `_t` type names.

Existing variables already declared by framework:
Inputs:
```systemverilog
{input_vars}
```

Internals:
```systemverilog
{internal_vars}
```

Outputs:
```systemverilog
{output_vars}
```

Relevant constants:
```text
{constants_block}
```

Type context:
```cpp
{cpp_type_sources}
```

```systemverilog
{sv_typedefs}
```

C++ source of truth:
```cpp
void {module_type}::{method_name}() {{
{method_body}
}}
```

Helper functions:
```cpp
{helpers_block}
```

Signals checked by snippet-stage verifier:
```text
{compare_targets}
```

Signal-agnostic harness inputs (free variables driven by verifier):
```text
{input_targets}
```

Current failing snippet:
```systemverilog
{current_snippet}
```

Verification failure:
```text
{verify_message}
```

Extra hint:
```text
{extra_hint}
```
"""


def build_method_io_targets(module_info, method_name, max_targets):
    # 这一步决定 snippet 要“看什么输入、比什么输出”。
    # 策略上采用：
    # - 读集合 -> 自由输入
    # - 写集合 -> 比较目标
    # 实现上则是 libclang 读写集 + 文本回退 + 精细路径修正的组合。
    method = next((m for m in module_info["methods"] if m["name"] == method_name), None)
    method_body = _expand_local_aliases(method["body"] if method else "")
    helpers = bsd_analyzer.extract_method_helpers(
        method_body,
        bsd_analyzer.parse_helper_functions(),
    ) if method else {}
    cpp_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "io_generator",
        "simulator_include",
        f"{module_info['module_type']}_cpp.h",
    )
    rw = signal_debug._extract_rw_libclang_file(cpp_path, [method_name])
    reads = sorted((rw or {}).get(method_name, {}).get("reads", set()))
    writes = sorted((rw or {}).get(method_name, {}).get("writes", set()))
    writes = sorted(set(writes) | set(_fallback_write_targets(module_info, method_name, method_body)))
    reads = sorted(set(reads) | set(_fallback_read_targets(module_info, method_name, method_body)))
    exact_paths = _extract_precise_signal_paths(method_body + "\n" + "\n".join(helpers.values()))
    reads = _refine_targets_with_exact_paths(reads, exact_paths)
    writes = _refine_targets_with_exact_paths(writes, exact_paths)
    reads = _supplement_scalar_root_reads(module_info, method_body + "\n" + "\n".join(helpers.values()), reads)
    output_targets = _filter_targets(module_info, writes, max_targets=max_targets, allow_inputs=False)
    input_targets = _filter_targets(module_info, reads, max_targets=max_targets * 4, allow_inputs=True)
    return _merge_targets(input_targets, output_targets), output_targets


def build_signal_plan(module_info, targets, instance_name, loop_domains=None, respect_dependencies=False):
    # signal plan 会把较粗的 target 路径展开成“可直接打包/比较”的叶子信号列表。
    # 后续的 SV wrapper、C++ reference、mismatch 定位都依赖这份 plan。
    leaves = []
    for target in targets:
        leaves.extend(_expand_target_path(module_info, target, instance_name, loop_domains=loop_domains))

    dedup = []
    seen = set()
    for leaf in leaves:
        key = (leaf["label"], leaf["width"])
        if key in seen:
            continue
        seen.add(key)
        dedup.append(leaf)

    if respect_dependencies:
        dedup = _order_signal_leaves_by_dependency(dedup)

    signal_map = []
    offset = 0
    for leaf in dedup:
        leaf["sv_expr"] = _sv_leaf_expr(module_info, leaf["label"])
        signal_map.append({"path": leaf["label"], "width": leaf["width"], "offset": offset})
        leaf["offset"] = offset
        offset += leaf["width"]
    width = max(1, offset)
    return {
        "targets": targets,
        "leaves": dedup,
        "signal_map": signal_map,
        "pi_width": width,
        "po_width": width,
    }


def build_sv_wrapper(module_name, combine_info, method_name, snippet_code, input_plan, compare_plan):
    # 单方法 SV wrapper 的职责非常明确：
    # 1) 复用完整模块的 typedef / 变量声明
    # 2) 把 input_plan 映射到 pi[]
    # 3) 在 begin : method_name 中嵌入 snippet
    # 4) 把 compare_plan 打包回 po[]
    # 它不是完整模块，只是为了让 method body 在 snippet 阶段可编译、可对拍。
    input_decls, output_decls, internal_decls = combine_info["var_decls"]
    parts = [
        f"module {module_name} (",
        f"    input  wire [{input_plan['pi_width'] - 1}:0] pi,",
        f"    output logic [{compare_plan['po_width'] - 1}:0] po",
        ");",
        "",
        combine_info["sv_typedefs"],
        "",
    ]
    for cname, cval in combine_info.get("sv_constants", []):
        parts.append(f"localparam int {cname} = {cval};")
    if combine_info.get("sv_constants"):
        parts.append("")
    parts.extend([
        *input_decls,
        "",
        *internal_decls,
        "",
        *output_decls,
        "",
        "always_comb begin",
    ])
    parts.extend(combine_helpers.build_default_assignments(
        [*input_decls, *internal_decls, *output_decls],
        strategy="zero_all",
    ))
    parts.extend([
        "    // ---- Input extraction from pi[] ----",
        _build_sv_input_unpack(input_plan),
        "",
        f"    begin : {method_name}",
    ])
    parts.extend([f"        {line}" if line.strip() else "" for line in _normalize_snippet(snippet_code)])
    parts.extend([
        "    end",
        "",
        f"    for (int __po_i = 0; __po_i < {compare_plan['po_width']}; __po_i++) begin",
        "        po[__po_i] = 1'b0;",
        "    end",
    ])
    for leaf in compare_plan["leaves"]:
        if leaf["width"] == 1:
            parts.append(f"    po[{leaf['offset']}] = {leaf['sv_expr']};")
        else:
            parts.append(f"    po[{leaf['offset']} +: {leaf['width']}] = {leaf['sv_expr']};")
    parts.extend(["end", "", "endmodule"])
    return "\n".join(parts)


def build_cpp_reference(*, wrapper_text, module_type, instance_name, method_name, input_plan, compare_plan):
    # 这里生成的不是原始 simulator 头文件，而是“只执行当前 method”的参考壳。
    # 它和 SV wrapper 共用同一份 input_plan / compare_plan，
    # 从而保证两边观察到的输入输出边界完全一致。
    header_guard = f"SNIPPET_REF_{module_type}_{method_name}".upper()
    header_prefix = _extract_wrapper_includes(wrapper_text)
    body_match = re.search(r"void\s+io_generator_outer\s*\([^)]*\)\s*\{([\s\S]*)\}\s*#endif", wrapper_text)
    if not body_match:
        raise ValueError("Failed to parse io_generator_outer body from wrapper")
    body = body_match.group(1)
    pi_call = f"{instance_name}.pi_to_simulator(pi);"
    pi_pos = body.find(pi_call)
    if pi_pos < 0:
        raise ValueError(f"Failed to locate `{pi_call}` in wrapper")
    prefix = body[:pi_pos]
    lines = [
        header_prefix.rstrip(),
        f"#ifndef {header_guard}",
        f"#define {header_guard}",
        f"extern const int PI_WIDTH = {input_plan['pi_width']};",
        f"extern const int PO_WIDTH = {compare_plan['po_width']};",
        "static inline uint64_t snippet_read_bits(const bool* pi, int& cursor, int width) {",
        "    uint64_t value = 0;",
        "    for (int i = 0; i < width; ++i) {",
        "        if (pi[cursor++]) value |= (1ULL << i);",
        "    }",
        "    return value;",
        "}",
        "static inline void snippet_pack_bits(bool* po, int& cursor, int width, uint64_t value) {",
        "    for (int i = 0; i < width; ++i) po[cursor++] = ((value >> i) & 1ULL) != 0;",
        "}",
        "void io_generator_outer(bool* pi, bool* po) {",
        prefix.rstrip(),
        "    int cursor = 0;",
        _build_cpp_input_unpack(input_plan),
        f"    {instance_name}.{method_name}();",
        "    for (int i = 0; i < PO_WIDTH; ++i) po[i] = false;",
        "    cursor = 0;",
    ]
    for leaf in compare_plan["leaves"]:
        lines.append(
            f"    snippet_pack_bits(po, cursor, {leaf['width']}, "
            f"static_cast<uint64_t>({leaf['cpp_expr']}));"
        )
    lines.extend(["}", "#endif"])
    return "\n".join(lines) + "\n"


def build_debug_prompt(*, module_info, method, method_ctx, input_plan, compare_plan, current_snippet, verify_message):
    # debug prompt 不追求上下文越多越好，而是强调“最小但闭环”：
    # - 当前 method 的源代码和 helper
    # - 当前 snippet
    # - 当前失败信息
    # - 当前 harness 观察到的输入/输出边界
    input_vars, output_vars, internal_vars = method_ctx["var_decls"]
    constants_block = "\n".join(f"{k} = {v}" for k, v in sorted(method_ctx["constants"].items())) or "(none)"
    helpers_block = "\n\n".join(method_ctx["helpers"].values()) or "(none)"
    extra_hint = _debug_extra_hint(verify_message)
    return SNIPPET_DEBUG_PROMPT.format(
        module_type=module_info["module_type"],
        method_name=method["name"],
        input_vars="\n".join(input_vars),
        internal_vars="\n".join(internal_vars),
        output_vars="\n".join(output_vars),
        constants_block=constants_block,
        cpp_type_sources=method_ctx["cpp_type_sources"] or "(none)",
        sv_typedefs=method_ctx["sv_typedefs"] or "(none)",
        method_body=method["body"],
        helpers_block=helpers_block,
        compare_targets="\n".join(compare_plan["targets"]) or "(none)",
        input_targets="\n".join(input_plan["targets"]) or "(none)",
        current_snippet=current_snippet,
        verify_message=verify_message,
        extra_hint=extra_hint,
    )


def _debug_extra_hint(verify_message):
    msg = verify_message or ""
    if "procedural for-loop" in msg and "not constant" in msg:
        return (
            "The failing `for` loop header contains a runtime-dependent expression. "
            "Do not try to fix this by adding constants. Rewrite it as a constant-bound loop "
            "over the full legal range, then guard the body with `if (...)`."
        )
    return "(none)"


def parse_instance_name(wrapper_text, module_type):
    match = re.search(rf"\b{re.escape(module_type)}\s+([A-Za-z_]\w*)\s*=\s*\{{\s*\}}\s*;", wrapper_text)
    return match.group(1) if match else module_type.lower()


def summarize_verify(message, max_lines=8):
    if not message:
        return ["<empty verify message>"]
    lines = [line.strip() for line in message.splitlines() if line.strip()]
    return lines[:max_lines]


def make_unique_dir(parent, base_name):
    os.makedirs(parent, exist_ok=True)
    for i in range(1000):
        name = base_name if i == 0 else f"{base_name}_{i:03d}"
        path = os.path.join(parent, name)
        try:
            os.makedirs(path, exist_ok=False)
            return name, path
        except FileExistsError:
            continue
    raise RuntimeError(f"Failed to create unique dir under {parent}")


def write_text(path, text):
    with open(path, "w") as f:
        f.write(text)


def write_json(path, data):
    with open(path, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def collect_loop_domains(text):
    domains = {}
    pat = re.compile(
        r"for\s*\(\s*(?:int|integer)\s+([A-Za-z_]\w*)\s*=\s*([^;]+);\s*"
        r"\1\s*(<=|<)\s*([^;]+);\s*(?:\+\+\1|\1\+\+|\1\s*\+=\s*1)\s*\)"
    )
    for match in pat.finditer(text or ""):
        name, start_expr, cmp_op, end_expr = match.groups()
        start = bsd_analyzer._try_eval_const_expr(start_expr.strip(), bsd_analyzer.KNOWN_CONSTANTS)
        end = bsd_analyzer._try_eval_const_expr(end_expr.strip(), bsd_analyzer.KNOWN_CONSTANTS)
        if start is None or end is None:
            continue
        stop = end if cmp_op == "<" else end + 1
        if stop > start:
            domains[name] = list(range(start, stop))
    return domains


def _merge_targets(primary, extra):
    merged = []
    for path in list(primary) + list(extra):
        if path and path not in merged:
            merged.append(path)
    return merged


def _supplement_scalar_root_reads(module_info, text, reads):
    merged = list(reads)
    existing = set(merged)
    for name in sorted(_root_scalar_names(module_info)):
        if name not in existing and re.search(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])", text):
            merged.append(name)
            existing.add(name)
    return merged


def _fallback_write_targets(module_info, method_name, method_body=None):
    # 当 libclang 没提到某些写路径时，这里用文本级赋值分析做保守补齐。
    # 它不如 AST 精确，但能覆盖不少“简单赋值却未被主通道捕捉”的情况。
    method = next((m for m in module_info["methods"] if m["name"] == method_name), None)
    if not method:
        return []
    body = _expand_local_aliases(method_body if method_body is not None else method["body"])
    out = set()
    for line in body.splitlines():
        stmt = line.strip()
        if not stmt:
            continue
        op = _find_assignment_operator(stmt)
        if not op:
            continue
        lhs = stmt[:op[0]].strip()
        lhs_paths = _extract_precise_signal_paths(lhs)
        if lhs_paths:
            canon = _canonical_target_path(max(lhs_paths, key=len))
            if canon:
                out.add(canon)
    return sorted(out)


def _fallback_read_targets(module_info, method_name, method_body=None):
    # read-set 的文本补齐比 write-set 更激进一些：
    # 除了宽度提示外，还会把精确路径扫描到的信号也纳入候选，
    # 以降低 snippet 输入不够导致的假 mismatch。
    method = next((m for m in module_info["methods"] if m["name"] == method_name), None)
    if not method:
        return []
    body = _expand_local_aliases(method_body if method_body is not None else method["body"])
    helpers = bsd_analyzer.extract_method_helpers(body, bsd_analyzer.parse_helper_functions())
    hints = bsd_analyzer.get_method_signal_width_hints(
        body,
        helpers,
        module_info["structs"],
        module_info["module_type"],
        module_info["type_widths"],
    )
    out = [_canonical_target_path(path) for path, _ in hints]
    local_decl_pat = re.compile(r"\b(?:int|bool|wire\d+_t|reg\d+_t)\s+([A-Za-z_]\w*)")
    local_vars = set(local_decl_pat.findall(body))
    for raw in _extract_precise_signal_paths(body):
        if raw.split(".", 1)[0].split("->", 1)[0] in local_vars:
            continue
        canon = _canonical_target_path(raw)
        if canon:
            out.append(canon)
    return sorted(set(out))


def _filter_targets(module_info, raw, *, max_targets, allow_inputs):
    filtered = []
    for path in raw:
        if not path:
            continue
        if path.startswith("in.") and not allow_inputs:
            continue
        if not allow_inputs and re.fullmatch(r"[A-Za-z_]\w*", path):
            filtered.append(path)
            continue
        root = _strip_all_indices(_split_path_tokens(path)[0])
        if path.startswith("in.") or path.startswith("out.") or root in _root_signal_names(module_info):
            filtered.append(path)

    kept = []
    for path in sorted(set(filtered), key=lambda item: (-len(_split_path_tokens(item)), item)):
        if any(_is_prefix(path, existing) for existing in kept):
            continue
        kept.append(path)
        if len(kept) >= max_targets:
            break
    return kept


def _refine_targets_with_exact_paths(coarse_targets, exact_paths):
    if not exact_paths:
        return coarse_targets
    refined = []
    exact_by_coarse = {}
    for path in exact_paths:
        coarse = signal_debug._canonical_path(path)
        if coarse:
            exact_by_coarse.setdefault(coarse, []).append(_framework_path(path))

    for coarse in coarse_targets:
        matches = []
        for exact_coarse, exact_list in exact_by_coarse.items():
            if exact_coarse == coarse or exact_coarse.startswith(coarse + "."):
                matches.extend(exact_list)
        refined.extend(matches or [coarse])
    return sorted(set(refined))


def _framework_path(path):
    path = (path or "").strip().replace("->", ".")
    if path.startswith("in_"):
        path = "in." + path[3:]
    elif path.startswith("out_"):
        path = "out." + path[4:]
    return path


def _root_scalar_names(module_info):
    names = set()
    for field in module_info["structs"].get(module_info["module_type"], []):
        if field["name"] in ("in", "out") or field.get("width") is None or field.get("array_dims"):
            continue
        names.add(field["name"])
    return names


def _root_signal_names(module_info):
    return {
        field["name"]
        for field in module_info["structs"].get(module_info["module_type"], [])
        if field["name"] not in ("in", "out")
    }


def _module_interface_type(module_info, field_name):
    for field in module_info["structs"].get(module_info["module_type"], []):
        if field["name"] == field_name:
            return field.get("type")
    legacy = f"{module_info['module_type']}_{field_name.upper()}"
    if legacy in module_info["structs"]:
        return legacy
    camel = f"{module_info['module_type']}{field_name.capitalize()}"
    if camel in module_info["structs"]:
        return camel
    return None


def _order_signal_leaves_by_dependency(leaves):
    # compare_plan 里的叶子信号有时会互相依赖，例如某个数组索引本身就是另一个信号。
    # 这里做一个轻量的拓扑排序，尽量保证“先解依赖、后用依赖”。
    if len(leaves) <= 1:
        return leaves
    known = {leaf["sv_expr"] for leaf in leaves}
    remaining = [dict(leaf, _deps=_leaf_dependencies(leaf["sv_expr"], known)) for leaf in leaves]
    ordered = []
    emitted = set()
    while remaining:
        progressed = False
        next_remaining = []
        for leaf in remaining:
            deps = {dep for dep in leaf["_deps"] if dep in known}
            if deps.issubset(emitted):
                out_leaf = dict(leaf)
                out_leaf.pop("_deps", None)
                ordered.append(out_leaf)
                emitted.add(out_leaf["sv_expr"])
                progressed = True
            else:
                next_remaining.append(leaf)
        if not progressed:
            for leaf in remaining:
                out_leaf = dict(leaf)
                out_leaf.pop("_deps", None)
                ordered.append(out_leaf)
            break
        remaining = next_remaining
    return ordered


def _leaf_dependencies(sv_expr, known_exprs):
    deps = set()
    for idx_expr in re.findall(r"\[([^\[\]]+)\]", sv_expr or ""):
        for path in _extract_precise_signal_paths(idx_expr):
            dep_expr = _sv_path_expr(path)
            if dep_expr in known_exprs and dep_expr != sv_expr:
                deps.add(dep_expr)
    return deps


def _is_prefix(prefix, path):
    return path == prefix or path.startswith(prefix + ".")


def _build_roots(module_info, instance_name):
    # roots 是所有路径展开的起点，统一描述三类根：
    # - in.*
    # - out.*
    # - 模块内部状态
    # 后面的路径展开都建立在这份“根节点表”上。
    roots = {}
    structs = module_info["structs"]
    in_type = _module_interface_type(module_info, "in")
    out_type = _module_interface_type(module_info, "out")
    for field in structs.get(in_type, []):
        key = f"in.{field['name']}"
        roots[key] = {
            "type": field.get("type"),
            "width": field.get("width"),
            "array_dims": field.get("array_dims") or [],
            "sv_expr": f"in_{escape_sv_keyword(field['name'])}",
            "cpp_expr": f"{instance_name}.in.{field['name']}",
            "label": key,
            "child_sep": "->",
        }
    for field in structs.get(out_type, []):
        key = f"out.{field['name']}"
        roots[key] = {
            "type": field.get("type"),
            "width": field.get("width"),
            "array_dims": field.get("array_dims") or [],
            "sv_expr": f"out_{escape_sv_keyword(field['name'])}",
            "cpp_expr": f"{instance_name}.out.{field['name']}",
            "label": key,
            "child_sep": "->",
        }
    for field in structs.get(module_info["module_type"], []):
        if field["name"] in ("in", "out"):
            continue
        roots[field["name"]] = {
            "type": field.get("type"),
            "width": field.get("width"),
            "array_dims": field.get("array_dims") or [],
            "sv_expr": escape_sv_keyword(field["name"]),
            "cpp_expr": f"{instance_name}.{field['name']}",
            "label": field["name"],
            "child_sep": ".",
        }
    return roots


def _sv_leaf_expr(module_info, label):
    # 对于最终要打包到 po[] 的叶子路径，这里把“语义路径”降成 SV 可取值表达式。
    # 如果路径落在 packed struct 的子字段里，需要额外计算 bit offset。
    roots = _build_roots(module_info, instance_name="")
    parts = _split_path_tokens(label)
    if not parts:
        return label

    if parts[0] in ("in", "out") and len(parts) >= 2:
        root_name, root_indices = _split_indexed_token(parts[1])
        root_key = f"{parts[0]}.{root_name}"
        remain = parts[2:]
    else:
        root_name, root_indices = _split_indexed_token(parts[0])
        root_key = root_name
        remain = parts[1:]

    root = roots.get(root_key)
    if not root:
        return _sv_path_expr(label)

    base_expr = root["sv_expr"]
    root_dims = list(root.get("array_dims") or [])
    for expr, dim in zip(root_indices, root_dims):
        base_expr += f"[{_sv_array_index_expr(expr, width=_index_bit_width(dim))}]"

    if not remain:
        return base_expr

    lsb_expr, leaf_width = _packed_subpath_offset(
        module_info,
        root.get("type"),
        root.get("width"),
        remain,
    )
    if lsb_expr is None or leaf_width is None:
        return _sv_path_expr(label)
    if int(leaf_width) == 1:
        return f"{base_expr}[{lsb_expr}]"
    return f"{base_expr}[{lsb_expr} +: {int(leaf_width)}]"


def _packed_subpath_offset(module_info, type_name, width, tokens):
    # packed struct / packed array 的子字段访问，在 snippet wrapper 里最终要转成位切片。
    # 这个函数负责递归计算“从根 packed 对象起，目标字段的 lsb 偏移和叶子宽度”。
    if not tokens:
        total_width = _packed_total_width(module_info, type_name, width, [])
        return ("0", total_width) if total_width is not None else (None, None)

    if type_name not in module_info["structs"]:
        return None, None

    token_name, token_indices = _split_indexed_token(tokens[0])
    fields = module_info["structs"][type_name]
    field_idx = next((idx for idx, item in enumerate(fields) if item["name"] == token_name), None)
    if field_idx is None:
        return None, None

    offset_terms = []
    lsb_const = 0
    for later in fields[field_idx + 1:]:
        later_width = _packed_total_width(
            module_info,
            later.get("type"),
            later.get("width"),
            later.get("array_dims") or [],
        )
        if later_width is None:
            return None, None
        lsb_const += later_width
    if lsb_const:
        offset_terms.append(str(lsb_const))

    field = fields[field_idx]
    remaining_dims = list(field.get("array_dims") or [])
    if len(token_indices) > len(remaining_dims):
        return None, None

    for expr in token_indices:
        dim = int(remaining_dims[0])
        stride = _packed_total_width(
            module_info,
            field.get("type"),
            field.get("width"),
            remaining_dims[1:],
        )
        if stride is None:
            return None, None
        offset_terms.append(_mul_expr(_sv_packed_offset_expr(expr), stride))
        remaining_dims = remaining_dims[1:]

    if tokens[1:]:
        sub_lsb, sub_width = _packed_subpath_offset(
            module_info,
            field.get("type"),
            field.get("width"),
            tokens[1:],
        )
        if sub_lsb is None or sub_width is None:
            return None, None
        if sub_lsb != "0":
            offset_terms.append(sub_lsb)
        return _sum_expr(offset_terms), sub_width

    leaf_width = _packed_total_width(
        module_info,
        field.get("type"),
        field.get("width"),
        remaining_dims,
    )
    if leaf_width is None:
        return None, None
    return _sum_expr(offset_terms), leaf_width


def _packed_total_width(module_info, type_name, width, array_dims):
    # 统一计算一个类型（可能是 primitive / struct / packed array）的总位宽。
    # 这是 packed offset 计算、路径宽度推导和 wrapper 切片生成的共同基础。
    if type_name in module_info["structs"]:
        total = 0
        for field in module_info["structs"][type_name]:
            field_width = _packed_total_width(
                module_info,
                field.get("type"),
                field.get("width"),
                field.get("array_dims") or [],
            )
            if field_width is None:
                return None
            total += field_width
    elif width is not None:
        total = int(width)
    elif type_name in (module_info.get("type_widths") or {}):
        total = int(module_info["type_widths"][type_name])
    elif type_name == "bool":
        total = 1
    else:
        return None

    for dim in array_dims or []:
        total *= int(dim)
    return total


def _mul_expr(expr, factor):
    factor = int(factor)
    if factor == 0:
        return "0"
    if factor == 1:
        return expr
    return f"(({expr}) * {factor})"


def _sum_expr(terms):
    filtered = [term for term in terms if term and term != "0"]
    if not filtered:
        return "0"
    if len(filtered) == 1:
        return filtered[0]
    return "(" + " + ".join(filtered) + ")"


def _expand_target_path(module_info, path, instance_name, loop_domains=None):
    # 把一个较粗的 target 路径展开成多个叶子路径。
    # 例如数组/结构体路径会递归下钻，必要时还会把循环变量替换成常量实例。
    roots = _build_roots(module_info, instance_name)
    parts = _split_path_tokens(path)
    if not parts:
        return []

    root_name, root_indices = _split_indexed_token(parts[0])
    if root_name in ("in", "out"):
        if len(parts) < 2:
            return []
        child_name, child_indices = _split_indexed_token(parts[1])
        root_key = f"{root_name}.{child_name}"
        remain = parts[2:]
        root_indices = child_indices
    else:
        root_key = root_name
        remain = parts[1:]

    node = roots.get(root_key)
    if not node:
        return []
    nodes = _apply_explicit_indices(
        [dict(node)],
        root_indices,
        runtime_ids=_runtime_index_ids(module_info),
        instance_name=instance_name,
        loop_domains=loop_domains or {},
    )
    out = []
    for cur in nodes:
        out.extend(_expand_target_recursive(
            module_info=module_info,
            type_name=cur.get("type"),
            width=cur.get("width"),
            array_dims=cur.get("array_dims") or [],
            sv_expr=cur["sv_expr"],
            cpp_expr=cur["cpp_expr"],
            label=cur["label"],
            child_sep=cur.get("child_sep", "."),
            tokens=remain,
            instance_name=instance_name,
            loop_domains=loop_domains,
        ))
    return out


def _expand_target_recursive(module_info, type_name, width, array_dims, sv_expr, cpp_expr,
                             label, child_sep, tokens, instance_name=None, loop_domains=None):
    # 这是 signal plan 展开的主递归：
    # - 先处理数组维度
    # - 再处理结构体字段
    # - 最终收敛到 width 已知的叶子信号
    # 返回值同时保留 SV 表达式、C++ 表达式和可读标签，供后续多处复用。
    if array_dims:
        first, rest = int(array_dims[0]), array_dims[1:]
        out = []
        for idx in range(first):
            out.extend(_expand_target_recursive(
                module_info,
                type_name,
                width,
                rest,
                f"{sv_expr}[{idx}]",
                f"{cpp_expr}[{idx}]",
                f"{label}[{idx}]",
                child_sep,
                tokens,
                instance_name=instance_name,
                loop_domains=loop_domains,
            ))
        return out

    if tokens:
        if type_name not in module_info["structs"]:
            return []
        token_name, token_indices = _split_indexed_token(tokens[0])
        field = next((item for item in module_info["structs"][type_name] if item["name"] == token_name), None)
        if not field:
            return []
        next_nodes = _apply_explicit_indices(
            [{
                "type": field.get("type"),
                "width": field.get("width"),
                "array_dims": field.get("array_dims") or [],
                "sv_expr": f"{sv_expr}.{escape_sv_keyword(field['name'])}",
                "cpp_expr": f"{cpp_expr}{child_sep}{field['name']}",
                "label": f"{label}.{field['name']}",
                "child_sep": ".",
            }],
            token_indices,
            runtime_ids=_runtime_index_ids(module_info),
            instance_name=instance_name,
            loop_domains=loop_domains or {},
        )
        out = []
        for cur in next_nodes:
            out.extend(_expand_target_recursive(
                module_info=module_info,
                type_name=cur.get("type"),
                width=cur.get("width"),
                array_dims=cur.get("array_dims") or [],
                sv_expr=cur["sv_expr"],
                cpp_expr=cur["cpp_expr"],
                label=cur["label"],
                child_sep=".",
                tokens=tokens[1:],
                instance_name=instance_name,
                loop_domains=loop_domains,
            ))
        return out

    if type_name in module_info["structs"] and width is None:
        out = []
        for field in module_info["structs"][type_name]:
            out.extend(_expand_target_recursive(
                module_info,
                field.get("type"),
                field.get("width"),
                field.get("array_dims") or [],
                f"{sv_expr}.{escape_sv_keyword(field['name'])}",
                f"{cpp_expr}{child_sep}{field['name']}",
                f"{label}.{field['name']}",
                ".",
                [],
                instance_name=instance_name,
                loop_domains=loop_domains,
            ))
        return out

    if width is None:
        return []
    return [{"label": label, "width": int(width), "sv_expr": sv_expr, "cpp_expr": cpp_expr}]


def _split_indexed_token(token):
    token = token.strip()
    match = re.match(r"^([A-Za-z_]\w*)", token)
    if not match:
        return token, []
    name = match.group(1)
    idxs = []
    i = len(name)
    while i < len(token):
        if token[i].isspace():
            i += 1
            continue
        if token[i] != "[":
            break
        depth = 1
        start = i + 1
        i += 1
        while i < len(token) and depth > 0:
            if token[i] == "[":
                depth += 1
            elif token[i] == "]":
                depth -= 1
            i += 1
        if depth != 0:
            break
        idxs.append(token[start:i - 1].strip())
    return name, idxs


def _split_path_tokens(path):
    parts = []
    cur = []
    depth = 0
    for ch in (path or "").strip():
        if ch == "." and depth == 0:
            part = "".join(cur).strip()
            if part:
                parts.append(part)
            cur = []
            continue
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth = max(0, depth - 1)
        cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        parts.append(tail)
    return parts


def _strip_all_indices(token):
    out = []
    depth = 0
    for ch in (token or "").strip():
        if ch == "[":
            depth += 1
            continue
        if ch == "]":
            depth = max(0, depth - 1)
            continue
        if depth == 0:
            out.append(ch)
    return "".join(out).strip()


def _runtime_index_ids(module_info):
    ids = set(_root_signal_names(module_info))
    for cls_name in (
        _module_interface_type(module_info, "in"),
        _module_interface_type(module_info, "out"),
    ):
        for field in module_info["structs"].get(cls_name, []):
            ids.add(f"in_{escape_sv_keyword(field['name'])}")
            ids.add(f"out_{escape_sv_keyword(field['name'])}")
            ids.add(field["name"])
    return ids


def _index_bit_width(dim):
    return max(1, (int(dim) - 1).bit_length())


def _apply_explicit_indices(nodes, index_exprs, runtime_ids, instance_name, loop_domains):
    # 显式下标有两种处理方式：
    # 1) 编译期可枚举的索引：直接展开成多个常量路径
    # 2) 运行时索引：保留成表达式，但要补上位宽约束
    # 这一步直接影响 snippet harness 的规模和可编译性。
    cur_nodes = nodes
    for expr in index_exprs:
        next_nodes = []
        for node in cur_nodes:
            for expr_variant in _expand_expr_variants(expr, loop_domains):
                dims = node.get("array_dims") or []
                if not dims:
                    next_nodes.append(node)
                    continue
                dim, rest = int(dims[0]), dims[1:]
                if _should_expand_index_expr(expr_variant, runtime_ids):
                    for idx in range(dim):
                        next_nodes.append({
                            **node,
                            "array_dims": rest,
                            "sv_expr": f"{node['sv_expr']}[{idx}]",
                            "cpp_expr": f"{node['cpp_expr']}[{idx}]",
                            "label": f"{node['label']}[{idx}]",
                        })
                else:
                    expr_bits = _index_bit_width(dim)
                    next_nodes.append({
                        **node,
                        "array_dims": rest,
                        "sv_expr": f"{node['sv_expr']}[{_sv_array_index_expr(expr_variant.strip(), width=expr_bits)}]",
                        "cpp_expr": f"{node['cpp_expr']}[{_cpp_index_expr(expr_variant.strip(), runtime_ids, instance_name, width=expr_bits)}]",
                        "label": f"{node['label']}[{expr_variant.strip()}]",
                    })
        cur_nodes = next_nodes
    return cur_nodes


def _should_expand_index_expr(expr, runtime_ids):
    expr = expr.strip()
    if re.fullmatch(r"\d+", expr):
        return False
    if re.fullmatch(r"[A-Za-z_]\w*", expr):
        return expr not in runtime_ids
    return False


def _cpp_index_expr(expr, runtime_ids, instance_name, width=None):
    # 把路径中的索引表达式翻成 C++ 侧可执行表达式。
    # 这里会把框架路径替换回 instance 成员访问，并按需要补 mask。
    expr = expr.strip()
    paths = sorted(_extract_precise_signal_paths(expr), key=len, reverse=True)
    if paths:
        out = expr
        for path in paths:
            out = out.replace(path, _cpp_path_expr(path, instance_name))
        return _mask_cpp_index_expr(out, width)
    if re.fullmatch(r"[A-Za-z_]\w*", expr) and expr in runtime_ids:
        if expr.startswith("in_") or expr.startswith("out_"):
            return _mask_cpp_index_expr(expr, width)
        return _mask_cpp_index_expr(f"{instance_name}.{expr}", width)
    def repl(match):
        tok = match.group(1)
        if tok not in runtime_ids:
            return tok
        if tok.startswith("in_") or tok.startswith("out_"):
            return tok
        return f"{instance_name}.{tok}"
    out = re.sub(r"(?<![A-Za-z0-9_\.])([A-Za-z_]\w*)(?=(?:\[|\.))", repl, expr)
    return _mask_cpp_index_expr(out, width)


def _mask_cpp_index_expr(expr, width):
    if not width:
        return expr
    if width >= 63:
        return f"(static_cast<size_t>({expr}))"
    mask = (1 << width) - 1
    return f"(static_cast<size_t>({expr}) & 0x{mask:x}u)"


def _sv_signal_expr(expr):
    out = expr
    for path in sorted(_extract_precise_signal_paths(expr), key=len, reverse=True):
        out = out.replace(path, _sv_path_expr(path))
    return out.strip()


def _sv_array_index_expr(expr, width=None):
    out = _sv_signal_expr(expr)
    if not width:
        return out
    return f"{width}'(({out}))"


def _sv_packed_offset_expr(expr):
    out = _sv_signal_expr(expr)
    if re.fullmatch(r"\d+", out):
        return out
    return f"({out})"


def _sv_path_expr(path):
    # 普通“路径字符串 -> SV 表达式”转换。
    # 与 _sv_leaf_expr 的区别是：这里不做 packed bit offset 计算，只保留结构化访问形式。
    tokens = _split_path_tokens(path.replace("->", "."))
    if not tokens:
        return path
    if tokens[0] in ("in", "out") and len(tokens) >= 2:
        root_name, root_indices = _split_indexed_token(tokens[1])
        head = f"{tokens[0]}_{escape_sv_keyword(root_name)}"
        for idx in root_indices:
            head += f"[{_sv_signal_expr(idx)}]"
        rest = tokens[2:]
    else:
        root_name, root_indices = _split_indexed_token(tokens[0])
        head = escape_sv_keyword(root_name)
        for idx in root_indices:
            head += f"[{_sv_signal_expr(idx)}]"
        rest = tokens[1:]
    cur = head
    for token in rest:
        name, indices = _split_indexed_token(token)
        cur += f".{escape_sv_keyword(name)}"
        for idx in indices:
            cur += f"[{_sv_signal_expr(idx)}]"
    return cur


def _cpp_path_expr(path, instance_name):
    tokens = _split_path_tokens(path.replace("->", "."))
    if not tokens:
        return path
    if tokens[0] in ("in", "out") and len(tokens) >= 2:
        root_name, root_indices = _split_indexed_token(tokens[1])
        cur = f"{instance_name}.{tokens[0]}.{root_name}"
        for idx in root_indices:
            cur += f"[{_cpp_index_expr(idx, set(), instance_name)}]"
        first_rest = True
        for token in tokens[2:]:
            name, indices = _split_indexed_token(token)
            cur += ("->" if first_rest else ".") + name
            for idx in indices:
                cur += f"[{_cpp_index_expr(idx, set(), instance_name)}]"
            first_rest = False
        return cur
    root_name, root_indices = _split_indexed_token(tokens[0])
    cur = f"{instance_name}.{root_name}"
    for idx in root_indices:
        cur += f"[{_cpp_index_expr(idx, set(), instance_name)}]"
    for token in tokens[1:]:
        name, indices = _split_indexed_token(token)
        cur += f".{name}"
        for idx in indices:
            cur += f"[{_cpp_index_expr(idx, set(), instance_name)}]"
    return cur


def _normalize_snippet(snippet_code):
    # LLM 输出的 snippet 缩进层级不稳定。
    # 这里把公共最小缩进去掉，保证嵌入 wrapper 后格式可读，但不改变语义。
    lines = snippet_code.splitlines()
    non_empty = [line for line in lines if line.strip()]
    if not non_empty:
        return []
    min_indent = min(len(re.match(r" *", line).group(0)) for line in non_empty)
    return [line[min_indent:] if line.strip() else "" for line in lines]


def _extract_wrapper_includes(wrapper_text):
    lines = []
    for line in wrapper_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#include"):
            lines.append(line)
        elif (stripped.startswith("//") or not stripped) and not lines:
            lines.append(line)
        elif "PI_WIDTH" in stripped or stripped.startswith("#ifndef") or stripped.startswith("void io_generator_outer"):
            break
    return "\n".join(lines).rstrip()


def _build_sv_input_unpack(input_plan):
    # snippet 的自由输入统一通过 pi[] 驱动。
    # input_plan 已经给出每个叶子信号在 pi 中的 offset/width，这里只负责直译成赋值语句。
    lines = []
    for leaf in input_plan["leaves"]:
        if leaf["width"] == 1:
            lines.append(f"    {leaf['sv_expr']} = pi[{leaf['offset']}];")
        else:
            lines.append(f"    {leaf['sv_expr']} = pi[{leaf['offset']} +: {leaf['width']}];")
    return "\n".join(lines) if lines else "    // (no free inputs)"


def _build_cpp_input_unpack(input_plan):
    # C++ 参考壳与 SV wrapper 必须使用完全对称的输入解包逻辑，
    # 否则 snippet 对拍结果没有意义。
    lines = [
        f"    {leaf['cpp_expr']} = static_cast<uint64_t>(snippet_read_bits(pi, cursor, {leaf['width']}));"
        for leaf in input_plan["leaves"]
    ]
    return "\n".join(lines) if lines else "    // (no free inputs)"


def _expand_local_aliases(text):
    # C++ method 中常见 `foo_t *p = &obj.xxx;` / `auto &ref = ...;` 这类本地别名。
    # 后面的文本路径分析不理解“别名再取字段”，所以先把显式别名展开回原路径。
    # 这是启发式处理，不是完整 C++ 语义。
    if not text:
        return text
    aliases = {}
    for line in text.splitlines():
        match = re.match(
            r"^\s*(?:const\s+)?[\w:<>]+\s*([*&])\s*([A-Za-z_]\w*)\s*=\s*&?\s*([^;]+?)\s*;\s*$",
            line,
        )
        if match:
            _, alias, target = match.groups()
            aliases[alias] = target.strip()
    if not aliases:
        return text
    out = text
    changed = True
    while changed:
        changed = False
        for alias, target in aliases.items():
            new = re.sub(rf"\b{re.escape(alias)}\s*->", f"{target}.", out)
            new = re.sub(rf"\b{re.escape(alias)}\s*\.", f"{target}.", new)
            if new != out:
                changed = True
                out = new
    return out


def _find_assignment_operator(stmt):
    for match in re.finditer(r"(\+\+|--|\+=|-=|\*=|/=|%=|\|=|&=|\^=|=)", stmt):
        op = match.group(1)
        idx = match.start()
        if op == "=":
            prev = stmt[idx - 1] if idx > 0 else ""
            nxt = stmt[idx + 1] if idx + 1 < len(stmt) else ""
            if prev in ("=", "!", "<", ">") or nxt == "=":
                continue
        return idx, op
    return None


def _extract_precise_signal_paths(text):
    # 这是文本级的“精确路径扫描器”：
    # 它会保留数组下标和多级字段，尽量从 C++ 语句中还原出接近原样的信号路径。
    # snippet 阶段很多 fallback 逻辑都依赖这份更细粒度的路径信息。
    out = []
    seen = set()

    def add_path(path):
        canon = _canonical_target_path(path)
        if canon and canon not in seen:
            seen.add(canon)
            out.append(canon)

    i = 0
    n = len(text or "")
    while i < n:
        if not (text[i].isalpha() or text[i] == "_"):
            i += 1
            continue
        j = i + 1
        while j < n and (text[j].isalnum() or text[j] == "_"):
            j += 1
        expr = text[i:j]
        advanced = False
        k = j
        while True:
            while k < n and text[k].isspace():
                k += 1
            if text.startswith("->", k):
                k += 2
                while k < n and text[k].isspace():
                    k += 1
                if k >= n or not (text[k].isalpha() or text[k] == "_"):
                    break
                end = k + 1
                while end < n and (text[end].isalnum() or text[end] == "_"):
                    end += 1
                expr += "." + text[k:end]
                k = end
                advanced = True
                continue
            if k < n and text[k] == ".":
                end = k + 1
                while end < n and text[end].isspace():
                    end += 1
                if end >= n or not (text[end].isalpha() or text[end] == "_"):
                    break
                tail = end + 1
                while tail < n and (text[tail].isalnum() or text[tail] == "_"):
                    tail += 1
                expr += "." + text[end:tail]
                k = tail
                advanced = True
                continue
            if k < n and text[k] == "[":
                depth = 1
                end = k + 1
                while end < n and depth > 0:
                    if text[end] == "[":
                        depth += 1
                    elif text[end] == "]":
                        depth -= 1
                    end += 1
                if depth != 0:
                    break
                inside = text[k + 1:end - 1]
                expr += "[" + inside + "]"
                advanced = True
                for nested in _extract_precise_signal_paths(inside):
                    add_path(nested)
                k = end
                continue
            break
        if advanced:
            add_path(expr)
            i = k
        else:
            i = j
    return out


def _canonical_target_path(path):
    path = signal_debug._strip_bit((path or "").strip())
    path = path.replace("->", ".")
    path = re.sub(r"\s+", "", path)
    path = re.sub(r"^(in|out)_", r"\1.", path)
    path = re.sub(r"^[A-Za-z_]\w+\.(in|out)\.", r"\1.", path)
    return re.sub(r"\.+", ".", path).strip(".")


def _expand_expr_variants(expr, loop_domains):
    expr = expr.strip()
    if not expr:
        return [expr]
    ids = []
    for tok in re.findall(r"\b[A-Za-z_]\w*\b", expr):
        if tok in loop_domains and tok not in ids:
            ids.append(tok)
    if not ids:
        return [expr]
    variants = []
    for combo in product(*[loop_domains[name] for name in ids]):
        cur = expr
        for name, value in zip(ids, combo):
            cur = re.sub(rf"\b{re.escape(name)}\b", str(value), cur)
        variants.append(cur)
    return variants
