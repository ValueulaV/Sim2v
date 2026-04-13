"""Helpers for assembling combined SystemVerilog modules."""

import re


_DECL_NAME_RE = re.compile(r'^\s*(.+?)\s+([A-Za-z_]\w*)(\s*(?:\[[^\]]+\]\s*)*)\s*;\s*$')


def parse_decl(decl):
    m = _DECL_NAME_RE.match(decl.strip())
    if not m:
        return None
    dims = (m.group(3) or "").strip()
    return {
        "type_part": m.group(1).strip(),
        "name": m.group(2),
        "dims": re.findall(r"\[([^\]]+)\]", dims) if dims else [],
    }


def _emit_zero_assignment(name, dims):
    if not dims:
        return [f"    {name} = '0;"]

    loop_vars = [f"__{name}_i{idx}" for idx in range(len(dims))]
    lines = []
    indent = "    "
    for idx, dim in enumerate(dims):
        if ":" not in dim:
            raise ValueError(f"Unsupported declaration dimension: [{dim}]")
        start, end = [part.strip() for part in dim.split(":", 1)]
        lines.append(f"{indent}for (int {loop_vars[idx]} = {start}; {loop_vars[idx]} <= {end}; {loop_vars[idx]}++) begin")
        indent += "    "
    indexed = name + "".join(f"[{loop_var}]" for loop_var in loop_vars)
    lines.append(f"{indent}{indexed} = '0;")
    for _ in loop_vars:
        indent = indent[:-4]
        lines.append(f"{indent}end")
    return lines


def build_default_assignments(decls, strategy="zero_all"):
    """Return SV default assignment lines for declared signals."""
    if strategy in (None, "", "none", "skip"):
        return []
    if strategy != "zero_all":
        raise ValueError(f"Unknown default_init strategy: {strategy}")

    writable_defaults = {}
    for decl in decls:
        info = parse_decl(decl)
        if not info:
            continue
        name = info["name"]
        if name not in writable_defaults:
            writable_defaults[name] = info

    if not writable_defaults:
        return []

    lines = ["    // ---- Framework defaults: decoded/input/internal/output vars = 0 ----"]
    for name in sorted(writable_defaults.keys()):
        info = writable_defaults[name]
        lines.extend(_emit_zero_assignment(name, info["dims"]))
    lines.append("")
    return lines


def build_combined_module_sv(bf, combine_info, snippets, default_strategy="zero_all"):
    """Build a full-module SV wrapper from snippet bodies.

    `snippets` is a mapping: `<MODULE>_<method>` -> method body text.
    Missing tasks are skipped; callers that need compile-equivalent empty method
    blocks should pass explicit empty strings for non-target methods.
    """
    input_decls, output_decls, internal_decls = combine_info["var_decls"]
    sv_constants = combine_info.get("sv_constants", [])
    method_order = combine_info["method_order"]
    module_type = combine_info["module_type"]

    def normalize_indent(lines):
        non_empty = [ln for ln in lines if ln.strip()]
        if not non_empty:
            return lines
        min_indent = min(len(re.match(r" *", ln).group(0)) for ln in non_empty)
        if min_indent <= 0:
            return lines
        return [ln[min_indent:] if ln.strip() else "" for ln in lines]

    parts = [
        f"module {bf['module_name']} (",
        f"    input  wire [{bf['pi_width'] - 1}:0] pi,",
        f"    output logic [{bf['po_width'] - 1}:0] po",
        ");",
        "",
        "// ---- SV type definitions (auto-generated) ----",
        combine_info["sv_typedefs"],
        "",
        "// ---- Constants (from C++ config) ----",
    ]
    if sv_constants:
        for cname, cval in sv_constants:
            parts.append(f"localparam int {cname} = {cval};")
    else:
        parts.append("// (none)")

    parts.extend([
        "",
        "// ---- Variable declarations ----",
        *input_decls,
        "",
        *internal_decls,
        "",
        *output_decls,
        "",
        "always_comb begin",
    ])

    parts.extend(build_default_assignments(
        [*input_decls, *internal_decls, *output_decls],
        strategy=default_strategy,
    ))
    parts.extend([
        "    // ---- Input extraction from pi[] ----",
        bf["pi_code"],
        "",
    ])

    for method_name in method_order:
        task_key = f"{module_type}_{method_name}"
        if task_key not in snippets:
            continue
        parts.append(f"    // ---- {method_name} (LLM) ----")
        body_lines = normalize_indent(snippets[task_key].splitlines())
        parts.append(f"    begin : {method_name}")
        parts.extend([f"        {ln}" if ln.strip() else "" for ln in body_lines])
        parts.append("    end")
        parts.append("")

    parts.extend([
        "    // ---- Output packing to po[] ----",
        f"    for (int __po_i = 0; __po_i < {bf['po_width']}; __po_i++) begin",
        "        po[__po_i] = 1'b0;",
        "    end",
        bf["po_code"],
        "end",
        "",
        "endmodule",
    ])

    return "\n".join(parts)
