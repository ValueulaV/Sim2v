"""IO mapping strategy for pi/po packing and unpacking."""

import os
import re

import cpp_helpers


def _read_text(path):
    with open(path) as f:
        return f.read()


def parse_pi_to_simulator(cpp_source, module_type):
    """
    Parse pi_to_simulator in ROB-style mapping form (comment-offset based).
    Each entry: {path, width, count, offset}.
    """
    # 当前 io_mapping 的核心假设是：
    # pi_to_simulator / simulator_to_po 的代码风格比较规整，
    # 并且 offset 信息可以从注释中恢复出来。
    # 这是实现方式，不是必须坚持的框架策略；后续完全可以换成 AST 方案。
    func_sig = rf"void\s+{re.escape(module_type)}::pi_to_simulator\s*\(bool\*\s*pi\)\s*\{{"
    match = re.search(func_sig, cpp_source)
    if not match:
        return []

    body = _extract_braced_body(cpp_source, match.end())
    entries = []
    sig_path = r"[\w]+(?:(?:\.|->)[\w]+|\[\w+\])*"
    loop_pat = re.compile(
        r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*(\d+)\s*;\s*i\+\+\s*\)\s*\{\s*"
        r"(" + sig_path + r")\s*=\s*pack_bits<[^>]+>\s*\(cursor\s*,\s*(\d+)\)\s*;"
        r"\s*//(\d+)\s*(?:cursor\s*\+=\s*\d+\s*;\s*)?\}",
        re.DOTALL,
    )
    single_pat = re.compile(
        r"(" + sig_path + r")\s*=\s*pack_bits<[^>]+>\s*\(cursor\s*,\s*(\d+)\)\s*;\s*//(\d+)",
    )
    segments = []
    loop_ranges = []
    for m in loop_pat.finditer(body):
        segments.append((int(m.group(4)), m.group(2).strip(), int(m.group(3)), int(m.group(1))))
        loop_ranges.append((m.start(), m.end()))
    for m in single_pat.finditer(body):
        pos = m.start()
        if any(s <= pos < e for s, e in loop_ranges):
            continue
        segments.append((int(m.group(3)), m.group(1).strip(), int(m.group(2)), 1))
    segments.sort(key=lambda x: x[0])
    for off, path, width, count in segments:
        entries.append({"path": path, "width": width, "count": count, "offset": off})

    return entries


def parse_simulator_to_po(cpp_source, module_type):
    """Parse simulator_to_po in ROB-style mapping form (comment-offset based)."""
    func_sig = rf"void\s+{re.escape(module_type)}::simulator_to_po\s*\(bool\*\s*po\)\s*\{{"
    match = re.search(func_sig, cpp_source)
    if not match:
        return []

    body = _extract_braced_body(cpp_source, match.end())
    entries = []

    sig_path = r"[\w]+(?:(?:\.|->)[\w]+|\[\w+\])*"

    loop_pat = re.compile(
        r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*(\d+)\s*;\s*i\+\+\s*\)\s*\{\s*"
        r"unpack_bits\s*\(\s*cursor\s*,\s*(" + sig_path + r")\s*,\s*(\d+)\s*\)\s*;\s*//(\d+)"
        r"\s*(?:cursor\s*\+=\s*\d+\s*;\s*)?\}",
        re.DOTALL,
    )
    single_pat = re.compile(
        r"unpack_bits\s*\(\s*cursor\s*,\s*(" + sig_path + r")\s*,\s*(\d+)\s*\)\s*;\s*//(\d+)"
    )

    segments = []
    loop_ranges = []
    for m in loop_pat.finditer(body):
        segments.append((int(m.group(4)), m.group(2).strip(), int(m.group(3)), int(m.group(1))))
        loop_ranges.append((m.start(), m.end()))
    for m in single_pat.finditer(body):
        pos = m.start()
        if any(s <= pos < e for s, e in loop_ranges):
            continue
        segments.append((int(m.group(3)), m.group(1).strip(), int(m.group(2)), 1))

    segments.sort(key=lambda x: x[0])
    for off, path, width, count in segments:
        entries.append({"path": path, "width": width, "count": count, "offset": off})

    return entries


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
            if count > 1:
                for idx in range(count):
                    expanded = path.replace("[i]", f"[{idx}]")
                    all_outputs.append({"path": expanded, "width": w})
            else:
                all_outputs.append({"path": path, "width": w})
        for bf in bsd_files:
            bf["unpack_lines"] = [(o["path"], o["width"]) for o in all_outputs]

    return bsd_files, all_outputs


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


class MappingProvider:
    """Interface for IO mapping strategy."""
    name = "pi_to_simulator_v1"

    def parse_inputs(self, cpp_source, module_type):
        return parse_pi_to_simulator(cpp_source, module_type)

    def collect_outputs(self, bsd_dir, module_type, cpp_source):
        return collect_outputs(bsd_dir, module_type, cpp_source)

    def generate_pi_sv(self, inputs):
        return cpp_helpers.generate_pi_sv(inputs)

    def generate_po_sv(self, output_signals):
        return cpp_helpers.generate_po_sv(output_signals)


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
