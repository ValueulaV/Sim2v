"""Minimal signal analysis helpers used by snippet-stage verification.

This module intentionally keeps a narrow scope:
- normalize C++/SV signal paths into a comparable canonical form
- extract per-method read/write signal sets with libclang

Legacy full-module debug ranking/reporting logic has been removed.
"""

import os
import re

try:
    from clang import cindex
except Exception:  # pragma: no cover
    cindex = None


PATH_RE = re.compile(r"[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*|\[[^\]]+\])+")
IDENT_RE = re.compile(r"\b[A-Za-z_]\w*\b")
IDENT_SKIP = {
    "if", "else", "for", "while", "switch", "case", "break", "continue",
    "return", "true", "false", "nullptr", "this", "const", "auto", "int",
    "unsigned", "long", "short", "char", "bool", "float", "double", "void",
    "in", "out",
}


def _strip_bit(sig):
    return re.sub(r"\[\d+\]$", "", (sig or "").strip())


def _canonical_path(path):
    """Normalize C++/SV path spelling to a comparable dotted form."""
    path = _strip_bit(path)
    path = path.replace("->", ".")
    path = re.sub(r"\[[^\]]+\]", "", path)
    path = re.sub(r"\s+", "", path)
    path = re.sub(r"^(in|out)_", r"\1.", path)
    path = re.sub(r"^[A-Za-z_]\w+\.(in|out)\.", r"\1.", path)
    path = re.sub(r"\.+", ".", path).strip(".")
    return path


def _extract_canonical_paths(text):
    paths = set()
    for m in PATH_RE.finditer(text or ""):
        p = _canonical_path(m.group(0))
        if p:
            paths.add(p)
    return paths


def _extract_identifiers(text, local_vars=None):
    local_vars = local_vars or set()
    out = set()
    for tok in IDENT_RE.findall(text or ""):
        if tok in IDENT_SKIP or tok in local_vars:
            continue
        out.add(tok)
    return out


def _add_expr_refs(target_set, expr, local_vars=None):
    paths = _extract_canonical_paths(expr)
    if paths:
        target_set.update(paths)
    else:
        target_set.update(_extract_identifiers(expr, local_vars))


def _cursor_text(cursor):
    return "".join(tok.spelling for tok in cursor.get_tokens())


def _clang_args_for_cpp(cpp_path):
    bsd_dir = os.path.dirname(cpp_path)
    io_gen_dir = os.path.dirname(bsd_dir)
    return [
        "-x", "c++", "-std=c++17",
        f"-I{bsd_dir}",
        f"-I{io_gen_dir}",
        f"-I{os.path.join(io_gen_dir, 'simulator_include')}",
    ]


def _extract_rw_libclang_file(cpp_path, method_names):
    """Extract per-method read/write signal sets from a C++ source file via libclang."""
    # 这条通道是 snippet 目标抽取的“主精确来源”：
    # 先用 libclang 把每个 method 的读写集合拉出来，
    # 后面再由 snippet_harness 做文本补齐和路径细化。
    # 如果这里失效，框架仍可退回文本启发式，但准确率会下降。
    if cindex is None or not os.path.exists(cpp_path):
        return None

    index = cindex.Index.create()
    tu = index.parse(cpp_path, args=_clang_args_for_cpp(cpp_path), options=0)
    method_set = set(method_names)
    out = {}

    for cur in tu.cursor.walk_preorder():
        if cur.kind != cindex.CursorKind.CXX_METHOD:
            continue
        if cur.spelling not in method_set or not cur.is_definition():
            continue

        local_vars = set()
        for node in cur.walk_preorder():
            if node.kind == cindex.CursorKind.VAR_DECL and node.spelling:
                local_vars.add(node.spelling)

        # 这里只覆盖项目当前最常见的写模式：
        # - 二元赋值
        # - ++ / --
        # - 带初始化的局部变量声明
        # 它不是完整的 C++ dataflow 分析器。
        writes = set()
        reads = set()
        for node in cur.walk_preorder():
            if node.kind == cindex.CursorKind.BINARY_OPERATOR:
                text = _cursor_text(node)
                if "=" not in text or any(op in text for op in ("==", "<=", ">=", "!=")):
                    continue
                kids = list(node.get_children())
                if len(kids) < 2:
                    continue
                _add_expr_refs(writes, _cursor_text(kids[0]), local_vars)
                _add_expr_refs(reads, _cursor_text(kids[1]), local_vars)
            elif node.kind == cindex.CursorKind.UNARY_OPERATOR:
                text = _cursor_text(node)
                if "++" in text or "--" in text:
                    _add_expr_refs(writes, text, local_vars)
                    _add_expr_refs(reads, text, local_vars)
            elif node.kind == cindex.CursorKind.VAR_DECL:
                kids = list(node.get_children())
                if kids:
                    _add_expr_refs(reads, _cursor_text(kids[-1]), local_vars)

        out[cur.spelling] = {"writes": writes, "reads": reads}

    return out
