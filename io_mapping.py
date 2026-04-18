"""IO mapping strategy for pi/po packing and unpacking."""

import ast
import os
import re

import cpp_helpers


def _read_text(path):
    with open(path) as f:
        return f.read()


def _strip_cpp_comments(text):
    """Remove C/C++ comments while keeping string literals intact."""
    out = []
    i = 0
    n = len(text)
    in_line = False
    in_block = False
    in_str = None
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if in_line:
            if ch == "\n":
                in_line = False
                out.append(ch)
            i += 1
            continue

        if in_block:
            if ch == "*" and nxt == "/":
                in_block = False
                i += 2
            else:
                i += 1
            continue

        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue

        if ch == "/" and nxt == "/":
            in_line = True
            i += 2
            continue
        if ch == "/" and nxt == "*":
            in_block = True
            i += 2
            continue
        if ch in ('"', "'"):
            in_str = ch
            out.append(ch)
            i += 1
            continue

        out.append(ch)
        i += 1

    return "".join(out)


def _extract_braced_body(text, start):
    """Extract body from position after opening {, matching braces."""
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[start:i - 1].strip()


def _skip_ws(text, pos):
    while pos < len(text) and text[pos].isspace():
        pos += 1
    return pos


def _find_matching(text, open_pos, open_ch, close_ch):
    depth = 1
    i = open_pos + 1
    in_str = None
    while i < len(text):
        ch = text[i]
        if in_str:
            if ch == "\\":
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch in ('"', "'"):
            in_str = ch
            i += 1
            continue
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _find_stmt_end(text, pos):
    depth_paren = 0
    depth_brack = 0
    depth_brace = 0
    in_str = None
    i = pos
    while i < len(text):
        ch = text[i]
        if in_str:
            if ch == "\\":
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch in ('"', "'"):
            in_str = ch
            i += 1
            continue
        if ch == "(":
            depth_paren += 1
        elif ch == ")":
            depth_paren = max(0, depth_paren - 1)
        elif ch == "[":
            depth_brack += 1
        elif ch == "]":
            depth_brack = max(0, depth_brack - 1)
        elif ch == "{":
            depth_brace += 1
        elif ch == "}":
            if depth_brace == 0:
                return i
            depth_brace -= 1
        elif ch == ";" and depth_paren == 0 and depth_brack == 0 and depth_brace == 0:
            return i
        i += 1
    return len(text)


def _safe_eval_int(expr, symbols=None):
    """Evaluate a small integer expression safely (literals + simple ops)."""
    symbols = symbols or {}
    expr = (expr or "").strip()
    if not expr:
        return None

    # Trim balanced outer parentheses.
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

    try:
        node = ast.parse(expr, mode="eval")
    except SyntaxError:
        return None

    def _eval(n):
        if isinstance(n, ast.Expression):
            return _eval(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, int):
            return int(n.value)
        if isinstance(n, ast.Name):
            val = symbols.get(n.id)
            return int(val) if isinstance(val, int) else None
        if isinstance(n, ast.UnaryOp):
            v = _eval(n.operand)
            if v is None:
                return None
            if isinstance(n.op, ast.UAdd):
                return +v
            if isinstance(n.op, ast.USub):
                return -v
            if isinstance(n.op, ast.Invert):
                return ~v
            return None
        if isinstance(n, ast.BinOp):
            l = _eval(n.left)
            r = _eval(n.right)
            if l is None or r is None:
                return None
            op = n.op
            if isinstance(op, ast.Add):
                return l + r
            if isinstance(op, ast.Sub):
                return l - r
            if isinstance(op, ast.Mult):
                return l * r
            if isinstance(op, ast.FloorDiv):
                return l // r if r != 0 else None
            if isinstance(op, ast.Div):
                return l // r if r != 0 else None
            if isinstance(op, ast.Mod):
                return l % r if r != 0 else None
            if isinstance(op, ast.LShift):
                return l << r
            if isinstance(op, ast.RShift):
                return l >> r
            if isinstance(op, ast.BitOr):
                return l | r
            if isinstance(op, ast.BitAnd):
                return l & r
            if isinstance(op, ast.BitXor):
                return l ^ r
            return None
        return None

    return _eval(node)


def _substitute_loop_indices(path, loop_vars):
    def _replace(m):
        expr = m.group(1).strip()
        val = _safe_eval_int(expr, loop_vars)
        if val is not None:
            return f"[{val}]"
        if expr in loop_vars:
            return f"[{loop_vars[expr]}]"
        return m.group(0)

    return re.sub(r"\[([^\]]+)\]", _replace, path)


def _parse_for_header(header, loop_vars):
    parts = [p.strip() for p in header.split(";")]
    if len(parts) != 3:
        return None
    init, cond, step = parts

    m_init = re.match(r"(?:[\w:<>]+\s+)*([A-Za-z_]\w*)\s*=\s*(.+)$", init)
    if not m_init:
        return None
    var = m_init.group(1)
    start_val = _safe_eval_int(m_init.group(2), loop_vars)
    if start_val is None:
        return None

    m_cond = re.match(r"([A-Za-z_]\w*)\s*(<=|<|>=|>)\s*(.+)$", cond)
    if not m_cond or m_cond.group(1) != var:
        return None
    cmp_op = m_cond.group(2)
    end_val = _safe_eval_int(m_cond.group(3), loop_vars)
    if end_val is None:
        return None

    step_clean = step.replace(" ", "")
    m_plus_eq = re.match(rf"{re.escape(var)}\+=(\d+)$", step_clean)
    m_minus_eq = re.match(rf"{re.escape(var)}-=(\d+)$", step_clean)
    if step_clean in (f"{var}++", f"++{var}"):
        step_val = 1
    elif step_clean in (f"{var}--", f"--{var}"):
        step_val = -1
    elif m_plus_eq:
        step_val = int(m_plus_eq.group(1))
    elif m_minus_eq:
        step_val = -int(m_minus_eq.group(1))
    else:
        return None
    if step_val == 0:
        return None

    values = []
    cur = start_val
    guard = 0
    max_iter = 1000000
    while guard < max_iter:
        ok = (
            (cmp_op == "<" and cur < end_val)
            or (cmp_op == "<=" and cur <= end_val)
            or (cmp_op == ">" and cur > end_val)
            or (cmp_op == ">=" and cur >= end_val)
        )
        if not ok:
            break
        values.append(cur)
        cur += step_val
        guard += 1
    return var, values


_SIG_PATH = r"[\w]+(?:(?:\.|->)[\w]+|\[[^\]]+\])*"
_PACK_ASSIGN_RE = re.compile(
    r"(?P<path>" + _SIG_PATH + r")\s*=\s*pack_bits<[^>]+>\s*"
    r"\(\s*cursor\s*,\s*(?P<width>[^)]+?)\s*\)\s*$"
)
_UNPACK_CALL_RE = re.compile(
    r"unpack_bits\s*\(\s*cursor\s*,\s*(?P<path>" + _SIG_PATH + r")\s*,\s*(?P<width>[^)]+?)\s*\)\s*$"
)
_CURSOR_DELTA_RE = re.compile(r"cursor\s*(?P<op>\+=|-=)\s*(?P<delta>.+)$")


def _parse_cursor_mapping_entries(body, mode):
    """
    Parse mapping entries by walking statements in source order and tracking `cursor`.
    Supports nested loops and loop indices like i0/i1.
    """
    text = _strip_cpp_comments(body)
    entries = []
    cursor_offset = 0

    def _process_statement(stmt, loop_vars):
        nonlocal cursor_offset
        if not stmt:
            return

        if mode == "pack":
            m_pack = _PACK_ASSIGN_RE.match(stmt)
            if m_pack:
                width = _safe_eval_int(m_pack.group("width"), loop_vars)
                if width is None:
                    return
                path = _substitute_loop_indices(m_pack.group("path"), loop_vars)
                entries.append({
                    "path": path,
                    "width": int(width),
                    "count": 1,
                    "offset": int(cursor_offset),
                })
                return
        else:
            m_unpack = _UNPACK_CALL_RE.match(stmt)
            if m_unpack:
                width = _safe_eval_int(m_unpack.group("width"), loop_vars)
                if width is None:
                    return
                path = _substitute_loop_indices(m_unpack.group("path"), loop_vars)
                entries.append({
                    "path": path,
                    "width": int(width),
                    "count": 1,
                    "offset": int(cursor_offset),
                })
                return

        m_cursor = _CURSOR_DELTA_RE.match(stmt)
        if m_cursor:
            delta = _safe_eval_int(m_cursor.group("delta"), loop_vars)
            if delta is None:
                return
            if m_cursor.group("op") == "+=":
                cursor_offset += int(delta)
            else:
                cursor_offset -= int(delta)

    def _parse_for_statement(src, for_pos, loop_vars, execute):
        # assumes src[for_pos:] starts with "for"
        pos = _skip_ws(src, for_pos + 3)
        if pos >= len(src) or src[pos] != "(":
            end = _find_stmt_end(src, for_pos)
            return end + (1 if end < len(src) and src[end] == ";" else 0)
        header_end = _find_matching(src, pos, "(", ")")
        if header_end < 0:
            return len(src)

        header = src[pos + 1:header_end]
        body_start = _skip_ws(src, header_end + 1)
        body_end = _parse_one_statement(src, body_start, loop_vars, execute=False)
        if body_end <= body_start:
            return body_start

        if execute:
            parsed = _parse_for_header(header, loop_vars)
            body_text = src[body_start:body_end]
            if parsed is None:
                _parse_block(body_text, dict(loop_vars), execute=True)
            else:
                var, values = parsed
                for v in values:
                    next_vars = dict(loop_vars)
                    next_vars[var] = int(v)
                    _parse_block(body_text, next_vars, execute=True)
        return body_end

    def _parse_one_statement(src, start_pos, loop_vars, execute):
        pos = _skip_ws(src, start_pos)
        if pos >= len(src):
            return pos

        if src.startswith("for", pos) and (pos + 3 == len(src) or not src[pos + 3].isalnum()):
            return _parse_for_statement(src, pos, loop_vars, execute)

        if src[pos] == "{":
            close = _find_matching(src, pos, "{", "}")
            if close < 0:
                return len(src)
            if execute:
                _parse_block(src[pos + 1:close], dict(loop_vars), execute=True)
            return close + 1

        end = _find_stmt_end(src, pos)
        stmt = src[pos:end].strip()
        if execute and stmt:
            _process_statement(stmt, loop_vars)
        if end < len(src) and src[end] == ";":
            return end + 1
        return end

    def _parse_block(block_src, loop_vars, execute):
        pos = 0
        while pos < len(block_src):
            next_pos = _parse_one_statement(block_src, pos, loop_vars, execute)
            if next_pos <= pos:
                break
            pos = next_pos

    _parse_block(text, {}, execute=True)
    return entries


def parse_pi_to_simulator(cpp_source, module_type):
    """
    Parse pi_to_simulator mapping as {path, width, count, offset}.

    Parsing is statement-order based (tracks cursor updates), so it naturally
    supports nested loops and loop indices like i0/i1.
    Each entry: {path, width, count, offset}.
    """
    # 当前实现依然是“文本解析 + 轻量语法走读”，不是完整 AST。
    # 但相比旧版只支持 `for (int i=...)` 的正则，这里可处理 i0/i1 嵌套循环。
    func_sig = rf"void\s+{re.escape(module_type)}::pi_to_simulator\s*\(\s*bool\s*\*\s*pi\s*\)\s*\{{"
    match = re.search(func_sig, cpp_source)
    if not match:
        return []

    body = _extract_braced_body(cpp_source, match.end())
    return _parse_cursor_mapping_entries(body, mode="pack")


def parse_simulator_to_po(cpp_source, module_type):
    """Parse simulator_to_po mapping as {path, width, count, offset}."""
    func_sig = rf"void\s+{re.escape(module_type)}::simulator_to_po\s*\(\s*bool\s*\*\s*po\s*\)\s*\{{"
    match = re.search(func_sig, cpp_source)
    if not match:
        return []

    body = _extract_braced_body(cpp_source, match.end())
    return _parse_cursor_mapping_entries(body, mode="unpack")


def collect_outputs(bsd_dir, module_type, cpp_source):
    """
    Collect bsd file info and output signals.
    Reads PO widths and optional unpack_bits from <dir_base>.h.
    If unpack_bits are absent in bsd file, parse outputs from cpp source's
    simulator_to_po.
    Returns (bsd_files_list, unique_outputs_list).
    """
    bsd_files = []
    all_outputs = []
    seen = set()
    dir_base = os.path.basename(bsd_dir)

    def _process_file(fpath, fname):
        content = _read_text(fpath)
        po_m = re.search(r"PO_WIDTH\s*=\s*(\d+)", content)
        if not po_m:
            return
        po_width = int(po_m.group(1))
        unpack_lines = re.findall(
            r"unpack_bits\s*\(\s*cursor\s*,\s*([^,]+)\s*,\s*(\d+)\s*\)\s*;",
            content,
        )
        bsd_files.append({
            "filename": fname,
            "po_width": po_width,
            "unpack_lines": [(sig.strip(), int(w)) for sig, w in unpack_lines],
        })
        for sig, w in unpack_lines:
            key = sig.strip()
            if key not in seen:
                seen.add(key)
                all_outputs.append({"path": key, "width": int(w)})

    # 优先信任 *_bsd.h 里的 unpack_bits，因为它最接近最终 wrapper。
    # 如果 wrapper 没写输出拆包，再退回 simulator_to_po() 文本解析。
    main_file = os.path.join(bsd_dir, f"{dir_base}.h")
    if not os.path.exists(main_file):
        raise ValueError(f"Missing bsd entry file: {main_file}")
    _process_file(main_file, f"{dir_base}.h")

    if bsd_files and not all_outputs:
        po_entries = parse_simulator_to_po(cpp_source, module_type)
        for entry in po_entries:
            path = entry["path"]
            w = entry["width"]
            count = entry["count"]
            if count > 1 and "[i]" in path:
                for idx in range(count):
                    expanded = path.replace("[i]", f"[{idx}]")
                    all_outputs.append({"path": expanded, "width": w})
            else:
                all_outputs.append({"path": path, "width": w})
        for bf in bsd_files:
            bf["unpack_lines"] = [(o["path"], o["width"]) for o in all_outputs]

    return bsd_files, all_outputs


class MappingProvider:
    """Interface for IO mapping strategy."""
    name = "pi_to_simulator_v1"

    def parse_inputs(self, cpp_source, module_type):
        return parse_pi_to_simulator(cpp_source, module_type)

    def collect_outputs(self, bsd_dir, module_type, cpp_source):
        return collect_outputs(bsd_dir, module_type, cpp_source)

    def generate_pi_sv(self, inputs, max_width=None):
        return cpp_helpers.generate_pi_sv(inputs, max_width=max_width)

    def generate_po_sv(self, output_signals, max_width=None):
        return cpp_helpers.generate_po_sv(output_signals, max_width=max_width)


_PROVIDERS = {
    "pi_to_simulator_v1": MappingProvider,
}


def get_mapping_provider(strategy=None):
    """Return a mapping provider instance for the given strategy name."""
    # 这里预留 provider 接口，是为了把“映射策略”与“当前正则实现”解耦。
    # 当前仓库只保留一个 provider，但后续换实现时最好沿用这层抽象。
    name = strategy or os.environ.get("SIM2V_IO_MAPPING") or "pi_to_simulator_v1"
    if name not in _PROVIDERS:
        raise ValueError(f"Unknown io_mapping strategy: {name}")
    return _PROVIDERS[name]()


def strategy_from_cfg(cfg):
    """Extract io_mapping.strategy from config dict."""
    if not cfg:
        return None
    return cfg.get("io_mapping", {}).get("strategy")
