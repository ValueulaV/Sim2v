"""Shared SystemVerilog path and keyword helpers."""

import re

# Keywords that could realistically collide with C++ struct field / variable names.
# Verilog is case-sensitive, all keywords are lowercase.
VERILOG_KEYWORDS = frozenset({
    # IEEE 1364-2005 (Verilog)
    "always", "and", "assign", "automatic", "begin", "buf", "case",
    "default", "disable", "edge", "else", "end", "endcase",
    "endfunction", "endmodule", "endtask", "event", "for", "force",
    "forever", "fork", "function", "generate", "genvar", "if",
    "initial", "inout", "input", "integer", "join", "module",
    "nand", "nor", "not", "or", "output", "parameter", "priority",
    "real", "reg", "release", "repeat", "return", "signed",
    "supply0", "supply1", "task", "time", "tri", "type",
    "unsigned", "wait", "while", "wire", "xnor", "xor",
    # IEEE 1800-2017 (SystemVerilog)
    "bit", "byte", "class", "const", "enum", "export", "extends",
    "extern", "import", "int", "local", "logic", "new", "null",
    "packed", "protected", "pure", "rand", "randc", "ref",
    "shortint", "shortreal", "static", "string", "struct",
    "super", "this", "typedef", "union", "unique", "var",
    "virtual", "void",
})


def escape_sv_keyword(name):
    """If name is a Verilog/SV keyword, append _v suffix.

    Rule: C++ field `type` → SV/Verilog `type_v`.
    This is applied to individual field names, NOT to compound names like `uop_type`.
    """
    if name in VERILOG_KEYWORDS:
        return name + "_v"
    return name


def _escape_sv_path(path):
    """Escape Verilog keywords in a dot-separated SV struct access path.

    Only escapes bare field names, not array indices or variable names.
    'uop[i].type' → 'uop[i].type_v'
    """
    parts = path.split(".")
    result = []
    for part in parts:
        # Split field name from array indices: 'entry[i]' → ('entry', '[i]')
        m = re.match(r'^(\w+)((?:\[[^\]]*\])*)$', part)
        if m:
            fname, arr = m.group(1), m.group(2)
            result.append(escape_sv_keyword(fname) + arr)
        else:
            result.append(part)
    return ".".join(result)


def cpp_path_to_sv(path):
    """Convert C++ signal path to SystemVerilog struct access.

    Escapes field names that are Verilog keywords (e.g., type → type_v).

    'in.dis2rob->uop[i].type' → 'in_dis2rob.uop[i].type_v'
    'out.rob_bcast->flush'     → 'out_rob_bcast.flush'
    """
    # in.X->Y.Z → in_X.Y.Z
    path = re.sub(r'^in\.(\w+)->', r'in_\1.', path)
    # out.X->Y.Z → out_X.Y.Z
    path = re.sub(r'^out\.(\w+)->', r'out_\1.', path)
    # out.field or in.field → out_field / in_field
    path = re.sub(r'^(out|in)\.', r'\1_', path)
    # Escape keyword field names in dot-separated components
    return _escape_sv_path(path)


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


def _field_name_matches(field_name, token_name):
    return field_name == token_name or escape_sv_keyword(field_name) == token_name


def _module_interface_type(module_info, field_name):
    for field in (module_info.get("structs", {}) or {}).get(module_info.get("module_type"), []):
        if field.get("name") == field_name:
            return field.get("type")
    return None


def _find_struct_field(struct_fields, token_name):
    for field in struct_fields:
        if _field_name_matches(field.get("name", ""), token_name):
            return field
    return None


def _packed_total_width(module_info, type_name, width, array_dims):
    structs = module_info.get("structs", {}) or {}
    if type_name in structs:
        total = 0
        for field in structs[type_name]:
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
    elif type_name == "bool":
        total = 1
    elif type_name in (module_info.get("type_widths") or {}):
        total = int(module_info["type_widths"][type_name])
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
    if re.fullmatch(r"\d+", expr):
        return str(int(expr) * factor)
    return f"(({expr}) * {factor})"


def _sum_expr(terms):
    norm = []
    const = 0
    for term in terms:
        tok = (term or "").strip()
        if not tok or tok == "0":
            continue
        if re.fullmatch(r"\d+", tok):
            const += int(tok)
            continue
        norm.append(tok)
    if const:
        norm.append(str(const))
    if not norm:
        return "0"
    if len(norm) == 1:
        return norm[0]
    return "(" + " + ".join(norm) + ")"


def _consume_indices(module_info, type_name, width, array_dims, index_exprs):
    dims = list(array_dims or [])
    offset_terms = []
    for expr in index_exprs:
        if not dims:
            return None, None
        dim = int(dims[0])
        stride = _packed_total_width(module_info, type_name, width, dims[1:])
        if stride is None:
            return None, None
        idx = expr.strip()
        if re.fullmatch(r"\d+", idx):
            idx_val = int(idx)
            if idx_val < 0 or idx_val >= dim:
                return None, None
            offset_terms.append(str(idx_val * stride))
        else:
            offset_terms.append(_mul_expr(idx, stride))
        dims = dims[1:]
    return _sum_expr(offset_terms), dims


def _resolve_struct_subpath(module_info, type_name, width, array_dims, tokens):
    if not tokens:
        leaf_width = _packed_total_width(module_info, type_name, width, array_dims)
        if leaf_width is None:
            return None, None
        return "0", leaf_width

    structs = module_info.get("structs", {}) or {}
    if type_name not in structs:
        return None, None

    token_name, token_indices = _split_indexed_token(tokens[0])
    fields = structs[type_name]
    field_idx = None
    field = None
    for idx, cand in enumerate(fields):
        if _field_name_matches(cand.get("name", ""), token_name):
            field_idx = idx
            field = cand
            break
    if field is None:
        return None, None

    offset_terms = []
    for later in fields[field_idx + 1:]:
        later_width = _packed_total_width(
            module_info,
            later.get("type"),
            later.get("width"),
            later.get("array_dims") or [],
        )
        if later_width is None:
            return None, None
        offset_terms.append(str(later_width))

    index_lsb, remaining_dims = _consume_indices(
        module_info,
        field.get("type"),
        field.get("width"),
        field.get("array_dims") or [],
        token_indices,
    )
    if index_lsb is None:
        return None, None
    if index_lsb != "0":
        offset_terms.append(index_lsb)

    sub_lsb, sub_width = _resolve_struct_subpath(
        module_info,
        field.get("type"),
        field.get("width"),
        remaining_dims,
        tokens[1:],
    )
    if sub_lsb is None or sub_width is None:
        return None, None
    if sub_lsb != "0":
        offset_terms.append(sub_lsb)
    return _sum_expr(offset_terms), sub_width


def _resolve_root_node(module_info, root_name):
    structs = module_info.get("structs", {}) or {}
    module_type = module_info.get("module_type")
    module_fields = structs.get(module_type, [])
    iface_match = re.match(r"^(in|out)_(.+)$", root_name)
    if iface_match:
        iface, iface_field_name = iface_match.groups()
        iface_type = _module_interface_type(module_info, iface)
        if not iface_type:
            return None
        field = _find_struct_field(structs.get(iface_type, []), iface_field_name)
        if not field:
            return None
        escaped = escape_sv_keyword(field.get("name", iface_field_name))
        return {
            "sv_root": f"{iface}_{escaped}",
            "type": field.get("type"),
            "width": field.get("width"),
            "array_dims": field.get("array_dims") or [],
        }

    field = _find_struct_field(module_fields, root_name)
    if not field:
        return None
    escaped = escape_sv_keyword(field.get("name", root_name))
    return {
        "sv_root": escaped,
        "type": field.get("type"),
        "width": field.get("width"),
        "array_dims": field.get("array_dims") or [],
    }


def path_to_sv_slice(module_info, path, width_hint=None):
    """Translate an SV/C++ field path to a packed bit-slice expression.

    Returns `None` when the path cannot be resolved as a packed-struct subpath.
    """
    if not module_info:
        return None

    raw = (path or "").strip()
    if not raw:
        return None
    if raw.startswith("in.") or raw.startswith("out."):
        norm = cpp_path_to_sv(raw)
    else:
        norm = raw.replace("->", ".")

    tokens = _split_path_tokens(norm)
    if not tokens:
        return None

    root_name, root_indices = _split_indexed_token(tokens[0])
    # Only rewrite interface-root field paths (`in_*`/`out_*`) where yosys
    # parser gaps on packed-struct internal arrays are observed.
    if not (root_name.startswith("in_") or root_name.startswith("out_")):
        return None
    root = _resolve_root_node(module_info, root_name)
    if not root:
        return None

    root_lsb, root_remaining_dims = _consume_indices(
        module_info,
        root.get("type"),
        root.get("width"),
        root.get("array_dims") or [],
        root_indices,
    )
    if root_lsb is None:
        return None

    sub_lsb, leaf_width = _resolve_struct_subpath(
        module_info,
        root.get("type"),
        root.get("width"),
        root_remaining_dims,
        tokens[1:],
    )
    if sub_lsb is None or leaf_width is None:
        return None

    lsb = _sum_expr([root_lsb, sub_lsb])
    final_width = int(width_hint) if width_hint is not None else int(leaf_width)
    if final_width <= 0:
        return None
    if final_width == 1:
        return f"{root['sv_root']}[{lsb}]"
    return f"{root['sv_root']}[{lsb} +: {final_width}]"
