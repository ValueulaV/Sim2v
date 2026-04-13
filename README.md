# sim2v

`sim2v` 把 `*_bsd` 模块对应的 C++ cycle-level simulator 逻辑翻译成 SystemVerilog，并用 Verilator 做等价验证。

交接与新人上手请优先看：
- [HANDOFF_CN.md](/nfs_global/S/lvhanqi/project/sim2v/HANDOFF_CN.md)

这个仓库已经收敛为一条主流程，只保留当前稳定使用的路径：

```text
prompt -> infer -> snippet -> combine -> verify
```


## 推荐命令

```bash
PYTHON=/workspace/S/lvhanqi/miniconda3/envs/mage/bin/python

$PYTHON run.py prompt
$PYTHON run.py infer
$PYTHON run.py snippet
$PYTHON run.py combine
$PYTHON run.py verify
```

或直接：

```bash
$PYTHON run.py pipeline
```

## 当前命令

- `prompt`
  - 解析 `*_bsd` 模块，按 method 生成 prompt JSONL
- `infer`
  - 调用 LLM，为每个 method 生成 snippet
- `snippet`
  - 对每个 method snippet 单独构造 harness 并验证；失败时就在这一层修复
- `combine`
  - 按 `io_generator_outer()` 的真实调用顺序组装完整 SV 模块
- `verify`
  - 先做 yosys 语法审查，再做 C++ vs Verilog 对拍
- `pipeline`
  - 顺序执行 `prompt -> infer -> snippet -> combine -> verify`

## 当前核心设计

- LLM 只翻译 method body，不负责 `module` 壳子、声明、`pi/po` 打包。
- active methods 以 `io_generator_outer()` 里的真实调用顺序为准。
- snippet 阶段使用 signal-agnostic harness：
  - read-set 作为自由输入
  - write-set 作为比较目标
- snippet 和 verify 都支持向量分片并行：
  - `snippet.parallel_jobs`
  - `verify.parallel_jobs`
- `verify()` 统一先跑 yosys 语法审查，再跑 Verilator 等价验证。

## 主要文件

- [run.py](/workspace/S/lvhanqi/sim2v/run.py): 主入口
- [prompt_builder.py](/workspace/S/lvhanqi/sim2v/prompt_builder.py): prompt 构造与 combine 元信息
- [snippet_stage.py](/workspace/S/lvhanqi/sim2v/snippet_stage.py): snippet 阶段任务调度与每轮修复
- [snippet_harness.py](/workspace/S/lvhanqi/sim2v/snippet_harness.py): snippet target 规划、wrapper/reference 生成、debug prompt
- [verify.py](/workspace/S/lvhanqi/sim2v/verify.py): Verilator testbench 生成与执行
- [bsd_analyzer.py](/workspace/S/lvhanqi/sim2v/bsd_analyzer.py): 模块分析入口与 method 提取
- [bsd_types.py](/workspace/S/lvhanqi/sim2v/bsd_types.py): 结构体解析、宽度解析、SV typedef/变量声明生成
- [io_mapping.py](/workspace/S/lvhanqi/sim2v/io_mapping.py): `pi/po` 映射解析
- [signal_debug.py](/workspace/S/lvhanqi/sim2v/signal_debug.py): libclang 读写集提取与路径规范化

## 输出目录

每次运行会落在：

```text
output/output_YYYYMMDD_HHMMSS/
```

常见内容：
- `prompts/`
- `responses/`
- `snippets/`
- `snippet_debug/`
- `verilog_llm/`
- `verify_results.json`

## 进一步说明

更详细的流程、prompt 内容来源、snippet harness、testbench 逻辑，见：
- [explain.md](/workspace/S/lvhanqi/sim2v/explain.md)
