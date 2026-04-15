"""Build LLM prompts for bsd module methods, output as JSONL.

Divide-and-conquer: each translated method body becomes an independent sub-task.
LLM is only asked to generate method-body statements (not modules/typedefs/framework code).
"""

import os
import json
import re

import io_mapping

from bsd_analyzer import (
    analyze_module, build_helper_db, generate_sv_typedefs, generate_sv_var_declarations,
    extract_method_helpers,
    parse_all_constants, get_struct_order_for_method, generate_cpp_type_sources,
    get_method_signal_width_hints, KNOWN_CONSTANTS, _try_eval_const_expr,
    project_context_for_logic,
)

# ---- Prompt constants ----

INFER_ROLE_INTRO = "You translate C++ cycle-level simulator logic into SystemVerilog."

FRAMEWORK_GUARANTEE_TEMPLATE = """\
Framework facts (do NOT re-implement):
- All `in_*`, `out_*`, and internal signals are already declared by the wrapper module.
- The wrapper `always_comb` already does: default-init of writable vars, input extraction from `pi[]`,
  calls methods in order, and output packing to `po[]`.
- Your output will be pasted directly inside an existing `begin : {method_name} ... end` wrapper block.
  Do NOT add another top-level `begin/end` wrapper (named or unnamed).
- You must only implement the logic that belongs to `{module_type}::{method_name}`."""

FRAMEWORK_SKELETON_TEMPLATE = """\
Framework skeleton (DO NOT output this wrapper; it's shown only to avoid misunderstanding):
```systemverilog
always_comb begin
    // ... framework defaults + input extraction already done ...
    begin : {method_name}
        // === YOUR OUTPUT IS INSERTED HERE ===
        // (statements only; no top-level begin/end)
    end
    // ... other methods + output packing handled by framework ...
end
```"""

def _infer_output_block(use_think):
    if use_think:
        return (
            "You MUST return two top-level XML blocks in this order:\n"
            "1) <think>...</think> : your private reasoning\n"
            "2) <answer>...</answer> : final answer only\n\n"
            "Inside <answer>, return a SystemVerilog code block (```systemverilog```) containing the logic body\n"
            "that goes INSIDE an `always_comb begin ... end` block.\n"
            "Do NOT include `always_comb`, `module`, `typedef`, `function`, or `localparam`.\n"
            "Do NOT wrap your entire output with a top-level `begin ... end` (named or unnamed)."
        )
    return (
        "Return ONLY a SystemVerilog code block (```systemverilog```) containing the logic body\n"
        "that goes INSIDE an `always_comb begin ... end` block.\n"
        "Do NOT include `always_comb`, `module`, `typedef`, `function`, or `localparam`.\n"
        "Do NOT wrap your entire output with a top-level `begin ... end` (named or unnamed)."
    )


SV_NAMING_RULES = """\
Only two prefix substitutions. Keep all other naming identical to C++:
- `in.X->` -> `in_X.` (example: `in.dis2rob->uop[i]` -> `in_dis2rob.uop[i]`)
- `out.X->` -> `out_X.` (example: `out.rob_bcast->flush` -> `out_rob_bcast.flush`)
- For pointer-style access in C++, use dot access in SystemVerilog.

**Keyword Escaping**: Some C++ field names are SystemVerilog reserved keywords.
These fields are renamed with `_v` suffix in the struct typedefs and all references:
- `type` → `type_v`   (e.g. `uop.type` → `uop.type_v`)
- `event` → `event_v`
The typedefs, variable declarations, and pi/po mappings already use the escaped names.
You MUST use the same escaped names in your logic."""

TRANSLATION_RULES = """\
1. Convert only this method body. Do not touch other methods.
2. Apply naming map: `in.X->Y` -> `in_X.Y`, `out.X->Y` -> `out_X.Y`.
3. Preserve control flow and ordering (`if/else/for/case`).
4. Translate helper macros/functions semantically.
5. Ignore prints/exits (`cout`, `printf`, `exit`).
6. Keep the result compile-clean (no WIDTHEXPAND/WIDTHTRUNC/LATCH).
6a. Stay within a yosys-friendly synthesizable SystemVerilog subset. Avoid simulator-only or highly dynamic constructs.
6b. Do not use `automatic` locals.
6b1. Do not use `break`, `continue`, or `disable` to exit loops. Rewrite with done-flags or gated execution.
6c. Do not use size-cast syntax like `4'(expr)` or `int'(expr)`. Use width-safe literals, concatenation, masks, or slices instead.
6d. Avoid direct dynamic field access on aggregate arrays such as `arr[idx].field` when `arr` is an array of structs/packed aggregates.
    Prefer one of these yosys/verilator-safe patterns instead:
    - Read: `tmp = arr[idx]; x = tmp.field;`
    - Write: `tmp = arr[idx]; tmp.field = ...; arr[idx] = tmp;`
    - Or use constant-bound loops / case statements that select whole elements, then access fields on the selected temporary.
6d1. For multi-dimensional aggregate arrays, never write forms like `arr[i][j].field` directly.
     Use a typed temporary for the selected element first, e.g.:
     - `req_tmp = in_dis2iss.req[i][j]; if (req_tmp.valid) ...`
     - Never use `in_dis2iss.req[i][j].valid` directly.
6d2. Never duplicate the container name after `in_*/out_*` mapping.
     Example: C++ `out.iss2dis->ready_num[i]` must become `out_iss2dis.ready_num[i]`,
     not `out_iss2dis.iss2dis.ready_num[i]`.
6d3. Yosys requires procedural `for` loops to have compile-time-constant init/condition/step expressions.
     If the original C++ loop starts or ends from a runtime signal, rewrite it as a constant-bound loop
     over the full legal range and guard the body with `if (...)`.
6e. Do not create extra named blocks such as `begin : helper_locals`.
6f. Do not invent new typedef names. Use only typedef names that already appear in the provided type context.
6g. Do not emit C++ container/member-method calls in SystemVerilog snippets.
    Names like `clear`, `push_back`, `reserve`, `resize`, `schedule`, `commit_issue`, `wakeup`, `tick`
    are C++ APIs, not synthesizable SV fields/methods in this framework.
7. Preserve update semantics exactly:
   - `x++`, `++x`, `x += y`, `x--`, `--x`, `x -= y` update `x` itself.
   - If the C++ writes `x_1++`, translate that as an update to `x_1`, not `x`.
   - Do not rewrite a self-update like `x_1--` into `x_1 = x - 1` unless the C++ explicitly uses `x`.
8. Preserve untouched signals:
   - If the C++ method does not assign a signal on some path, do not invent a value for it from a similarly named signal.
   - Do not add whole-object copies such as `a_1 = a` or `state_next = state` unless the C++ method explicitly does that."""

OUTPUT_CONSTRAINTS = """\
Output rules (STRICT):
1. Output only statements valid inside an existing `always_comb` block.
2. Do NOT output `always_*`, `module`, `typedef`, `function`, `localparam`, or `assign`.
   Do NOT output a top-level `begin : ...` / `begin ... end` wrapper for the whole method body.
3. You MAY declare local variables if needed, but:
   - do not redeclare/shadow framework-declared signals/fields
   - declare locals at the top of the method body before executable statements
   - do not declare array-typed locals (packed or unpacked) inside the method body
   - do not declare unpacked-array locals inside procedural code
   - unconditionally initialize locals before any branching; assign defaults on all paths to avoid latches
   - if a C++ local would otherwise be conditionally assigned before later use, give it an explicit safe default
     instead of relying on uninitialized behavior
   - for maximum yosys compatibility, declare loop variables separately as `integer` or `int` locals
     and then use `for (i = ... )`; avoid `for (int i = ... )`
   - if a loop bound or start depends on a runtime signal, do not put that runtime expression directly
     in the `for (...)` header; use a constant-bound loop plus `if (...)` inside the body
4. Do NOT emit framework-owned boilerplate (pi/po mapping, default init) unless it exists in the C++ method.
   Do NOT invent carry/copy/default behavior for unrelated signals just because names look similar
   (for example, do not assume `x_1` must copy `x`) unless the C++ method explicitly does that.
   If a checked signal also appears as an existing variable, treat it as an independent signal.
   When the C++ performs an in-place update on that signal, update that same signal directly.
   Example: C++ `count_1--;` means SystemVerilog `count_1 = count_1 - 1;`, not `count_1 = count - 1;`.
5. Keep widths explicit: use casts/slices/masks to avoid WIDTHEXPAND/WIDTHTRUNC.
   For power-of-two modulo on fixed-width vars, prefer `& mask` or slices."""


METHOD_PROMPT_TEMPLATE = """\
## TASK (STRICT SCOPE)
{role_intro}
Target: `{module_type}::{method_name}` only.
Do NOT implement other methods. Do NOT modify framework code.
{framework_guarantee}
{framework_skeleton}
### END TASK ###

## OUTPUT (STRICT)
{output_block}
### END OUTPUT ###

## SV ENVIRONMENT (ALREADY DECLARED BY FRAMEWORK)
Use these existing variables directly. Do NOT redeclare them.

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
### END SV ENVIRONMENT ###

## CONTEXT
Naming rules:
{naming_rules}
{constants_block}
{signal_width_block}
{project_context_section}\
### END CONTEXT ###

{type_defs_section}\
## SOURCE OF TRUTH (C++)
```cpp
void {module_type}::{method_name}() {{
{method_body}
}}
```
{helpers_section}\
### END SOURCE OF TRUTH ###

## RULES
{translation_rules}
{output_constraints}
### END RULES ###"""


def _is_translated_method(name):
    return name == "init" or name.startswith("comb_")


def _format_helpers(helpers):
    if not helpers:
        return ""
    lines = ["Helper functions (C++ source):", "```cpp"]
    for src in helpers.values():
        lines.append(src)
        lines.append("")
    lines.append("```")
    lines.append("### END OF HELPER FUNCTIONS ###")
    return "\n".join(lines)


def filter_used_constants(all_constants, method_body, helpers):
    text = method_body + " " + " ".join(helpers.values())
    return {
        k: v for k, v in all_constants.items()
        if re.search(rf'\b{re.escape(k)}\b', text)
    }


def _strip_outer_parens(expr):
    expr = expr.strip()
    while expr.startswith("(") and expr.endswith(")"):
        depth = 0
        balanced = True
        for i, ch in enumerate(expr):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and i != len(expr) - 1:
                    balanced = False
                    break
        if not balanced or depth != 0:
            break
        expr = expr[1:-1].strip()
    return expr


def _split_top_level(expr, operator):
    parts = []
    depth = 0
    start = 0
    i = 0
    while i < len(expr):
        ch = expr[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and expr.startswith(operator, i):
            parts.append(expr[start:i].strip())
            i += len(operator)
            start = i
            continue
        i += 1
    if parts:
        parts.append(expr[start:].strip())
    return [p for p in parts if p]


def _lower_sv_constant_expr(expr, known_constants):
    expr = (expr or "").strip()
    if not expr:
        return None

    val = _try_eval_const_expr(expr, known_constants)
    if val is not None:
        return str(val)

    expr = _strip_outer_parens(expr)

    and_parts = _split_top_level(expr, "&&")
    if and_parts:
        lowered = [_lower_sv_constant_expr(part, known_constants) for part in and_parts]
        if any(part == "0" for part in lowered):
            return "0"
        if all(part is not None for part in lowered):
            return "1" if all(int(part) != 0 for part in lowered) else "0"
        return None

    or_parts = _split_top_level(expr, "||")
    if or_parts:
        lowered = [_lower_sv_constant_expr(part, known_constants) for part in or_parts]
        if any(part is not None and int(part) != 0 for part in lowered):
            return "1"
        if all(part is not None for part in lowered):
            return "1" if any(int(part) != 0 for part in lowered) else "0"
        return None

    return None


def _select_sv_constants(all_constants, text):
    # prompt 里只保留“能安全降成 SV 常量”的项。
    # 这样可以避免把 C++ 环境专属表达式直接暴露给 LLM，
    # 也保证这些常量在 wrapper/combine 阶段能被稳定复用。
    used = []
    for name, value in sorted(all_constants.items()):
        if not re.search(rf"\b{re.escape(name)}\b", text):
            continue
        lowered = _lower_sv_constant_expr(value, KNOWN_CONSTANTS)
        if lowered is None:
            continue
        used.append((name, lowered))
    return used


def select_prompt_constants(all_constants, text):
    return {name: value for name, value in _select_sv_constants(all_constants, text)}


def render_method_prompt(
    *,
    module_type,
    method_name,
    method_body,
    cpp_type_sources,
    sv_typedefs,
    var_decls,
    method_helpers,
    used_constants,
    signal_width_hints,
    project_context,
    role_intro,
    output_block,
):
    # infer prompt 的核心装配点。
    # 上游模块只负责提供上下文；这里统一拼接任务边界、变量环境、
    # 常量/宽度提示、类型定义和 C++ source of truth。
    input_vars, output_vars, internal_vars = var_decls
    const_section = "\n".join(f"{k} = {v}" for k, v in sorted(used_constants.items()))
    constants_block = (
        "Constants (C preprocessor defines, may be referenced by name):\n"
        "```text\n"
        f"{const_section}\n"
        "```\n"
    ) if const_section else ""
    if signal_width_hints:
        width_lines = "\n".join(f"- `{p}` : {w} bits" for p, w in signal_width_hints)
        signal_width_block = f"Signal width hints:\n{width_lines}\n"
    else:
        signal_width_block = ""
    if project_context.strip():
        project_context_section = (
            "Project config excerpts:\n"
            "```cpp\n"
            f"{project_context}\n"
            "```\n"
        )
    else:
        project_context_section = ""
    if cpp_type_sources.strip() or sv_typedefs.strip():
        type_defs_section = (
            "## TYPE DEFINITIONS (C++ -> SV)\n"
            "```cpp\n"
            f"{cpp_type_sources}\n"
            "```\n\n"
            "```systemverilog\n"
            f"{sv_typedefs}\n"
            "```\n"
            "### END OF TYPE DEFINITIONS ###\n\n"
        )
    else:
        type_defs_section = ""
    return METHOD_PROMPT_TEMPLATE.format(
        role_intro=role_intro,
        module_type=module_type,
        method_name=method_name,
        framework_guarantee=FRAMEWORK_GUARANTEE_TEMPLATE.format(
            module_type=module_type,
            method_name=method_name,
        ),
        framework_skeleton=FRAMEWORK_SKELETON_TEMPLATE.format(
            method_name=method_name,
        ),
        naming_rules=SV_NAMING_RULES,
        constants_block=constants_block,
        signal_width_block=signal_width_block,
        project_context_section=project_context_section,
        type_defs_section=type_defs_section,
        input_vars="\n".join(input_vars),
        output_vars="\n".join(output_vars),
        internal_vars="\n".join(internal_vars),
        helpers_section=_format_helpers(method_helpers),
        method_body=method_body,
        translation_rules=TRANSLATION_RULES,
        output_block=output_block,
        output_constraints=OUTPUT_CONSTRAINTS,
    )


def render_infer_method_prompt(
    *,
    module_type,
    method_name,
    method_body,
    cpp_type_sources,
    sv_typedefs,
    var_decls,
    method_helpers,
    used_constants,
    signal_width_hints,
    project_context,
    use_think=True,
):
    return render_method_prompt(
        module_type=module_type,
        method_name=method_name,
        method_body=method_body,
        cpp_type_sources=cpp_type_sources,
        sv_typedefs=sv_typedefs,
        var_decls=var_decls,
        method_helpers=method_helpers,
        used_constants=used_constants,
        signal_width_hints=signal_width_hints,
        project_context=project_context,
        role_intro=INFER_ROLE_INTRO,
        output_block=_infer_output_block(use_think),
    )




def build_subtask_prompt(method, helpers_db, all_constants, module_info,
                         cpp_type_sources, sv_typedefs, var_decls, infer_use_think=True):
    """Build a prompt for one comb_* method sub-task using SV struct syntax."""
    # prompt 的上下文按“只够翻当前 method”为原则裁剪：
    # helper 只带被当前 method 直接/间接调用到的；
    # 常量只带 logic 文本实际引用到的；
    # 宽度提示只覆盖当前 method 出现过的信号。
    method_helpers = extract_method_helpers(method["body"], helpers_db)
    logic_text = method["body"] + "\n" + "\n".join(method_helpers.values())
    used_consts = select_prompt_constants(all_constants, logic_text)
    width_hints = get_method_signal_width_hints(
        method["body"], method_helpers,
        module_info["structs"], module_info["module_type"], module_info["type_widths"],
    )
    project_context = project_context_for_logic(logic_text)
    content = render_infer_method_prompt(
        module_type=module_info["module_type"],
        method_name=method["name"],
        method_body=method["body"],
        cpp_type_sources=cpp_type_sources,
        sv_typedefs=sv_typedefs,
        var_decls=var_decls,
        method_helpers=method_helpers,
        used_constants=used_consts,
        signal_width_hints=width_hints,
        project_context=project_context,
        use_think=infer_use_think,
    )

    return {
        "task": f"{module_info['module_type']}_{method['name']}",
        "module_type": module_info["module_type"],
        "method": method["name"],
        "messages": [{"role": "user", "content": content}],
    }


def _read_text(path):
    with open(path) as f:
        return f.read()


def _find_io_generator_outer_header(bsd_dir):
    """Find the generated <dir_base>.h that defines io_generator_outer()."""
    dir_base = os.path.basename(bsd_dir.rstrip("/"))
    preferred = os.path.join(bsd_dir, f"{dir_base}.h")
    if os.path.exists(preferred):
        return preferred

    try:
        for fname in sorted(os.listdir(bsd_dir)):
            if not fname.endswith(".h"):
                continue
            fpath = os.path.join(bsd_dir, fname)
            try:
                text = _read_text(fpath)
            except OSError:
                continue
            if "io_generator_outer" in text:
                return fpath
    except OSError:
        return None
    return None


def _parse_comb_call_order_from_outer(header_text):
    """Extract translated method call order from io_generator_outer() wrapper."""
    region = header_text
    m = re.search(r"//please add code below(.*?)//end of code add", header_text, re.DOTALL)
    if m:
        region = m.group(1)

    calls = re.findall(
        r"(?:\.|->)\s*((?:init|comb_[A-Za-z0-9_]+))\s*\(\s*\)\s*;",
        region,
    )
    out = []
    seen = set()
    for name in calls:
        if name in seen:
            continue
        seen.add(name)
        out.append(name)
    return out


def resolve_active_method_order(module_info, outer_header_text=None):
    # active methods 以 io_generator_outer() 的真实调用顺序为准，
    # 这是框架级策略：prompt/snippet/combine/full verify 必须看到同一组 method。
    parsed_method_order = [
        m["name"] for m in module_info["methods"] if _is_translated_method(m["name"])
    ]
    if outer_header_text:
        outer_order = _parse_comb_call_order_from_outer(outer_header_text)
        if outer_order:
            parsed_set = set(parsed_method_order)
            return [m for m in outer_order if m in parsed_set]
    return parsed_method_order


def get_combine_info(bsd_dir, base_dir=".", mapping_provider=None):
    """Collect everything needed to assemble per-bsd-file SV modules.

    Returns dict:
      module_type, pi_width, po_width,
      method_order, sv_typedefs, var_decls,
      pi_sv_code, po_sv_code,
      bsd_files: [{module_name, pi_width, po_width}]
    """
    if mapping_provider is None:
        mapping_provider = io_mapping.get_mapping_provider()

    full_dir = os.path.join(base_dir, bsd_dir) if base_dir != "." else bsd_dir
    module_info = analyze_module(full_dir, mapping_provider=mapping_provider)
    module_type = module_info["module_type"]

    # 优先采用 wrapper 中的真实调用顺序。
    # 这样可以保证 combine 出来的 always_comb 调用顺序与最终验证壳一致。
    method_order = None
    outer_h = _find_io_generator_outer_header(full_dir)
    if outer_h:
        outer_text = _read_text(outer_h)
        method_order = resolve_active_method_order(module_info, outer_text)
    if method_order is None:
        method_order = resolve_active_method_order(module_info)

    # combine_info 是各阶段共享的“模块装配上下文”：
    # prompt 用它准备环境，snippet 用它生成单方法 wrapper，
    # combine 用它装配完整模块。
    sv_typedefs = generate_sv_typedefs(
        module_info["structs"], module_info["type_widths"], module_type=module_type, expand_depth=-1
    )
    var_decls = generate_sv_var_declarations(module_info["structs"], module_type)
    all_constants = parse_all_constants()
    logic_text = module_info.get("logic_source", "")
    used_constants = _select_sv_constants(all_constants, logic_text)

    pi_lines = mapping_provider.generate_pi_sv(module_info["inputs"])
    pi_code = "\n".join(pi_lines)

    bsd_entries = []
    for bf in module_info["bsd_files"]:
        module_name = os.path.splitext(bf["filename"])[0]
        out_signals = [{"path": sig, "width": w} for sig, w in bf["unpack_lines"]]
        po_lines = mapping_provider.generate_po_sv(out_signals)

        bsd_entries.append({
            "module_name": module_name,
            "pi_width": module_info["pi_width"],
            "po_width": bf["po_width"],
            "pi_code": pi_code,
            "po_code": "\n".join(po_lines),
        })

    return {
        "module_type": module_type,
        "pi_width": module_info["pi_width"],
        "method_order": method_order,
        "sv_typedefs": sv_typedefs,
        "sv_constants": used_constants,
        "var_decls": var_decls,
        "bsd_files": bsd_entries,
    }


def build_prompts(input_dirs, output_path, base_dir=".", struct_expand_depth=2,
                  infer_use_think=True, mapping_provider=None):
    """Generate per-method sub-task prompts for bsd modules.

    Only processes *_bsd directories. Each translated method becomes one prompt entry.
    """
    # 这一层只做静态任务生成，不调用 LLM。
    # 粒度是：input_dir -> module -> active method -> one JSONL prompt entry。
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    count = 0

    if mapping_provider is None:
        mapping_provider = io_mapping.get_mapping_provider()

    with open(output_path, "w") as f:
        for d in input_dirs:
            full_dir = os.path.join(base_dir, d)
            if not os.path.isdir(full_dir) or not os.path.basename(d).endswith("_bsd"):
                continue

            module_info = analyze_module(full_dir, mapping_provider=mapping_provider)
            helpers_db = build_helper_db(module_info)
            all_constants = parse_all_constants()
            var_decls = generate_sv_var_declarations(
                module_info["structs"], module_info["module_type"],
            )
            combine_info = get_combine_info(d, base_dir=base_dir, mapping_provider=mapping_provider)
            ordered_methods = {m["name"]: m for m in module_info["methods"]}
            # 这里只遍历 combine_info 给出的 method_order，
            # 避免把 wrapper 不会调用的方法也送去翻译。
            for method_name in combine_info["method_order"]:
                method = ordered_methods.get(method_name)
                if not method:
                    continue
                method_helpers = extract_method_helpers(method["body"], helpers_db)
                logic_text = method["body"] + "\n" + "\n".join(method_helpers.values())
                ordered = get_struct_order_for_method(
                    module_info["structs"],
                    module_info["module_type"],
                    method_body=logic_text,
                    expand_depth=struct_expand_depth,
                )
                sv_typedefs = generate_sv_typedefs(
                    module_info["structs"], module_info["type_widths"],
                    ordered_structs=ordered,
                )
                cpp_type_sources = generate_cpp_type_sources(
                    module_info.get("struct_sources", {}),
                    ordered,
                )
                entry = build_subtask_prompt(
                    method, helpers_db, all_constants, module_info,
                    cpp_type_sources, sv_typedefs, var_decls, infer_use_think=infer_use_think,
                )
                f.write(json.dumps(entry, ensure_ascii=False) + "\n")
                count += 1

    return count
