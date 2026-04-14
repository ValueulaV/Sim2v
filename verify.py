"""Verilator-based verification: compare C++ reference with generated Verilog."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
SIMULATOR_INCLUDE = os.path.join(ROOT, "io_generator", "simulator_include")
COMPILE_MSG_MAX_CHARS = 10000


def _clip_text(text, limit):
    text = text or ""
    if len(text) <= limit:
        return text
    remain = len(text) - limit
    return text[:limit] + f"\n... [truncated {remain} chars]"


def _normalize_tool_path(path):
    return os.path.expanduser(path) if path else path


def verify(cpp_path, verilog_code, module_name, pi_width, po_width,
           exhaustive_threshold=20, max_test_vectors=100000,
           extra_cflags="", output_signal_map=None, verilator_bin="verilator",
           yosys_bin=None, artifact_dir=None, parallel_jobs=1):
    """
    Verify verilog against cpp reference via verilator simulation.
    Returns (passed: bool, message: str).
    """
    # verify 的阶段顺序是固定的：
    # 1) 物化 RTL + testbench
    # 2) 可选 yosys 前端语法检查
    # 3) Verilator 编译
    # 4) 运行随机或穷举向量
    exhaustive = pi_width <= exhaustive_threshold
    num_tests = (1 << pi_width) if exhaustive else max_test_vectors

    # Auto-detect if bsd file (needs simulator_include)
    with open(cpp_path) as f:
        cpp_content = f.read()
    needs_include = "<PRF.h>" in cpp_content or "<ROB.h>" in cpp_content
    if needs_include and not extra_cflags:
        extra_cflags = f"-I{SIMULATOR_INCLUDE}"
    elif needs_include and f"-I{SIMULATOR_INCLUDE}" not in extra_cflags:
        extra_cflags = f"{extra_cflags} -I{SIMULATOR_INCLUDE}".strip()

    if artifact_dir:
        paths = _materialize_verification_artifacts(
            cpp_path=cpp_path,
            verilog_code=verilog_code,
            module_name=module_name,
            pi_width=pi_width,
            po_width=po_width,
            num_tests=num_tests,
            exhaustive=exhaustive,
            extra_cflags=extra_cflags,
            output_signal_map=output_signal_map,
            verilator_bin=verilator_bin,
            yosys_bin=yosys_bin,
            artifact_dir=artifact_dir,
            parallel_jobs=parallel_jobs,
        )
        return _run_verification(paths)

    with tempfile.TemporaryDirectory() as tmpdir:
        paths = _materialize_verification_artifacts(
            cpp_path=cpp_path,
            verilog_code=verilog_code,
            module_name=module_name,
            pi_width=pi_width,
            po_width=po_width,
            num_tests=num_tests,
            exhaustive=exhaustive,
            extra_cflags=extra_cflags,
            output_signal_map=output_signal_map,
            verilator_bin=verilator_bin,
            yosys_bin=yosys_bin,
            artifact_dir=tmpdir,
            parallel_jobs=parallel_jobs,
        )
        return _run_verification(paths)


def compile_only(verilog_code, module_name, verilator_bin="verilator", yosys_bin=None, artifact_dir=None):
    """Run yosys + Verilator compile/lint checks on Verilog only."""
    # compile_only 给 snippet full-shell gate 使用：
    # 只回答“是否能编译”，不回答“功能是否等价”。
    if artifact_dir:
        return _run_compile_only(
            _materialize_compile_only_artifacts(
                verilog_code=verilog_code,
                module_name=module_name,
                verilator_bin=verilator_bin,
                yosys_bin=yosys_bin,
                artifact_dir=artifact_dir,
            )
        )

    with tempfile.TemporaryDirectory() as tmpdir:
        return _run_compile_only(
            _materialize_compile_only_artifacts(
                verilog_code=verilog_code,
                module_name=module_name,
                verilator_bin=verilator_bin,
                yosys_bin=yosys_bin,
                artifact_dir=tmpdir,
            )
        )


def _materialize_verification_artifacts(*, cpp_path, verilog_code, module_name, pi_width, po_width,
                                        num_tests, exhaustive, extra_cflags, output_signal_map,
                                        verilator_bin, yosys_bin, artifact_dir, parallel_jobs):
    # 把一次 verify 所需的全部产物显式落盘，便于：
    # - 失败后离线复盘
    # - 用户手工进入 build 目录继续调试
    # - snippet/full verify 复用同一条物化逻辑
    os.makedirs(artifact_dir, exist_ok=True)
    verilator_bin = _normalize_tool_path(verilator_bin)
    yosys_bin = _normalize_tool_path(yosys_bin)

    is_sv = "typedef struct" in verilog_code or "always_comb" in verilog_code
    ext = ".sv" if is_sv else ".v"
    rtl_path = os.path.join(artifact_dir, f"{module_name}{ext}")
    with open(rtl_path, "w") as f:
        f.write(verilog_code)

    abs_cpp = os.path.abspath(cpp_path)
    tb_code = _gen_testbench(
        module_name, abs_cpp, pi_width, po_width, num_tests, exhaustive,
        output_signal_map=output_signal_map,
    )
    tb_path = os.path.join(artifact_dir, f"tb_{module_name}.cpp")
    with open(tb_path, "w") as f:
        f.write(tb_code)

    meta = {
        "module_name": module_name,
        "cpp_path": abs_cpp,
        "rtl_path": rtl_path,
        "tb_path": tb_path,
        "pi_width": pi_width,
        "po_width": po_width,
        "num_tests": num_tests,
        "exhaustive": exhaustive,
        "extra_cflags": extra_cflags,
        "output_signal_map": output_signal_map or [],
        "verilator_bin": verilator_bin,
        "yosys_bin": yosys_bin,
        "parallel_jobs": max(1, int(parallel_jobs or 1)),
    }
    with open(os.path.join(artifact_dir, "verify_meta.json"), "w") as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)

    build_dir = os.path.join(artifact_dir, "build")
    cflags = f"-std=c++17 -O2 {extra_cflags}".strip()
    cmd = (
        f'{verilator_bin} -Wno-SYMRSVDWORD -Wno-WIDTHCONCAT --cc "{rtl_path}" --exe "{tb_path}" '
        f"--build --top-module {module_name} "
        f'--Mdir "{build_dir}" '
        f'-CFLAGS "{cflags}"'
    )
    return {
        "artifact_dir": artifact_dir,
        "module_name": module_name,
        "rtl_path": rtl_path,
        "tb_path": tb_path,
        "build_dir": build_dir,
        "cmd": cmd,
        "num_tests": num_tests,
        "exhaustive": exhaustive,
        "yosys_bin": yosys_bin,
        "parallel_jobs": max(1, int(parallel_jobs or 1)),
    }


def _materialize_compile_only_artifacts(*, verilog_code, module_name, verilator_bin, yosys_bin, artifact_dir):
    os.makedirs(artifact_dir, exist_ok=True)
    is_sv = "typedef struct" in verilog_code or "always_comb" in verilog_code
    ext = ".sv" if is_sv else ".v"
    rtl_path = os.path.join(artifact_dir, f"{module_name}{ext}")
    with open(rtl_path, "w") as f:
        f.write(verilog_code)
    return {
        "artifact_dir": artifact_dir,
        "module_name": module_name,
        "rtl_path": rtl_path,
        "verilator_bin": _normalize_tool_path(verilator_bin),
        "yosys_bin": _normalize_tool_path(yosys_bin),
    }


def _run_yosys_syntax_check(paths):
    yosys_bin = paths.get("yosys_bin")
    if not yosys_bin:
        return True, ""

    # 这里只把 yosys 当“前端兼容性检查器”，不做重综合。
    # `-defer -noopt -nomem2reg` 的目的是缩短大 harness 上的开销，
    # 同时尽量保留我们关心的前端语法/展开错误。
    script = (
        f'read_verilog -sv -defer -noopt -nomem2reg "{paths["rtl_path"]}"; '
        f'hierarchy -check -top {paths["module_name"]}'
    )
    cmd = [yosys_bin, "-q", "-p", script]
    ret = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    _write_text(os.path.join(paths["artifact_dir"], "yosys_stdout.txt"), ret.stdout or "")
    _write_text(os.path.join(paths["artifact_dir"], "yosys_stderr.txt"), ret.stderr or "")
    if ret.returncode == 0:
        return True, ""

    stdout = _clip_text((ret.stdout or "").strip(), COMPILE_MSG_MAX_CHARS)
    stderr = _clip_text((ret.stderr or "").strip(), COMPILE_MSG_MAX_CHARS)
    msg = [
        "YOSYS_SYNTAX_ERROR:",
        "---- Command ----",
        " ".join(cmd),
        "---- Yosys stdout ----",
        stdout if stdout else "<empty>",
        "---- Yosys stderr ----",
        stderr if stderr else "<empty>",
    ]
    return False, "\n".join(msg)


def _run_verification(paths):
    # 随机验证支持分 shard 并发：
    # - 编译只做一次
    # - 运行阶段把向量数量切给多个子进程
    # 这样比起重复编译多个 testbench 要便宜很多。
    yosys_ok, yosys_msg = _run_yosys_syntax_check(paths)
    if not yosys_ok:
        return False, yosys_msg

    ret = subprocess.run(paths["cmd"], shell=True, capture_output=True, text=True, timeout=1200)
    _write_text(os.path.join(paths["artifact_dir"], "compile_stdout.txt"), ret.stdout or "")
    _write_text(os.path.join(paths["artifact_dir"], "compile_stderr.txt"), ret.stderr or "")
    if ret.returncode != 0:
        stderr = _clip_text(ret.stderr.strip(), COMPILE_MSG_MAX_CHARS)
        stdout = _clip_text(ret.stdout.strip(), COMPILE_MSG_MAX_CHARS)
        msg = [
            "COMPILE_ERROR:",
            "---- Command ----",
            paths["cmd"],
            "---- Verilator stderr ----",
            stderr if stderr else "<empty>",
            "---- Verilator stdout ----",
            stdout if stdout else "<empty>",
        ]
        return False, "\n".join(msg)

    exe = os.path.join(paths["build_dir"], f"V{paths['module_name']}")
    jobs = max(1, int(paths.get("parallel_jobs", 1) or 1))
    if paths.get("exhaustive"):
        jobs = 1

    if jobs == 1:
        ret = subprocess.run(exe, capture_output=True, text=True, timeout=1200)
        _write_text(os.path.join(paths["artifact_dir"], "run_stdout.txt"), ret.stdout or "")
        _write_text(os.path.join(paths["artifact_dir"], "run_stderr.txt"), ret.stderr or "")
        if ret.returncode == 0 and "PASS" in ret.stdout:
            return True, ret.stdout.strip()

        msg = ret.stderr.strip() if ret.stderr.strip() else ret.stdout.strip()
        return False, "SIMULATION_ERROR:\n" + msg

    shard_dir = os.path.join(paths["artifact_dir"], "shards")
    os.makedirs(shard_dir, exist_ok=True)
    shard_counts = _split_counts(paths["num_tests"], jobs)
    futures = []
    results = []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for shard_id, count in enumerate(shard_counts):
            if count <= 0:
                continue
            futures.append(pool.submit(_run_one_shard, exe, shard_dir, shard_id, count, 42 + shard_id))
        for fut in as_completed(futures):
            results.append(fut.result())

    results.sort(key=lambda x: x["shard_id"])
    _write_text(
        os.path.join(paths["artifact_dir"], "run_stdout.txt"),
        "\n".join(r["stdout"].rstrip() for r in results if r["stdout"]).rstrip() + ("\n" if any(r["stdout"] for r in results) else ""),
    )
    _write_text(
        os.path.join(paths["artifact_dir"], "run_stderr.txt"),
        "\n".join(r["stderr"].rstrip() for r in results if r["stderr"]).rstrip() + ("\n" if any(r["stderr"] for r in results) else ""),
    )

    failed = [r for r in results if r["returncode"] != 0 or "PASS" not in r["stdout"]]
    if not failed:
        total = sum(r["count"] for r in results)
        return True, f"PASS: {total} vectors verified across {len(results)} shards."

    first = failed[0]
    msg = first["stderr"].strip() if first["stderr"].strip() else first["stdout"].strip()
    return False, f"SIMULATION_ERROR:\n[shard {first['shard_id']}] {msg}"


def _run_compile_only(paths):
    yosys_ok, yosys_msg = _run_yosys_syntax_check(paths)
    if not yosys_ok:
        return False, yosys_msg

    cmd = [
        paths["verilator_bin"],
        "-Wno-SYMRSVDWORD",
        "-Wno-WIDTHCONCAT",
        "--lint-only",
        "--top-module", paths["module_name"],
        paths["rtl_path"],
    ]
    ret = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
    _write_text(os.path.join(paths["artifact_dir"], "compile_stdout.txt"), ret.stdout or "")
    _write_text(os.path.join(paths["artifact_dir"], "compile_stderr.txt"), ret.stderr or "")
    if ret.returncode == 0:
        return True, ""

    stderr = _clip_text((ret.stderr or "").strip(), COMPILE_MSG_MAX_CHARS)
    stdout = _clip_text((ret.stdout or "").strip(), COMPILE_MSG_MAX_CHARS)
    msg = [
        "COMPILE_ERROR:",
        "---- Command ----",
        " ".join(cmd),
        "---- Verilator stderr ----",
        stderr if stderr else "<empty>",
        "---- Verilator stdout ----",
        stdout if stdout else "<empty>",
    ]
    return False, "\n".join(msg)


def _split_counts(total, jobs):
    base = total // jobs
    rem = total % jobs
    return [base + (1 if i < rem else 0) for i in range(jobs)]


def _run_one_shard(exe, shard_dir, shard_id, count, seed):
    cmd = [exe, "--count", str(count), "--seed", str(seed)]
    ret = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
    _write_text(os.path.join(shard_dir, f"shard_{shard_id:02d}_stdout.txt"), ret.stdout or "")
    _write_text(os.path.join(shard_dir, f"shard_{shard_id:02d}_stderr.txt"), ret.stderr or "")
    return {
        "shard_id": shard_id,
        "count": count,
        "seed": seed,
        "returncode": ret.returncode,
        "stdout": ret.stdout or "",
        "stderr": ret.stderr or "",
    }


def _gen_testbench(module_name, cpp_path, pi_width, po_width, num_tests, exhaustive,
                   output_signal_map=None):
    """Generate C++ testbench that compares DUT output with C++ reference."""
    signal_meta = _signal_meta_code(output_signal_map or [])
    signal_bookkeeping = _signal_bookkeeping_code(output_signal_map or [])
    rand_input = _random_input_code(pi_width, exhaustive)
    compare_po = _compare_po_code("po", po_width, signal_bookkeeping)

    if exhaustive:
        loop = (
            f"    const uint64_t total_all = 1ULL << {pi_width};\n"
            f"    uint64_t start = 0;\n"
            f"    uint64_t total = total_all;\n"
            f"    if (argc >= 3 && std::string(argv[1]) == \"--start\") start = std::strtoull(argv[2], nullptr, 10);\n"
            f"    if (argc >= 5 && std::string(argv[3]) == \"--count\") total = std::strtoull(argv[4], nullptr, 10);\n"
            f"    for (uint64_t local = 0; local < total; ++local) {{\n"
            f"        uint64_t vec = start + local;\n"
            f"{rand_input}"
        )
    else:
        loop = (
            f"    uint64_t seed = 42;\n"
            f"    uint64_t total = {num_tests};\n"
            f"    for (int ai = 1; ai + 1 < argc; ai += 2) {{\n"
            f"        if (std::string(argv[ai]) == \"--count\") total = std::strtoull(argv[ai + 1], nullptr, 10);\n"
            f"        else if (std::string(argv[ai]) == \"--seed\") seed = std::strtoull(argv[ai + 1], nullptr, 10);\n"
            f"    }}\n"
            f"    std::mt19937_64 rng(seed);\n"
            f"    for (uint64_t vec = 0; vec < total; ++vec) {{\n"
            f"{rand_input}"
        )

    return f"""\
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include "V{module_name}.h"
#include "verilated.h"
#include "{cpp_path}"

int main(int argc, char** argv) {{
    Verilated::commandArgs(argc, argv);
    V{module_name} dut;
    bool pi[{pi_width}];
    bool po_ref[{po_width}];
    int mismatches = 0;
{signal_meta}

{loop}

        io_generator_outer(pi, po_ref);

        dut.eval();
{compare_po}
    }}

    if (mismatches == 0) {{
        std::cout << "PASS: " << total << " vectors verified.\\n";
        return 0;
    }}
    std::cerr << "FAIL: " << mismatches << " mismatches.\\n";
{_signal_summary_code(output_signal_map or [])}
    return 1;
}}
"""


def _pack_code(name, width):
    """Generate C++ code to pack bool[] into Verilator signal."""
    if width <= 32:
        return (
            f"        uint32_t packed = 0;\n"
            f"        for (int i = 0; i < {width}; ++i) if ({name}[i]) packed |= (1u << i);\n"
            f"        dut.{name} = packed;"
        )
    elif width <= 64:
        return (
            f"        uint64_t packed = 0;\n"
            f"        for (int i = 0; i < {width}; ++i) if ({name}[i]) packed |= (1ULL << i);\n"
            f"        dut.{name} = packed;"
        )
    else:
        nw = (width + 31) // 32
        return (
            f"        for (int w = 0; w < {nw}; ++w) dut.{name}[w] = 0;\n"
            f"        for (int i = 0; i < {width}; ++i)\n"
            f"            if ({name}[i]) dut.{name}[i/32] |= (1u << (i%32));"
        )


def _read_expr(name, width):
    """Generate C++ expression to read bit i from Verilator signal."""
    if width <= 64:
        return f"((dut.{name} >> i) & 1) != 0"
    return f"((dut.{name}[i/32] >> (i%32)) & 1u) != 0"


def _compare_po_code(name, width, signal_bookkeeping):
    # DUT/REF 比较采用“按 32-bit word 扫描，再在不相等的 word 内细化到 bit”。
    # 这样既能支持大位宽 po，也能在 mismatch 时保留信号级定位信息。
    words = (width + 31) // 32
    lines = []

    if width <= 32:
        lines.append(f"        uint32_t dut_words[1] = {{static_cast<uint32_t>(dut.{name})}};")
    elif width <= 64:
        lines.append("        uint32_t dut_words[2];")
        lines.append(f"        uint64_t dut_packed = static_cast<uint64_t>(dut.{name});")
        lines.append("        dut_words[0] = static_cast<uint32_t>(dut_packed & 0xffffffffULL);")
        lines.append("        dut_words[1] = static_cast<uint32_t>((dut_packed >> 32) & 0xffffffffULL);")
    else:
        lines.append(f"        uint32_t* dut_words = dut.{name};")

    lines.extend([
        f"        for (int w = 0; w < {words}; ++w) {{",
        "            uint32_t ref_word = 0;",
        "            int base = w * 32;",
        f"            int limit = ((base + 32) <= {width}) ? 32 : ({width} - base);",
        "            for (int b = 0; b < limit; ++b) {",
        "                if (po_ref[base + b]) ref_word |= (1u << b);",
        "            }",
        "            uint32_t dut_word = dut_words[w];",
        "            if (dut_word == ref_word) continue;",
        "            for (int b = 0; b < limit; ++b) {",
        "                int i = base + b;",
        "                bool dut_bit = ((dut_word >> b) & 1u) != 0;",
        "                if (dut_bit != po_ref[i]) {",
    ])
    lines.extend(signal_bookkeeping.rstrip().splitlines())
    lines.extend([
        "                    ++mismatches;",
        "                }",
        "            }",
        "        }",
    ])
    return "\n".join(lines)


def _random_input_code(width, exhaustive):
    if width <= 64:
        if exhaustive:
            return (
                f"        uint64_t packed = vec;\n"
                f"        for (int i = 0; i < {width}; ++i) pi[i] = ((packed >> i) & 1ULL) != 0;\n"
                f"        dut.pi = packed;\n"
            )
        return (
            f"        uint64_t packed = rng();\n"
            f"        for (int i = 0; i < {width}; ++i) pi[i] = ((packed >> i) & 1ULL) != 0;\n"
            f"        dut.pi = packed;\n"
        )
    nw = (width + 31) // 32
    lines = [f"        for (int w = 0; w < {nw}; ++w) dut.pi[w] = 0;"]
    for w in range(nw):
        if exhaustive and w == 0:
            word_expr = "static_cast<uint32_t>(vec)"
        elif exhaustive:
            word_expr = "0u"
        else:
            word_expr = "static_cast<uint32_t>(rng())"
        lines.append(f"        uint32_t word_{w} = {word_expr};")
        lines.append(f"        dut.pi[{w}] = word_{w};")
        base = w * 32
        upper = min(width, base + 32)
        lines.append(
            f"        for (int b = {base}; b < {upper}; ++b) pi[b] = ((word_{w} >> (b - {base})) & 1u) != 0;"
        )
    return "\n".join(lines) + "\n"


def _signal_meta_code(output_signal_map):
    """Generate C++ tables for bit->signal lookup."""
    if not output_signal_map:
        return ""

    starts = ", ".join(str(s["offset"]) for s in output_signal_map)
    widths = ", ".join(str(s["width"]) for s in output_signal_map)
    escaped_names = []
    for s in output_signal_map:
        escaped = s["path"].replace('"', '\\"')
        escaped_names.append(f"\"{escaped}\"")
    names = ", ".join(escaped_names)
    n = len(output_signal_map)

    return (
        f"    const int sig_n = {n};\n"
        f"    const int sig_start[{n}] = {{{starts}}};\n"
        f"    const int sig_width[{n}] = {{{widths}}};\n"
        f"    const char* sig_name[{n}] = {{{names}}};\n"
        f"    int sig_mismatch[{n}] = {{0}};\n"
    )


def _signal_bookkeeping_code(output_signal_map):
    """Generate C++ mismatch print/count logic."""
    if not output_signal_map:
        return (
            "                if (mismatches < 20)\n"
            "                    std::cerr << \"MISMATCH vec=\" << vec << \" bit=\" << i\n"
            "                              << \" dut=\" << dut_bit << \" ref=\" << po_ref[i] << \"\\n\";\n"
        )

    return (
        "                int sig_idx = -1;\n"
        "                int rel_bit = -1;\n"
        "                for (int s = 0; s < sig_n; ++s) {\n"
        "                    if (i >= sig_start[s] && i < sig_start[s] + sig_width[s]) {\n"
        "                        sig_idx = s;\n"
        "                        rel_bit = i - sig_start[s];\n"
        "                        break;\n"
        "                    }\n"
        "                }\n"
        "                if (sig_idx >= 0) sig_mismatch[sig_idx]++;\n"
        "                if (mismatches < 20) {\n"
        "                    std::cerr << \"MISMATCH vec=\" << vec << \" bit=\" << i;\n"
        "                    if (sig_idx >= 0)\n"
        "                        std::cerr << \" sig=\" << sig_name[sig_idx] << \"[\" << rel_bit << \"]\";\n"
        "                    std::cerr << \" dut=\" << dut_bit << \" ref=\" << po_ref[i] << \"\\n\";\n"
        "                }\n"
    )


def _signal_summary_code(output_signal_map):
    """Generate per-signal mismatch summary block."""
    if not output_signal_map:
        return ""
    return (
        "    std::cerr << \"MISMATCH_BY_SIGNAL (top non-zero):\\n\";\n"
        "    int printed = 0;\n"
        "    for (int s = 0; s < sig_n; ++s) {\n"
        "        if (sig_mismatch[s] > 0) {\n"
        "            std::cerr << \"  \" << sig_name[s] << \": \" << sig_mismatch[s] << \"\\n\";\n"
        "            ++printed;\n"
        "            if (printed >= 40) break;\n"
        "        }\n"
        "    }\n"
    )


def _write_text(path, text):
    with open(path, "w") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser(description="Generate and optionally run a persistent Verilator testbench.")
    parser.add_argument("--cpp-path", required=True, help="Path to C++ reference header containing io_generator_outer()")
    parser.add_argument("--rtl-path", required=True, help="Path to Verilog/SystemVerilog DUT file")
    parser.add_argument("--module-name", required=True, help="Top module name for Verilator")
    parser.add_argument("--pi-width", type=int, required=True, help="Input bit width")
    parser.add_argument("--po-width", type=int, required=True, help="Output bit width")
    parser.add_argument("--out-dir", required=True, help="Directory to write DUT/tb/build artifacts")
    parser.add_argument("--signal-map", help="Optional JSON file containing output signal map")
    parser.add_argument("--exhaustive-threshold", type=int, default=20)
    parser.add_argument("--max-test-vectors", type=int, default=100000)
    parser.add_argument("--parallel-jobs", type=int, default=1, help="Parallel shard count for random verification")
    parser.add_argument("--extra-cflags", default="")
    parser.add_argument("--verilator-bin", default="verilator")
    parser.add_argument("--yosys-bin", default=None)
    args = parser.parse_args()

    with open(args.rtl_path) as f:
        verilog_code = f.read()

    output_signal_map = None
    if args.signal_map:
        with open(args.signal_map) as f:
            output_signal_map = json.load(f)

    passed, message = verify(
        cpp_path=args.cpp_path,
        verilog_code=verilog_code,
        module_name=args.module_name,
        pi_width=args.pi_width,
        po_width=args.po_width,
        exhaustive_threshold=args.exhaustive_threshold,
        max_test_vectors=args.max_test_vectors,
        extra_cflags=args.extra_cflags,
        output_signal_map=output_signal_map,
        verilator_bin=args.verilator_bin,
        yosys_bin=args.yosys_bin,
        artifact_dir=args.out_dir,
        parallel_jobs=args.parallel_jobs,
    )
    print(message)
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
