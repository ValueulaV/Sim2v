"""Method-level snippet verification and repair orchestration."""

import os
import threading
import time
import math
import re
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime

import bsd_analyzer
import call_llm
import combine_helpers
import prompt_builder
import snippet_harness as harness
import verify as verifier
from utils import extract_model_payload, extract_verilog


class _VerifyShardBudget:
    def __init__(self, total_tokens, total_methods):
        self.total_tokens = max(1, int(total_tokens))
        self.available = self.total_tokens
        self.remaining_methods = max(1, int(total_methods))
        self._cv = threading.Condition()

    def acquire(self, requested):
        requested = max(1, int(requested))
        with self._cv:
            # 这里做的是“全局 verify shard 预算”的公平分配，
            # 不是简单的每个 method 固定 parallel_jobs。
            # 这样可以在前期避免所有 method 同时抢满核，
            # 后期也允许剩余 method 自动拿到更多 shard。
            while self.available <= 0:
                self._cv.wait()
            fair_share = max(1, self.total_tokens // max(1, self.remaining_methods))
            grant = min(requested, fair_share, self.available)
            self.available -= grant
            return grant

    def release(self, granted):
        granted = max(0, int(granted))
        with self._cv:
            self.available = min(self.total_tokens, self.available + granted)
            self._cv.notify_all()

    def finish_method(self):
        with self._cv:
            self.remaining_methods = max(1, self.remaining_methods - 1)
            self._cv.notify_all()


DEFAULT_SNIPPET_MAX_TEST_VECTORS = 100_000


def run(cfg, logger, run_dir, target=None):
    scfg = cfg.get("snippet", {})
    if not scfg.get("enabled", True):
        logger.info("Snippet stage disabled by config")
        return {}

    snippet_dir = os.path.join(run_dir, "snippets")
    if not os.path.isdir(snippet_dir):
        raise ValueError(f"No snippets found at {snippet_dir}. Run 'infer' first.")

    base_name = datetime.now().strftime("snippet_%Y%m%d_%H%M%S")
    run_id, out_dir = harness.make_unique_dir(os.path.join(run_dir, "snippet_debug"), base_name)
    logger.info(f"Snippet-stage run: {os.path.join(run_dir, 'snippet_debug', run_id)}")

    provider, client = call_llm.get_client(cfg["llm"]["model"])
    results = {}
    tasks = []
    max_workers = int(scfg.get("max_workers", cfg["llm"].get("max_workers", 4)))
    requested_parallel_jobs = int(scfg.get("parallel_jobs", 1))
    verify_max_test_vectors = int(cfg["verify"]["max_test_vectors"])
    snippet_max_test_vectors = int(
        scfg.get("max_test_vectors", min(verify_max_test_vectors, DEFAULT_SNIPPET_MAX_TEST_VECTORS))
    )
    cpu_count = os.cpu_count() or 1
    verify_budget = int(scfg.get("verify_process_budget", cpu_count))
    verify_budget = max(1, min(verify_budget, cpu_count))
    if "max_test_vectors" not in scfg and verify_max_test_vectors > snippet_max_test_vectors:
        logger.info(
            "Snippet max_test_vectors not set; using capped default "
            f"{snippet_max_test_vectors} instead of verify.max_test_vectors={verify_max_test_vectors}."
        )
    logger.info(
        f"Snippet concurrency: max_workers={max_workers}, requested_parallel_jobs={requested_parallel_jobs}, "
        f"verify_process_budget={verify_budget}, cpu_count={cpu_count}, "
        f"worst_case_peak_verify_processes~{min(verify_budget, max_workers * max(1, requested_parallel_jobs))}, "
        f"max_test_vectors={snippet_max_test_vectors}"
    )
    if max_workers * max(1, requested_parallel_jobs) > cpu_count * 2:
        logger.warning(
            "Snippet concurrency may be too aggressive for this host; "
            "verify subprocesses can dominate runtime when max_workers * parallel_jobs is large. "
            "Runtime shard allocation is capped by a global verify_process_budget."
        )
    repo_root = os.path.dirname(os.path.abspath(__file__))

    # 先把所有 method 任务一次性枚举出来，再统一交给线程池。
    # 这样 shard 预算器才能知道“总共有多少 method 尚未完成”，
    # 从而做动态公平分配。
    for rel_dir in cfg["input_dirs"]:
        full_dir = os.path.join(repo_root, rel_dir)
        if not os.path.isdir(full_dir) or not os.path.basename(rel_dir).endswith("_bsd"):
            continue

        module_info = bsd_analyzer.analyze_module(full_dir)
        if target and not _target_matches_module(target, module_info):
            continue

        combine_info = prompt_builder.get_combine_info(rel_dir, repo_root)
        helper_db = bsd_analyzer.build_helper_db(module_info)
        all_constants = bsd_analyzer.parse_all_constants()
        method_ctx = _build_method_context(module_info, combine_info, helper_db, all_constants)
        wrapper_header = prompt_builder._find_io_generator_outer_header(full_dir)
        if not wrapper_header:
            logger.warning(f"Skip {module_info['module_type']}: io_generator_outer header not found")
            continue
        with open(wrapper_header) as f:
            wrapper_text = f.read()
        instance_name = harness.parse_instance_name(wrapper_text, module_info["module_type"])
        methods_by_name = {method["name"]: method for method in module_info["methods"]}

        # method 遍历顺序与 combine 保持一致，避免 snippet 阶段和 full module 看到不同的调用顺序。
        for method_name in combine_info["method_order"]:
            method = methods_by_name.get(method_name)
            if not method:
                continue
            task_name = f"{module_info['module_type']}_{method['name']}"
            if target and target not in (task_name, module_info["module_type"], module_info["module_type"].lower()):
                continue
            snippet_path = os.path.join(snippet_dir, f"{task_name}.sv")
            if not os.path.exists(snippet_path):
                logger.warning(f"Missing snippet: {snippet_path}")
                continue

            # snippet harness 的边界来自“read-set / write-set -> signal plan”，
            # 而不是直接把整个模块接口全塞进来。
            loop_domains = harness.collect_loop_domains(method["body"])
            input_targets, output_targets = harness.build_method_io_targets(
                module_info,
                method["name"],
                int(scfg.get("max_compare_targets", 8)),
            )
            if not output_targets:
                logger.warning(f"Skip {task_name}: no writable compare targets identified")
                continue
            input_plan = harness.build_signal_plan(
                module_info,
                input_targets,
                instance_name,
                loop_domains=loop_domains,
                respect_dependencies=True,
            )
            compare_plan = harness.build_signal_plan(
                module_info,
                output_targets,
                instance_name,
                loop_domains=loop_domains,
            )
            if not compare_plan["leaves"]:
                logger.warning(f"Skip {task_name}: compare plan resolved to empty leaves")
                continue
            # 避免把 compare 目标回灌成 snippet 自由输入。
            # 这会诱导模型“读当前 out 再写 out”，并显著放大 input harness 规模。
            # 对 init 这类“大量输出需要在方法内构造”的初始化方法，保留输出回灌输入，
            # 否则会把关键配置常量全部移掉，难以复现 C++ 初始化语义。
            compare_labels = [leaf.get("label") for leaf in compare_plan["leaves"] if leaf.get("label")]
            if method["name"] != "init":
                input_plan = harness.drop_leaves_from_plan(input_plan, compare_labels)

            task_dir = os.path.join(out_dir, task_name)
            os.makedirs(task_dir, exist_ok=True)
            harness.write_json(os.path.join(task_dir, "input_plan.json"), input_plan)
            harness.write_json(os.path.join(task_dir, "compare_plan.json"), compare_plan)
            tasks.append({
                "task_name": task_name,
                "task_dir": task_dir,
                "module_info": module_info,
                "combine_info": combine_info,
                "method": method,
                "method_ctx": method_ctx[method["name"]],
                "snippet_path": snippet_path,
                "wrapper_text": wrapper_text,
                "instance_name": instance_name,
                "input_plan": input_plan,
                "compare_plan": compare_plan,
            })

    shard_budget = _VerifyShardBudget(total_tokens=verify_budget, total_methods=max(1, len(tasks)))
    initial_fair_share = max(1, verify_budget // max(1, len(tasks)))
    initial_vectors_per_shard = math.ceil(snippet_max_test_vectors / initial_fair_share)
    logger.info(
        f"Initial fair-share shards/method={initial_fair_share}, "
        f"initial vectors/shard≈{initial_vectors_per_shard}"
    )
    if initial_vectors_per_shard > 2_000_000:
        logger.warning(
            "Snippet vectors per initial shard are very high; early pass cases may run for a long time or hit shard timeouts. "
            "Lower snippet.max_test_vectors or raise verify_process_budget if you need stronger snippet checking."
        )

    # method 级并发由线程池控制；每个 method 内部再通过 verify_parallel_jobs 分裂成 shard。
    # 两级并发之间的协调由 _VerifyShardBudget 完成。
    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = []
        for task in tasks:
            futures.append((
                task["task_name"],
                pool.submit(
                    _run_one_method,
                    cfg=cfg,
                    logger=logger,
                    provider=provider,
                    client=client,
                    shard_budget=shard_budget,
                    requested_parallel_jobs=requested_parallel_jobs,
                    snippet_max_test_vectors=snippet_max_test_vectors,
                    task_dir=task["task_dir"],
                    module_info=task["module_info"],
                    combine_info=task["combine_info"],
                    method=task["method"],
                    method_ctx=task["method_ctx"],
                    snippet_path=task["snippet_path"],
                    wrapper_text=task["wrapper_text"],
                    instance_name=task["instance_name"],
                    input_plan=task["input_plan"],
                    compare_plan=task["compare_plan"],
                ),
            ))

        for task_name, future in futures:
            results[task_name] = future.result()

    harness.write_json(os.path.join(out_dir, "snippet_results.json"), results)
    passed = sum(1 for item in results.values() if item.get("passed"))
    logger.info(f"Snippet-stage summary: {passed}/{len(results)} passed")
    return results


def _run_one_method(*, cfg, logger, provider, client, task_dir, module_info,
                    combine_info, method, method_ctx, snippet_path, wrapper_text,
                    instance_name, input_plan, compare_plan, shard_budget,
                    requested_parallel_jobs, snippet_max_test_vectors):
    scfg = cfg.get("snippet", {})
    max_iterations = int(scfg.get("max_iterations", 2))
    debug_use_think = cfg.get("llm", {}).get("debug_use_think", False)

    try:
        for step_idx in range(max_iterations + 1):
            step_dir = os.path.join(task_dir, f"step_{step_idx:03d}")
            os.makedirs(step_dir, exist_ok=True)

            # 每一轮都重新从 snippets/ 读当前版本，保证：
            # 1) 调试后的补丁会成为下一轮输入
            # 2) 用户手工改动 snippets/ 也能被这一轮看到
            with open(snippet_path) as f:
                snippet_code = f.read().strip()
            harness.write_text(os.path.join(step_dir, "snippet_before.sv"), snippet_code + "\n")

            static_fail = _snippet_static_guard(module_info["module_type"], method["name"], snippet_code)

            module_name = f"snippet_{module_info['module_type'].lower()}_{method['name']}"
            # 单方法验证依赖一对对称的产物：
            # - SV wrapper：把当前 snippet 放进可编译可验证的 always_comb 壳里
            # - C++ reference：只执行同一个 method，并按 compare_plan 打包输出
            sv_code = harness.build_sv_wrapper(
                module_name,
                combine_info,
                method["name"],
                snippet_code,
                input_plan,
                compare_plan,
            )
            ref_header = harness.build_cpp_reference(
                wrapper_text=wrapper_text,
                module_info=module_info,
                module_type=module_info["module_type"],
                instance_name=instance_name,
                method_name=method["name"],
                input_plan=input_plan,
                compare_plan=compare_plan,
            )
            ref_path = os.path.join(step_dir, f"{module_name}_ref.h")
            harness.write_text(ref_path, ref_header)
            harness.write_text(os.path.join(step_dir, f"{module_name}.sv"), sv_code)

            # verify 的 shard 数是动态分配的。
            # 这里不要把 requested_parallel_jobs 理解成“固定值”，它只是单 method 想要的上限。
            if static_fail:
                passed = False
                message = "STATIC_GUARD_FAIL:\n" + static_fail
                verify_elapsed = 0.0
                granted_parallel_jobs = 0
            else:
                granted_parallel_jobs = shard_budget.acquire(requested_parallel_jobs)
                try:
                    verify_start = time.perf_counter()
                    verify_exhaustive_threshold = int(cfg["verify"]["exhaustive_threshold"])
                    if method["name"] == "init":
                        # Keep init on randomized/fixed-count vectors instead of 2^pi_width exhaustive mode.
                        # init often has tiny pi_width in snippet harness, which otherwise collapses to 2 vectors.
                        verify_exhaustive_threshold = 0
                    passed, message = verifier.verify(
                        ref_path,
                        sv_code,
                        module_name,
                        input_plan["pi_width"],
                        compare_plan["po_width"],
                        verify_exhaustive_threshold,
                        snippet_max_test_vectors,
                        output_signal_map=compare_plan["signal_map"],
                        verilator_bin=cfg["verilator"],
                        yosys_bin=cfg.get("yosys"),
                        parallel_jobs=granted_parallel_jobs,
                    )
                    verify_elapsed = time.perf_counter() - verify_start
                finally:
                    shard_budget.release(granted_parallel_jobs)

            full_compile_elapsed = 0.0
            if passed:
                # 单方法对拍通过后，再做一次“只填当前 method、其他 method 置空”的 full-shell compile gate。
                # 这一步的作用是尽早暴露 combine 壳下才会出现的编译问题，
                # 但又不依赖别的 method 的逻辑是否正确。
                compile_start = time.perf_counter()
                full_ok, full_msg = _run_full_shell_compile_gate(
                    cfg=cfg,
                    task_dir=step_dir,
                    module_info=module_info,
                    combine_info=combine_info,
                    method_name=method["name"],
                    snippet_code=snippet_code,
                )
                full_compile_elapsed = time.perf_counter() - compile_start
                if not full_ok:
                    passed = False
                    message = (
                        "FULL_SHELL_COMPILE_ERROR:\n"
                        "This error comes from the exact full-module combine shell with only the current method body filled.\n"
                        "Other methods are emitted as empty blocks, so this failure should be fixed inside this method or in framework generation.\n\n"
                        + full_msg
                    )

            harness.write_text(os.path.join(step_dir, "verify_message.txt"), (message or "") + "\n")
            logger.info(
                f"Snippet {module_info['module_type']}::{method['name']} step {step_idx}: "
                f"{'PASS' if passed else 'FAIL'} "
                f"({verify_elapsed:.1f}s verify, {granted_parallel_jobs} shards"
                f"{', ' + format(full_compile_elapsed, '.1f') + 's full-shell' if full_compile_elapsed else ''})"
            )
            for line in harness.summarize_verify(message):
                logger.info(f"  {line}")
            if passed:
                return {"passed": True, "step": step_idx, "snippet_path": snippet_path, "task_dir": task_dir}
            if step_idx >= max_iterations:
                break

            # debug prompt 始终基于“当前 snippet + 当前错误信息”重建，
            # 不沿用上一轮 prompt，以免旧错误上下文污染新一轮修复。
            prompt = harness.build_debug_prompt(
                module_info=module_info,
                method=method,
                method_ctx=method_ctx,
                input_plan=input_plan,
                compare_plan=compare_plan,
                current_snippet=snippet_code,
                verify_message=message or "",
            )
            harness.write_text(os.path.join(step_dir, "prompt.txt"), prompt)
            llm_start = time.perf_counter()
            response = call_llm.ask_llm_with_retry(
                provider,
                client,
                cfg["llm"]["model"],
                [{"role": "user", "content": prompt}],
                logger=logger,
                task=f"{module_info['module_type']}_{method['name']}_debug_step_{step_idx}",
            )
            llm_elapsed = time.perf_counter() - llm_start
            harness.write_text(os.path.join(step_dir, "response.txt"), response or "")
            logger.info(
                f"Snippet {module_info['module_type']}::{method['name']} step {step_idx}: "
                f"debug LLM call finished in {llm_elapsed:.1f}s"
            )
            answer = extract_model_payload(response or "", debug_use_think)
            patched = extract_verilog(answer)
            patched = harness.sanitize_snippet_code(patched)
            if not patched.strip():
                return {"passed": False, "step": step_idx, "stop_reason": "empty_patch", "task_dir": task_dir}
            if patched.strip() == snippet_code.strip():
                return {"passed": False, "step": step_idx, "stop_reason": "no_change", "task_dir": task_dir}
            harness.write_text(snippet_path, patched.rstrip() + "\n")
            harness.write_text(os.path.join(step_dir, "snippet_after.sv"), patched.rstrip() + "\n")

        return {"passed": False, "step": max_iterations, "stop_reason": "max_iterations", "task_dir": task_dir}
    finally:
        shard_budget.finish_method()


def _snippet_static_guard(module_type, method_name, snippet_code):
    """Fast reject for clearly invalid snippet patterns before heavy verify."""
    if module_type != "Isu" or method_name != "comb_issue":
        return None

    src = snippet_code or ""
    findings = []

    assign = r"(?:<=|(?<![=!<>])=(?!=))"
    forbidden_lhs = [
        r"configs\s*\[[^\]]+\]\s*\.[A-Za-z_]\w*",
        r"iqs\s*\[[^\]]+\]\s*\.size",
        r"iqs\s*\[[^\]]+\]\s*\.dispatch_width",
        r"iqs\s*\[[^\]]+\]\s*\.ports\s*\[[^\]]+\]\s*\.port_idx",
        r"iqs\s*\[[^\]]+\]\s*\.entry\s*\[[^\]]+\]\s*\.valid",
        r"iqs\s*\[[^\]]+\]\s*\.entry\s*\[[^\]]+\]\s*\.uop\.",
    ]
    for lhs in forbidden_lhs:
        if re.search(rf"{lhs}\s*{assign}", src):
            findings.append(f"forbidden write matched: `{lhs}`")

    # In this framework comb_issue should not reference global init tables directly.
    if "GLOBAL_IQ_CONFIG" in src or "GLOBAL_ISSUE_PORT_CONFIG" in src:
        findings.append("comb_issue must not reference GLOBAL_* config tables")

    # Reject the known bad pattern: inferring active port count by non-zero capability masks.
    if re.search(r"\bcapability_mask\s*!=\s*(?:64'd0|0)\b", src):
        findings.append("do not infer active port count by scanning `capability_mask != 0`")
    if re.search(r"\bcfg_cap\s*!=\s*(?:64'd0|0)\b", src):
        findings.append("do not infer active port count by scanning temporary capability masks")
    if re.search(r"\bif\s*\([^)]*cap(?:ability)?_mask[^)]*!=\s*(?:64'd0|0)[^)]*\)\s*num_ports\w*\s*=", src):
        findings.append("do not increment `num_ports` from capability-mask non-zero checks")
    if re.search(r"\bif\s*\([^)]*cfg_cap[^)]*!=\s*(?:64'd0|0)[^)]*\)\s*num_ports\w*\s*=", src):
        findings.append("do not increment `num_ports` from cfg_cap non-zero checks")
    if re.search(r"\bnum_ports\w*\s*=\s*[^;\n]*dispatch_width\b", src):
        findings.append("do not derive `num_ports` from `dispatch_width` in comb_issue schedule domain")

    if not findings:
        return None
    return (
        "The snippet is changing read-only metadata/state source for `Isu::comb_issue`.\n"
        "Fix by keeping schedule source on `q.entry`, commit side effects on `entry_1/count_1/wake_matrix`, "
        "and avoid writing `configs`/queue metadata.\n"
        + "\n".join(f"- {x}" for x in findings)
    )


def _build_method_context(module_info, combine_info, helper_db, all_constants):
    contexts = {}
    for method in module_info["methods"]:
        # 这里构造的是“method 级 prompt / debug 上下文”。
        # 它与 combine_info 不同：combine_info 偏向完整模块装配，
        # method_ctx 则偏向单个 method 的最小可翻译环境。
        method_helpers = bsd_analyzer.extract_method_helpers(method["body"], helper_db)
        logic_text = method["body"] + "\n" + "\n".join(method_helpers.values())
        expand_depth = 3 if module_info["module_type"] == "Isu" else 1
        used_consts = prompt_builder.select_prompt_constants(all_constants, logic_text)
        project_context = bsd_analyzer.project_context_for_logic(logic_text)
        order = bsd_analyzer.get_struct_order_for_method(
            module_info["structs"],
            module_info["module_type"],
            method_body=logic_text,
            expand_depth=expand_depth,
        )
        contexts[method["name"]] = {
            "helpers": method_helpers,
            "constants": used_consts,
            "cpp_type_sources": bsd_analyzer.generate_cpp_type_sources(module_info["struct_sources"], order),
            "sv_typedefs": bsd_analyzer.generate_sv_typedefs(
                module_info["structs"],
                module_info["type_widths"],
                module_type=module_info["module_type"],
                method_body=logic_text,
                expand_depth=expand_depth,
            ),
            "project_context": project_context,
            "var_decls": combine_info["var_decls"],
        }
    return contexts


def _target_matches_module(target, module_info):
    if target in (module_info["module_type"], module_info["module_type"].lower()):
        return True
    return any(target == f"{module_info['module_type']}_{method['name']}" for method in module_info["methods"])


def _run_full_shell_compile_gate(*, cfg, task_dir, module_info, combine_info, method_name, snippet_code):
    default_strategy = cfg.get("combine", {}).get("default_init", "zero_all")
    snippets = {}
    for other_method in combine_info["method_order"]:
        task_key = f"{module_info['module_type']}_{other_method}"
        snippets[task_key] = snippet_code if other_method == method_name else ""

    # 这个 gate 不是 full verify，它只回答一个问题：
    # “当前 method 放回真实 combine 壳后，是否至少能通过前端编译检查？”
    # 其他 method 全部置空，避免把跨 method 的波动混进当前调试回路。
    failures = []
    for bf in combine_info["bsd_files"]:
        sv_code = combine_helpers.build_combined_module_sv(
            bf,
            combine_info,
            snippets,
            default_strategy=default_strategy,
        )
        harness.write_text(os.path.join(task_dir, f"{bf['module_name']}_fullshell.sv"), sv_code)
        ok, msg = verifier.compile_only(
            sv_code,
            bf["module_name"],
            verilator_bin=cfg["verilator"],
            yosys_bin=cfg.get("yosys"),
            artifact_dir=os.path.join(task_dir, f"{bf['module_name']}_fullshell_compile"),
        )
        if not ok:
            failures.append(f"[{bf['module_name']}]\n{msg}")

    return (len(failures) == 0), "\n\n".join(failures)
