"""Call LLM API on JSONL prompt file, write responses.

Features:
- Concurrent API calls with ThreadPoolExecutor
- Resume: checks output file for completed task keys, skips them
- max_tasks: limit number of tasks to process (0 = no limit)
- Graceful interrupt: output is append-mode, safe to Ctrl+C and resume
"""

import json
import os
import threading
import time
import hashlib
import random
from concurrent.futures import ThreadPoolExecutor, as_completed

from openai import OpenAI
from anthropic import Anthropic
from anthropic import RateLimitError, APIConnectionError, APITimeoutError

def _prompt_hash(messages):
    """Stable hash for a prompt message list."""
    packed = json.dumps(messages, ensure_ascii=False, sort_keys=True)
    return hashlib.sha256(packed.encode("utf-8")).hexdigest()[:16]


def get_client(model_name):
    """Create LLM client based on model name."""
    if "gemini" in model_name:
        from google import genai
        return "google", genai.Client(api_key=os.environ["GOOGLE_API_KEY"])
    elif "claude" in model_name.lower():
        return "anthropic", Anthropic(
            api_key=os.environ.get("AWS_API_KEY"),
            base_url="https://ace2.ezclaude.com",
            timeout=900,
        )
    return "openai", OpenAI(
        api_key=os.environ["ARK_API_KEY"],
        base_url="https://ark.cn-beijing.volces.com/api/v3",
        timeout=900,
    )


def ask_llm(provider, client, model_name, messages):
    """Send messages to LLM, return response text."""
    if provider == "google":
        prompt = "\n\n".join(m["content"] for m in messages)
        resp = client.models.generate_content(model=model_name, contents=prompt)
        return resp.text
    if provider == "anthropic":
        resp = client.messages.create(model=model_name, messages=messages, max_tokens=128000)
        return resp.content[0].text

    resp = client.chat.completions.create(model=model_name, messages=messages, max_tokens=65536)
    return resp.choices[0].message.content


def ask_llm_with_retry(provider, client, model_name, messages, logger, task,
                       max_attempts=8, retry_interval_s=10.0):
    """Provider call with bounded retry for transient/rate-limit failures."""
    attempt = 0
    while True:
        attempt += 1
        try:
            return ask_llm(provider, client, model_name, messages)
        except (RateLimitError, APIConnectionError, APITimeoutError) as e:
            if attempt >= max_attempts:
                logger.error(f"Task {task}: retry exhausted after {attempt} attempts ({type(e).__name__})")
                raise
            # Keep retry cadence short and stable to avoid long stalls.
            # Add slight jitter so concurrent retries do not synchronize.
            sleep_s = max(1.0, float(retry_interval_s)) * random.uniform(0.9, 1.1)
            logger.warning(
                f"Task {task}: transient {type(e).__name__}, retry {attempt}/{max_attempts} after {sleep_s:.1f}s"
            )
            time.sleep(sleep_s)


def _process_entry(entry, provider, client, model_name, write_lock, out_file, logger,
                   record_dir=None):
    """Process one JSONL entry: call LLM and append result."""
    task = entry["task"]
    messages = entry["messages"]
    phash = entry.get("prompt_hash", _prompt_hash(messages))
    sample_idx = entry.get("_sample_idx")
    run_id = entry.get("_run_id")
    logger.info(f"Calling LLM for: {task}")
    logger.debug(f"Prompt hash: {phash}")

    # 这里的重试只处理“空响应”这种轻故障，不负责做复杂退避或 provider 级容错。
    response = ask_llm_with_retry(provider, client, model_name, messages, logger, task)
    retries = 0
    while not response or not response.strip():
        retries += 1
        if retries > 3:
            logger.error(f"Task {task}: failed after 3 retries")
            return
        time.sleep(2)
        logger.warning(f"Task {task}: empty response, retry {retries}")
        response = ask_llm_with_retry(provider, client, model_name, messages, logger, task)

    logger.info(f"Task {task}: got {len(response)} chars")
    logger.debug(f"Task {task}: response recorded={bool(record_dir)}")

    result = {
        "task": task,
        "prompt_hash": phash,
        "response": response,
    }
    if run_id:
        result["run_id"] = run_id
    if sample_idx is not None:
        result["sample_idx"] = int(sample_idx)
    for key in ("pi_width", "po_width", "slots", "method", "module_type"):
        if key in entry:
            result[key] = entry[key]

    if record_dir:
        task_dir = os.path.join(record_dir, task, phash)
        os.makedirs(task_dir, exist_ok=True)
        prompt_path = os.path.join(task_dir, "prompt.txt")
        if not os.path.exists(prompt_path):
            with open(prompt_path, "w") as pf:
                pf.write(messages[-1]["content"])
        suffix = f"{int(sample_idx):02d}" if sample_idx is not None else f"{int(time.time_ns())}"
        resp_path = os.path.join(task_dir, f"response_{suffix}.txt")
        with open(resp_path, "w") as rf:
            rf.write(response)

    with write_lock:
        # 输出文件用 append 模式逐条落盘，这样 Ctrl+C 后可以直接 resume，
        # 不需要把整轮结果先攒在内存里。
        out_file.write(json.dumps(result, ensure_ascii=False) + "\n")
        out_file.flush()


def _load_completed(output_path):
    """Load completed task keys from existing output file."""
    completed = {}
    if not os.path.exists(output_path):
        return completed
    with open(output_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            data = json.loads(line)
            tid = data["task"]
            phash = data["prompt_hash"] if "prompt_hash" in data else ""
            key = f"{tid}#{phash}"
            completed[key] = completed.get(key, 0) + 1
    return completed


def run(input_path, output_path, model_name, max_workers, sample_num, logger,
        max_tasks=0, run_id=None, record_dir=None):
    """
    Process entries in input JSONL with concurrent LLM calls.
    Args:
        max_tasks: limit total tasks to process (0 = no limit).
                   Useful for testing with a small subset.
    """
    provider, client = get_client(model_name)

    # infer 的恢复语义基于 (task, prompt_hash)。
    # 只要 prompt 文本没变，就不会重复调用相同任务。
    completed = _load_completed(output_path)
    logger.info(f"Already completed: {sum(completed.values())} responses "
                f"for {len(completed)} unique tasks")

    write_lock = threading.Lock()
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

    with open(output_path, "a") as out_file:
        with ThreadPoolExecutor(max_workers=max_workers) as pool:
            futures = []
            task_count = 0
            with open(input_path) as f:
                for line in f:
                    entry = json.loads(line)
                    task = entry["task"]
                    phash = _prompt_hash(entry["messages"])
                    entry["prompt_hash"] = phash
                    done = completed.get(f"{task}#{phash}", 0)
                    needed = max(0, sample_num - done)
                    if needed == 0:
                        continue
                    for idx in range(needed):
                        if max_tasks > 0 and task_count >= max_tasks:
                            break
                        run_entry = dict(entry)
                        run_entry["_sample_idx"] = done + idx
                        if run_id:
                            run_entry["_run_id"] = run_id
                        fut = pool.submit(
                            _process_entry, run_entry, provider, client,
                            model_name, write_lock, out_file, logger,
                            record_dir=record_dir,
                        )
                        futures.append(fut)
                        task_count += 1
                    if max_tasks > 0 and task_count >= max_tasks:
                        break

            logger.info(f"Submitted {task_count} tasks to LLM")
            done_count = 0
            for fut in as_completed(futures):
                fut.result()
                done_count += 1
                if done_count % 10 == 0:
                    logger.info(f"Progress: {done_count}/{task_count}")

    logger.info(f"LLM inference complete: {task_count} tasks processed")
