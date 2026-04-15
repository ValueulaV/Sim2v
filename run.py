"""CLI entry point for the current sim2v workflow.

Supported commands:
  prompt   - Generate per-method LLM prompts as JSONL
  infer    - Call LLM on prompts and save snippet responses
  snippet  - Verify/debug per-method snippets before combine
  combine  - Assemble method snippets into complete Verilog modules
  verify   - Verify generated Verilog against C++ references
  pipeline - End-to-end: prompt -> infer -> snippet -> combine -> verify
"""

import os
import sys
import json
import argparse
try:
    import yaml
except ModuleNotFoundError as e:
    raise SystemExit(
        "Missing dependency: PyYAML (module 'yaml'). "
        "Install it (e.g. `pip install pyyaml`) or run with the project's configured environment."
    ) from e
import re
from datetime import datetime

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

from utils import setup_logger, parse_cpp_header, extract_verilog, extract_model_payload
import verify as verifier
import prompt_builder
import call_llm
import bsd_analyzer
import io_mapping
import combine_helpers
import snippet_stage


def load_config():
    with open(os.path.join(ROOT, "config.yaml")) as f:
        return yaml.safe_load(f)


def _get_mapping_provider(cfg):
    strategy = io_mapping.strategy_from_cfg(cfg)
    return io_mapping.get_mapping_provider(strategy)


def _output_root(cfg):
    return os.path.join(ROOT, cfg["output_dir"])


def _list_run_dirs(root):
    if not os.path.isdir(root):
        return []
    out = []
    for name in os.listdir(root):
        if not name.startswith("output_"):
            continue
        path = os.path.join(root, name)
        if os.path.isdir(path):
            out.append(path)
    # Sort by last modification time so we don't depend on directory naming format.
    return sorted(out, key=lambda p: (os.path.getmtime(p), p))


def _latest_run_dir(root):
    runs = _list_run_dirs(root)
    return runs[-1] if runs else None


def _make_unique_dir(parent, base_name):
    """Create a unique directory under parent.

    Uses second-level timestamps. If a collision occurs, suffix with _NNN.
    Returns (chosen_name, full_path).
    """
    os.makedirs(parent, exist_ok=True)
    for i in range(0, 1000):
        name = base_name if i == 0 else f"{base_name}_{i:03d}"
        path = os.path.join(parent, name)
        if not os.path.exists(path):
            os.makedirs(path, exist_ok=False)
            return name, path
    raise RuntimeError(f"Failed to create unique dir for base={base_name} under {parent}")


def _make_run_dir(root):
    ts = datetime.now().strftime("output_%Y%m%d_%H%M%S")
    _, path = _make_unique_dir(root, ts)
    return path


def _ensure_prompt_run_dir(root):
    latest = _latest_run_dir(root)
    if latest:
        prompt_path = os.path.join(latest, "prompts", "prompts.jsonl")
        if not os.path.exists(prompt_path):
            return latest
    return _make_run_dir(root)


def _require_latest_run_dir(root, action):
    latest = _latest_run_dir(root)
    if not latest:
        raise ValueError(f"No output run found for {action}. Run 'prompt' first.")
    return latest


def cmd_prompt(cfg, logger, run_dir):
    """Generate LLM prompts as JSONL."""
    # prompt 阶段只做“静态准备”：
    # 1) 解析模块/方法/类型信息
    # 2) 生成每个 method 的独立翻译任务
    # 3) 将任务落成 JSONL，供 infer 阶段并发调用 LLM
    out_path = os.path.join(run_dir, "prompts", "prompts.jsonl")
    prompt_cfg = cfg.get("prompt", {})
    struct_expand_depth = prompt_cfg.get("struct_expand_depth", 2)
    infer_use_think = cfg.get("llm", {}).get("infer_use_think", True)
    mapping_provider = _get_mapping_provider(cfg)
    count = prompt_builder.build_prompts(
        cfg["input_dirs"], out_path, ROOT,
        struct_expand_depth=struct_expand_depth,
        infer_use_think=infer_use_think,
        mapping_provider=mapping_provider,
    )
    logger.info(f"Generated {count} prompts -> {out_path}")
    logger.info(f"Prompt run dir: {run_dir}")


def cmd_infer(cfg, logger, run_dir):
    """Call LLM on prompt JSONL, save responses + per-method snippet .v files."""
    in_path = os.path.join(run_dir, "prompts", "prompts.jsonl")
    if not os.path.exists(in_path):
        raise ValueError(f"No prompts found at {in_path}. Run 'prompt' first.")
    out_path = os.path.join(run_dir, "responses", "responses.jsonl")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    run_id_base = datetime.now().strftime("infer_%Y%m%d_%H%M%S")
    infer_parent = os.path.join(run_dir, "infer_runs")
    run_id, record_dir = _make_unique_dir(infer_parent, run_id_base)

    max_tasks = cfg["llm"].get("max_tasks", 0)
    call_llm.run(
        in_path, out_path,
        cfg["llm"]["model"],
        cfg["llm"]["max_workers"],
        cfg["llm"]["sample_num"],
        logger,
        max_tasks=max_tasks,
        run_id=run_id,
        record_dir=record_dir,
    )
    logger.info(f"Infer run snapshot: {record_dir}")

    snippet_dir = os.path.join(run_dir, "snippets")
    os.makedirs(snippet_dir, exist_ok=True)
    infer_use_think = cfg.get("llm", {}).get("infer_use_think", True)
    with open(out_path) as f:
        for line in f:
            entry = json.loads(line)
            answer = extract_model_payload(entry["response"], infer_use_think)
            code = extract_verilog(answer)
            if not code:
                logger.warning(f"No verilog in response for {entry['task']}")
                continue
            # 这里把 JSONL 响应再展开成单独的 .sv 文件，目的是让后续人工覆写、
            # snippet 调试和 combine 都只面向“最终 snippet 文件”，而不是直接回读 response JSON。
            vpath = os.path.join(snippet_dir, f"{entry['task']}.sv")
            with open(vpath, "w") as vf:
                vf.write(code)
            logger.info(f"Snippet saved: {entry['task']}.sv")
            if entry.get("run_id") == run_id:
                task_dir = os.path.join(record_dir, entry["task"], entry.get("prompt_hash", ""))
                os.makedirs(task_dir, exist_ok=True)
                sample_idx = entry.get("sample_idx", 0)
                if isinstance(sample_idx, int):
                    suffix = f"{sample_idx:02d}"
                else:
                    suffix = "00"
                snippet_path = os.path.join(task_dir, f"snippet_{suffix}.sv")
                with open(snippet_path, "w") as sf:
                    sf.write(code)


def cmd_combine(cfg, logger, run_dir):
    """Assemble LLM method snippets into complete SystemVerilog modules.

    Generates a single module with SV struct types, always_comb block containing
    auto-generated IO mapping and LLM-generated method logic.
    """
    resp_path = os.path.join(run_dir, "responses", "responses.jsonl")
    if not os.path.exists(resp_path):
        logger.error(f"No responses found at {resp_path}, run 'infer' first")
        return

    snippets = {}
    infer_use_think = cfg.get("llm", {}).get("infer_use_think", True)
    with open(resp_path) as f:
        for line in f:
            entry = json.loads(line)
            answer = extract_model_payload(entry["response"], infer_use_think)
            code = extract_verilog(answer)
            if not code:
                logger.warning(f"No verilog in response for {entry['task']}")
                continue
            snippets[entry["task"]] = code

    # combine 之前优先读取 snippets/ 下的文件。
    # 这样用户可以在不改 responses.jsonl 的前提下，直接手工修某个 method 的 snippet。
    snippet_dir = os.path.join(run_dir, "snippets")
    if os.path.isdir(snippet_dir):
        for fname in sorted(os.listdir(snippet_dir)):
            if not fname.endswith(".sv"):
                continue
            task_key = os.path.splitext(fname)[0]
            fpath = os.path.join(snippet_dir, fname)
            with open(fpath) as sf:
                code = sf.read().strip()
            if code:
                snippets[task_key] = code

    logger.info(f"Loaded {len(snippets)} method snippets (responses + snippets override)")

    verilog_dir = os.path.join(run_dir, "verilog_llm")
    os.makedirs(verilog_dir, exist_ok=True)
    mapping_provider = _get_mapping_provider(cfg)
    default_strategy = cfg.get("combine", {}).get("default_init", "zero_all")

    for d in cfg["input_dirs"]:
        full_dir = os.path.join(ROOT, d)
        if not os.path.isdir(full_dir) or not os.path.basename(d).endswith("_bsd"):
            continue

        # combine_info 是 prompt / snippet / combine 三个阶段共享的“模块装配上下文”。
        # 这里集中拿到 method 顺序、typedef、变量声明、bsd 文件列表等信息，
        # 后面组合完整模块时不再重新推导。
        combine_info = prompt_builder.get_combine_info(d, ROOT, mapping_provider=mapping_provider)
        module_type = combine_info["module_type"]
        method_order = combine_info["method_order"]

        logger.info(
            f"Combining {module_type}: methods={method_order}, "
            f"bsd_files={len(combine_info['bsd_files'])}"
        )

        for bf in combine_info["bsd_files"]:
            # 这里不因为某个 method 缺失 snippet 而直接中断 combine，
            # 只记录 warning。这样可以保留当前版本的完整输出，便于人工定位。
            for method_name in method_order:
                task_key = f"{module_type}_{method_name}"
                if task_key not in snippets:
                    logger.warning(f"Missing snippet for {task_key} in {bf['module_name']}")
            sv_code = combine_helpers.build_combined_module_sv(
                bf,
                combine_info,
                snippets,
                default_strategy=default_strategy,
            )
            vpath = os.path.join(verilog_dir, f"{bf['module_name']}.sv")
            with open(vpath, "w") as vf:
                vf.write(sv_code)
            logger.info(f"  Combined: {bf['module_name']}.sv")


def cmd_verify(cfg, logger, run_dir, target=None):
    """Verify all generated Verilog against C++ references."""
    vcfg = cfg["verify"]
    results = {}
    mapping_provider = _get_mapping_provider(cfg)

    verilog_dirs = [os.path.join(run_dir, "verilog_llm")]

    for vdir in verilog_dirs:
        if not os.path.isdir(vdir):
            continue
        source = os.path.basename(vdir)
        for vfile in sorted(os.listdir(vdir)):
            if not (vfile.endswith(".v") or vfile.endswith(".sv")):
                continue
            name = os.path.splitext(vfile)[0]
            if target and name != target:
                continue

            cpp_path = _find_cpp(cfg, name)
            if not cpp_path:
                logger.warning(f"No cpp reference for {name}")
                continue

            info = parse_cpp_header(cpp_path)
            # output_signal_map 决定了 testbench 在 mismatch 时如何把 bit 位置反查成语义化信号名。
            # 没有它也能验证，但报错会退化成“第 N bit 错”，调试体验会差很多。
            output_signal_map = _build_output_signal_map(cpp_path, name, mapping_provider)
            with open(os.path.join(vdir, vfile)) as f:
                verilog_code = f.read()

            logger.info(f"Verifying {name} ({source})...")
            passed, msg = verifier.verify(
                cpp_path, verilog_code, name,
                info["pi_width"], info["po_width"],
                vcfg["exhaustive_threshold"],
                vcfg["max_test_vectors"],
                output_signal_map=output_signal_map,
                verilator_bin=cfg["verilator"],
                yosys_bin=cfg.get("yosys"),
                parallel_jobs=vcfg.get("parallel_jobs", 1),
                yosys_timeout_s=vcfg.get("yosys_timeout_s", 600),
                verilator_timeout_s=vcfg.get("verilator_timeout_s", 3600),
            )
            tag = "PASS" if passed else "FAIL"
            logger.info(f"  {tag}: {msg}")
            results[f"{source}/{name}"] = {"passed": passed, "message": msg}

    # Summary
    total = len(results)
    passed = sum(1 for r in results.values() if r["passed"])
    logger.info(f"Verification summary: {passed}/{total} passed")

    result_path = os.path.join(run_dir, "verify_results.json")
    os.makedirs(os.path.dirname(result_path), exist_ok=True)
    with open(result_path, "w") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)

    return results

def cmd_pipeline(cfg, logger, run_dir):
    """End-to-end: prompt -> infer -> snippet -> combine -> verify."""
    # pipeline 只是顺序编排，不引入额外逻辑。
    # 新同学调试时优先单独跑每一阶段；只有流程稳定后再跑 pipeline。
    logger.info("=== Step 1: Generate prompts ===")
    cmd_prompt(cfg, logger, run_dir)
    logger.info("=== Step 2: LLM inference ===")
    cmd_infer(cfg, logger, run_dir)
    if cfg.get("snippet", {}).get("enabled", True):
        logger.info("=== Step 3: Snippet-stage verify/debug ===")
        snippet_stage.run(cfg, logger, run_dir)
        logger.info("=== Step 4: Combine snippets ===")
    else:
        logger.info("=== Step 3: Combine snippets ===")
    cmd_combine(cfg, logger, run_dir)
    logger.info("=== Step 5: Verification ===")
    cmd_verify(cfg, logger, run_dir)


# ---- Helpers ----

def _find_cpp(cfg, module_name):
    """Locate cpp header for a module name across input_dirs."""
    for d in cfg["input_dirs"]:
        path = os.path.join(ROOT, d, f"{module_name}.h")
        if os.path.exists(path):
            return path
    return None


def _build_output_signal_map(cpp_path, module_name, mapping_provider):
    """Build output signal map for a bsd module from unpack_lines."""
    bsd_dir = os.path.dirname(cpp_path)
    if not os.path.basename(bsd_dir).endswith("_bsd"):
        return None

    # 这里复用 bsd_analyzer 的输出定义，而不是自己再解析一套，
    # 是为了让 full verify 和 snippet compare_plan 使用同一份信号视图。
    module_info = bsd_analyzer.analyze_module(
        bsd_dir, mapping_provider=mapping_provider,
    )
    for bf in module_info["bsd_files"]:
        if os.path.splitext(bf["filename"])[0] == module_name:
            offset = 0
            signal_map = []
            for sig_path, width in bf["unpack_lines"]:
                signal_map.append({
                    "path": sig_path,
                    "width": int(width),
                    "offset": offset,
                })
                offset += int(width)
            return signal_map
    return None

def main():
    # run_dir 的复用规则：
    # - prompt: 复用最近一次“只生成了 run_dir 但尚未落 prompts”的目录，否则新建
    # - infer/snippet/combine/verify: 默认接最近一次输出目录
    # - pipeline: 永远新建一轮完整输出目录
    # 这样可以同时兼顾单阶段迭代和整链路留档。
    parser = argparse.ArgumentParser(description="sim2v: C++ simulator -> Verilog")
    parser.add_argument(
        "command",
        choices=["prompt", "infer", "snippet", "combine", "verify", "pipeline"],
    )
    parser.add_argument("--target", help="Process only this module name")
    args = parser.parse_args()

    cfg = load_config()
    out_root = _output_root(cfg)

    run_dir = None
    if args.command == "prompt":
        run_dir = _ensure_prompt_run_dir(out_root)
    elif args.command in ("infer", "snippet", "combine", "verify"):
        run_dir = _require_latest_run_dir(out_root, args.command)
    elif args.command == "pipeline":
        run_dir = _make_run_dir(out_root)

    if not run_dir:
        run_dir = _make_run_dir(out_root)
    log_dir = os.path.join(run_dir, "logs")
    logger = setup_logger(args.command, log_dir)
    logger.info(f"Run dir: {run_dir}")

    dispatch = {
        "prompt": lambda: cmd_prompt(cfg, logger, run_dir),
        "infer": lambda: cmd_infer(cfg, logger, run_dir),
        "snippet": lambda: snippet_stage.run(cfg, logger, run_dir, args.target),
        "combine": lambda: cmd_combine(cfg, logger, run_dir),
        "verify": lambda: cmd_verify(cfg, logger, run_dir, args.target),
        "pipeline": lambda: cmd_pipeline(cfg, logger, run_dir),
    }
    dispatch[args.command]()


if __name__ == "__main__":
    main()
