# sim2v 说明

这份文档只描述当前精简后的版本，也就是仓库里真正保留下来的主流程和代码路径。

## 1. 现在项目只做什么

当前项目只做一件事：

- 对 `io_generator/*_bsd` 模块
- 按 method 拆分 C++ simulator 逻辑
- 用 LLM 生成 method body 的 SystemVerilog snippet
- 先做 snippet 级验证，再组装完整模块并做 full verify

主流程是：

```text
prompt -> infer -> snippet -> combine -> verify
```


## 2. 目录和入口

当前主干代码只有这些模块：

1. [run.py](/workspace/S/lvhanqi/sim2v/run.py)
   - CLI 调度
2. [prompt_builder.py](/workspace/S/lvhanqi/sim2v/prompt_builder.py)
   - prompt 构造与 combine 元信息
3. [snippet_stage.py](/workspace/S/lvhanqi/sim2v/snippet_stage.py)
   - snippet 阶段任务调度与每轮修复
4. [snippet_harness.py](/workspace/S/lvhanqi/sim2v/snippet_harness.py)
   - snippet target 规划、wrapper/reference 生成、debug prompt
5. [verify.py](/workspace/S/lvhanqi/sim2v/verify.py)
   - yosys 语法审查与 Verilator testbench 生成/执行
6. [bsd_analyzer.py](/workspace/S/lvhanqi/sim2v/bsd_analyzer.py)
   - 模块分析入口与 method 提取
7. [bsd_types.py](/workspace/S/lvhanqi/sim2v/bsd_types.py)
   - 结构体解析、宽度解析、SV typedef/变量声明生成
8. [io_mapping.py](/workspace/S/lvhanqi/sim2v/io_mapping.py)
   - `pi/po` 映射解析
9. [signal_debug.py](/workspace/S/lvhanqi/sim2v/signal_debug.py)
   - method 读写集提取与路径规范化
10. [call_llm.py](/workspace/S/lvhanqi/sim2v/call_llm.py)
   - LLM 调用与响应落盘

## 3. active methods 是怎么决定的

当前不会再简单地把源码里的所有 `init` / `comb_*` 都拿来翻译。

规则是：
- 优先解析 `io_generator_outer()` 中真实调用的方法顺序
- 只有 wrapper 里真实调用的方法才会进入：
  - prompt
  - snippet
  - combine
- 如果拿不到 wrapper 调用顺序，才退回源码里所有 `init` / `comb_*`

对应代码：
- [prompt_builder.py](/workspace/S/lvhanqi/sim2v/prompt_builder.py)
- `resolve_active_method_order()`
- `_parse_comb_call_order_from_outer()`

这样可以避免像早期那样把 `ROB.init()` 错拼进 full module。

## 4. 解析信息分别从哪里来

### 4.1 模块、方法、结构体

入口：
- [bsd_analyzer.py](/workspace/S/lvhanqi/sim2v/bsd_analyzer.py)
- `analyze_module()`
- [bsd_types.py](/workspace/S/lvhanqi/sim2v/bsd_types.py)

来源方式：

1. 模块类型
   - 直接读 `*_bsd` wrapper 头文件
   - 用正则匹配 `#include <ROB.h>` / `#include <PRF.h>`

2. method body
   - 直接读 `io_generator/simulator_include/<MODULE>_cpp.h`
   - 用正则匹配 `void MODULE::method()`
   - 用花括号配对提取函数体

3. 结构体/类定义
   - 遍历 `simulator_include/` 里的头文件
   - 用手写文本解析提取 typedef / struct / class 字段

结论：
- 这一层不是通用 C++ AST
- 主要是“文件读取 + 正则 + brace matching + 手写结构解析”

### 4.2 `pi/po` 映射

入口：
- [io_mapping.py](/workspace/S/lvhanqi/sim2v/io_mapping.py)

当前默认 provider：
- `pi_to_simulator_v1`

输入映射：
- `parse_pi_to_simulator()`
- 解析 `MODULE::pi_to_simulator(bool* pi)`
- 通过正则识别：
  - `pack_bits<...>(cursor, width)`
  - 注释中的 offset
  - 简单 `for (int i = 0; i < N; i++)` 循环

输出映射：
- `collect_outputs()` 优先读 `*_bsd.h` 里的 `unpack_bits(...)`
- 如果 wrapper 里没有，再退回 `parse_simulator_to_po()`

结论：
- `pi/po` 映射是文本模式解析，不是 AST

### 4.3 snippet 的 read-set / write-set

入口：
- [snippet_stage.py](/workspace/S/lvhanqi/sim2v/snippet_stage.py)
- [snippet_harness.py](/workspace/S/lvhanqi/sim2v/snippet_harness.py)
- [signal_debug.py](/workspace/S/lvhanqi/sim2v/signal_debug.py)

来源方式：

1. 主通道：libclang AST
   - `signal_debug._extract_rw_libclang_file()`
   - 提取 method 的 reads / writes

2. 回退补齐：文本路径解析
   - `snippet_stage._fallback_write_targets()`
   - `snippet_stage._fallback_read_targets()`

3. 精细化：
   - `_extract_precise_signal_paths()`
   - `_refine_targets_with_exact_paths()`
   - `_supplement_scalar_root_reads()`

结论：
- 这部分是“AST 为主，文本回退为辅”

## 5. infer prompt 怎么构造

入口：
- [prompt_builder.py](/workspace/S/lvhanqi/sim2v/prompt_builder.py)
- `build_prompts()`
- `build_subtask_prompt()`
- `render_method_prompt()`

模板：
- `METHOD_PROMPT_TEMPLATE`

每个 infer prompt 的核心部分：

1. 任务边界
   - 只翻译 `MODULE::method`
   - 不要碰其他 method
   - 不要输出 framework 代码

2. framework guarantee
   - wrapper 已经声明好输入、输出、内部变量
   - wrapper 已经负责默认初始化、输入解包、方法顺序和输出打包
   - LLM 输出会被插进 `begin : method_name ... end`

3. 现有变量环境
   - Inputs
   - Internals
   - Outputs

4. naming rules
   - `in.X->Y -> in_X.Y`
   - `out.X->Y -> out_X.Y`
   - `type -> type_v`

5. constants / width hints
6. C++ type context + SV typedef context
7. source of truth
   - 原始 C++ method body
   - helper functions
8. translation rules
   - 不要发明 `x_1 = x`
   - `x_1--` 必须更新 `x_1`
   - 不要输出 `module` / `always_comb` / `typedef` / `function`

### 一个简化版 prompt 结构

```text
## TASK
Translate only ROB::comb_fire.
Wrapper already declares all signals.
Your code will be inserted inside begin : comb_fire ... end.

## SV ENVIRONMENT
Inputs:
  ...
Internals:
  ...
Outputs:
  ...

## CONTEXT
Naming rules
Constants
Type definitions

## SOURCE OF TRUTH (C++)
void ROB::comb_fire() {
    ...
}

## RULES
- statements only
- no module / always_comb / typedef
- do not invent x_1 = x
- keep widths clean
```

## 6. snippet 阶段具体做什么

入口：
- [snippet_stage.py](/workspace/S/lvhanqi/sim2v/snippet_stage.py)
- `run()`
- `_method_io_targets()`
- `_build_signal_plan()`
- `_build_sv_wrapper()`
- `_build_cpp_reference()`

### 6.1 核心思想

snippet 阶段验证的是：
- 单个 method snippet
- 在它自己的显式输入/输出边界下
- 是否与对应 C++ method 等价

而不是直接拿完整模块一起 debug。

### 6.2 signal-agnostic harness

当前不默认假设：
- `x_1` 是 `x` 的 next-state
- `x_1` 应该自动复制 `x`

当前规则是：
- read-set -> 自由输入
- write-set -> 比较目标
- 在需要时，compare targets 也可以进入输入

这样能支持像 `count_1--` 这种“对目标自身做增量更新”的方法语义。

### 6.3 单个 snippet 的生成物

对每个 method，会生成：

1. `input_plan.json`
2. `compare_plan.json`
3. 单方法 SV wrapper
4. 单方法 C++ reference
5. `verify_message.txt`
6. 如果失败，还会生成：
   - `prompt.txt`
   - `response.txt`
   - `snippet_after.sv`

对应目录在：

```text
output/output_xxx/snippet_debug/snippet_xxx/<TASK>/step_000/
```

## 7. snippet debug prompt 怎么构造

模板在：
- [snippet_stage.py](/workspace/S/lvhanqi/sim2v/snippet_stage.py)
- `SNIPPET_DEBUG_PROMPT`

输入包括：
- 当前 method 的 C++ source of truth
- helper functions
- 当前 snippet
- verifier 报错
- 已声明变量
- constants
- type context
- compare targets
- signal-agnostic harness inputs

核心约束：
- 只修当前 method
- 只输出一个 `systemverilog` 代码块
- 先修 compile/lint error
- 不要重写 framework-owned 默认初始化和 pi/po packing
- 不要发明 `*_1 = *`
- 局部变量允许声明，但必须先无条件初始化

## 8. combine 怎么做

入口：
- [run.py](/workspace/S/lvhanqi/sim2v/run.py)
- `cmd_combine()`
- [prompt_builder.py](/workspace/S/lvhanqi/sim2v/prompt_builder.py)
- `get_combine_info()`

combine 只负责结构装配：
- SV typedefs
- safe constants
- variable declarations
- framework defaults
- `pi[]` 解包
- method snippets
- `po[]` 打包


## 9. verify 和 snippet verify 的关系

入口：
- [verify.py](/workspace/S/lvhanqi/sim2v/verify.py)

两层验证共用同一个 Verilator testbench 框架，区别只在比较对象：

1. snippet verify
   - reference: 单方法 C++ harness
   - dut: 单方法 SV wrapper

2. full verify
   - reference: 原始 `io_generator_outer()`
   - dut: combine 后完整模块

### testbench 逻辑

统一都是：

```text
生成输入向量
-> 调 reference 得到 po_ref
-> 驱动 DUT
-> dut.eval()
-> 比较 DUT 输出和 po_ref
```

### 当前性能优化

`verify.py` 目前已经做了：
- shard 并行
- 输入侧 word-level 快路径
- 输出侧 word-level 比较快路径

配置项：
- `verify.parallel_jobs`
- `snippet.parallel_jobs`

## 10. 当前项目最重要的约束

1. LLM 只负责 method body，不负责 framework 壳子
2. active methods 服从真实 wrapper call order
3. snippet harness 不把命名习惯误当语义
4. `x++ / x-- / x += y / x -= y` 必须更新 `x` 自身
5. 先在 snippet 层收敛，再看 full verify

## 11. 建议从哪里开始读代码

如果你要重新熟悉整个项目，按这个顺序最省时间：

1. [run.py](/workspace/S/lvhanqi/sim2v/run.py)
2. [prompt_builder.py](/workspace/S/lvhanqi/sim2v/prompt_builder.py)
3. [bsd_analyzer.py](/workspace/S/lvhanqi/sim2v/bsd_analyzer.py)
4. [io_mapping.py](/workspace/S/lvhanqi/sim2v/io_mapping.py)
5. [snippet_stage.py](/workspace/S/lvhanqi/sim2v/snippet_stage.py)
6. [verify.py](/workspace/S/lvhanqi/sim2v/verify.py)
7. [signal_debug.py](/workspace/S/lvhanqi/sim2v/signal_debug.py)
