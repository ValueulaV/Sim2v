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
