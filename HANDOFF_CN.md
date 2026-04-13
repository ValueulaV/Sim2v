# sim2v 中文交接文档

## 1. 先说结论

这份仓库当前应当按 **PRF / ROB 基线可用版** 来理解。

对新人最重要的判断是：

- 这不是“任意 C++ 到任意 SV”的通用编译器。
- 这是一套面向 `io_generator/*_bsd` 生态、围绕 `simulator_include/` 的 **工程化翻译框架**。
- 当前版本的稳定目标是：
  - `prompt -> infer -> snippet -> combine -> verify`
  - 在 `PRF` / `ROB` 上能稳定工作
- 其他更复杂 case 的实验性扩展不要作为当前主干理解前提。

如果要接手维护，请先默认：

1. **PRF / ROB 是基线**
2. **框架核心策略优先于当前实现细节**
3. **不要轻易把“当前写法”误当成“唯一正确方案”**


## 2. 项目想解决什么问题

项目目标是把 `*_bsd` 对应的 C++ cycle-level simulator 逻辑翻成 SystemVerilog，并用 C++ 参考模型做验证。

但这里有一个非常重要的边界：

- LLM **只负责翻 method body**
- 框架负责：
  - 类型定义
  - 变量声明
  - `pi/po` 打包解包
  - 默认初始化
  - method 调用顺序
  - snippet harness
  - 最终验证

也就是说，这套系统本质上是：

- **框架决定边界**
- **LLM 只填边界中的一小块逻辑**

这条原则是整个项目最重要的“策略”。


## 3. 主流程

当前主流程固定为：

```text
prompt -> infer -> snippet -> combine -> verify
```

各阶段职责如下。

### 3.1 `prompt`

输入：

- `config.yaml` 中的 `input_dirs`
- `io_generator/<module>_bsd`
- `io_generator/simulator_include/*.h`

输出：

- `output/.../prompts/prompts.jsonl`

作用：

- 解析模块类型
- 提取 method body
- 构造每个 method 的独立 prompt
- 将任务写成 JSONL

这一阶段**不调用 LLM**。

### 3.2 `infer`

输入：

- `prompts.jsonl`

输出：

- `responses/responses.jsonl`
- `snippets/<MODULE>_<method>.sv`

作用：

- 并发调用 LLM
- 从回答中抽出 Verilog code block
- 落成单个 method 的 snippet 文件

### 3.3 `snippet`

输入：

- `snippets/*.sv`
- 模块静态信息

输出：

- `snippet_debug/...`
- 每个 method 的 `input_plan.json` / `compare_plan.json`
- 单方法 SV wrapper
- 单方法 C++ reference
- 失败时的 debug prompt 和修复轨迹

作用：

- 对每个 method 单独验证
- 失败时进行 method 级 debug 修复

这是项目里最关键的一层，因为它把“整模块问题”拆成了“单方法问题”。

### 3.4 `combine`

输入：

- 通过 snippet 的 method snippets

输出：

- `verilog_llm/<module>.sv`

作用：

- 按 `io_generator_outer()` 的真实调用顺序
- 把 method snippets 拼回完整模块

### 3.5 `verify`

输入：

- `combine` 生成的完整模块
- 对应 C++ 参考壳

输出：

- `verify_results.json`
- 编译与运行日志

作用：

1. 可选 yosys 前端检查
2. Verilator 编译
3. 运行随机/穷举向量，对比 DUT 与 C++ reference


## 4. 核心策略 vs 当前实现方式

这部分最重要。接手时要优先区分：

- 什么是**策略**
- 什么只是**当前实现方式**

### 4.1 不建议轻易改的“策略”

这些是项目现在真正成立的基础。

#### 策略 A：只翻 method body，不翻 framework

含义：

- LLM 不负责输出 `module`
- 不负责输出 `always_comb`
- 不负责输出 `typedef`
- 不负责 `pi/po` packing/unpacking
- 不负责默认初始化

原因：

- method body 比完整模块更容易约束
- 失败时可以局部 debug
- 能把框架稳定性和模型能力拆开看

#### 策略 B：以 `io_generator_outer()` 的真实调用顺序为准

含义：

- active methods 不是“源码里所有 `init/comb_*`”
- 而是 wrapper 里真实会调用的 method

原因：

- prompt / snippet / combine / verify 必须看到同一组 method
- 否则很容易出现“snippet 过了但 combine 顺序不一致”的问题

#### 策略 C：snippet 先于 combine

含义：

- 先把单个 method 验证好
- 再拼回完整模块

原因：

- 整模块错误太大，LLM 不容易修
- method 级边界更适合收敛

#### 策略 D：snippet 的边界由 read-set / write-set 决定

含义：

- read-set 作为自由输入
- write-set 作为比较目标

原因：

- 避免把整个模块接口都塞进 snippet harness
- 也避免假设 `x_1 = x` 之类不存在的默认语义

#### 策略 E：C++ reference 与 SV wrapper 必须共用同一份 signal plan

含义：

- 输入如何展开
- 输出如何打包
- 哪些信号参与比较

都必须由同一份 `input_plan / compare_plan` 驱动。

原因：

- 如果两边对边界理解不一致，snippet 验证没有意义


### 4.2 可以重写的“实现方式”

这些不是必须坚持的设计，只是当前版本的落地手段。

#### 实现方式 1：大量使用正则和文本扫描

例如：

- `bsd_analyzer.py` 提取 method
- `io_mapping.py` 解析 `pi_to_simulator()` / `simulator_to_po()`
- `bsd_types.py` 解析 struct/class
- `snippet_harness.py` 提取精确路径

这些都可以换成：

- clang AST
- tree-sitter
- 更稳定的 parser

只要不破坏前面的策略即可。

#### 实现方式 2：当前只有一个 io_mapping provider

虽然有 `MappingProvider` 抽象层，但目前只有：

- `pi_to_simulator_v1`

这层是为了将来替换实现准备的，不代表当前 provider 本身特别优雅。

#### 实现方式 3：当前 snippet 调度使用“两级并发 + shard 预算器”

即：

- method 级线程池
- verify 级 shard 并发
- `_VerifyShardBudget` 做全局预算分配

这是为了平衡吞吐和资源占用的当前解法，不是唯一做法。

#### 实现方式 4：当前 prompt 主要靠手工规则堆约束

这是经验化实现，不是理论最优方案。

如果以后：

- 类型上下文更准确
- wrapper 更强
- 自动修复策略更成熟

prompt 可以简化。


## 5. 代码架构与文件职责

下面按文件解释职责，并标出“建议怎么理解它”。

### 5.1 [run.py](/nfs_global/S/lvhanqi/project/sim2v/run.py)

角色：

- CLI 主入口
- 负责阶段编排
- 管理 run_dir

看点：

- `cmd_prompt`
- `cmd_infer`
- `cmd_snippet` 实际由 `snippet_stage.run()` 完成
- `cmd_combine`
- `cmd_verify`
- `cmd_pipeline`

新人建议：

- 先看这个文件，建立全流程心智模型
- 不要在这里塞太多业务细节

### 5.2 [prompt_builder.py](/nfs_global/S/lvhanqi/project/sim2v/prompt_builder.py)

角色：

- infer prompt 构造器
- combine 元信息收集器

关键函数：

- `build_prompts()`
- `build_subtask_prompt()`
- `render_method_prompt()`
- `resolve_active_method_order()`
- `get_combine_info()`

理解重点：

- 这里不只是“拼 prompt 字符串”
- 更重要的是构造了很多后续阶段共享的上下文

### 5.3 [bsd_analyzer.py](/nfs_global/S/lvhanqi/project/sim2v/bsd_analyzer.py)

角色：

- 模块静态分析总入口

关键函数：

- `analyze_module()`
- `_detect_module_type()`
- `extract_methods()`

理解重点：

- 这是“上游元数据入口”
- 下游大多数模块都依赖它返回的 `module_info`

### 5.4 [bsd_types.py](/nfs_global/S/lvhanqi/project/sim2v/bsd_types.py)

角色：

- 解析 `simulator_include` 中的类型信息
- 生成 SV typedef 和变量声明

关键函数：

- `parse_all_structs()`
- `parse_all_constants()`
- `parse_helper_functions()`
- `generate_sv_typedefs()`
- `generate_sv_var_declarations()`
- `get_struct_order_for_method()`

理解重点：

- 这里不是通用 C++ 类型系统
- 是为当前 sim2v 任务裁剪过的类型数据库

### 5.5 [io_mapping.py](/nfs_global/S/lvhanqi/project/sim2v/io_mapping.py)

角色：

- 解析 `pi/po` 映射

关键函数：

- `parse_pi_to_simulator()`
- `parse_simulator_to_po()`
- `collect_outputs()`
- `get_mapping_provider()`

理解重点：

- 它解决的是“wrapper 与 simulator 的位级接口如何对应”
- 不是一般意义上的模块 IO parser

### 5.6 [signal_debug.py](/nfs_global/S/lvhanqi/project/sim2v/signal_debug.py)

角色：

- 用 libclang 提取 method 读写集合
- 做路径规范化

关键函数：

- `_extract_rw_libclang_file()`
- `_canonical_path()`

理解重点：

- 这是 snippet target 抽取的“主精确通道”
- 一旦失效，框架会退回文本启发式，质量会下降

### 5.7 [snippet_harness.py](/nfs_global/S/lvhanqi/project/sim2v/snippet_harness.py)

角色：

- snippet 阶段最核心的工程文件

它负责：

1. 选择 method 级输入/输出目标
2. 构造 `input_plan` / `compare_plan`
3. 生成单方法 SV wrapper
4. 生成单方法 C++ reference
5. 构造 debug prompt

关键函数：

- `build_method_io_targets()`
- `build_signal_plan()`
- `build_sv_wrapper()`
- `build_cpp_reference()`
- `build_debug_prompt()`

内部复杂点：

- `_expand_target_path()` / `_expand_target_recursive()`
- `_sv_leaf_expr()`
- `_packed_subpath_offset()`
- `_extract_precise_signal_paths()`
- `_expand_local_aliases()`

新人建议：

- 这是最值得花时间读的文件
- 如果 snippet 行为怪异，先查这里

### 5.8 [snippet_stage.py](/nfs_global/S/lvhanqi/project/sim2v/snippet_stage.py)

角色：

- snippet 调度器
- method 级 debug 循环入口

关键函数：

- `run()`
- `_run_one_method()`
- `_run_full_shell_compile_gate()`

理解重点：

- 这里不做复杂语义转换
- 它主要负责“任务调度、重试、结果落盘、调用 wrapper/reference/verify/LLM”

### 5.9 [combine_helpers.py](/nfs_global/S/lvhanqi/project/sim2v/combine_helpers.py)

角色：

- 完整模块拼装器

它负责把：

- typedef
- 变量声明
- 默认初始化
- method snippets
- `pi/po` 映射

组装成完整 `module`

理解重点：

- combine 只做“装配”
- 不应在这里偷偷引入新语义

### 5.10 [verify.py](/nfs_global/S/lvhanqi/project/sim2v/verify.py)

角色：

- 统一验证入口

关键函数：

- `verify()`
- `compile_only()`
- `_run_yosys_syntax_check()`
- `_run_verification()`
- `_gen_testbench()`

理解重点：

- 这里统一负责：
  - 物化测试产物
  - yosys 前端检查
  - Verilator 编译
  - testbench 运行

### 5.11 [call_llm.py](/nfs_global/S/lvhanqi/project/sim2v/call_llm.py)

角色：

- LLM 调用与恢复

关键函数：

- `get_client()`
- `ask_llm()`
- `run()`

理解重点：

- resume 语义基于 `(task, prompt_hash)`
- 这里不要和业务逻辑耦合过深


## 6. 当前版本哪些地方是“临时 / 脆弱 / case-driven”的

这部分要明确，不然后人容易误以为这些写法本身很优雅。

### 6.1 文本解析器很多

包括：

- `io_mapping.py`
- `bsd_analyzer.py`
- `bsd_types.py`
- `snippet_harness.py`

问题：

- 依赖代码风格稳定
- 容易被新 case 的语法细节打断

结论：

- 这是当前实现方式，不是框架本质

### 6.2 `simulator_include` 路径是硬编码共享目录

当前版本默认使用：

- `io_generator/simulator_include`

这对 PRF / ROB 足够，但不适合更复杂的多 case 混用场景。

结论：

- 这是当前基线分支的实现约束
- 不是框架级必须

### 6.3 helper / constants 来源比较窄

当前主要来自：

- `config.h`
- `util.h`

结论：

- 对当前 case 够用
- 对更一般的模块并不充分

### 6.4 prompt 规则里有大量经验化约束

例如：

- 不要发明 `x_1 = x`
- 不要用某些 size-cast
- 不要在 loop header 放运行时边界

这些规则大多是为了收敛实际错误样本积累出来的。

结论：

- 它们有价值
- 但属于经验规则，不是原理性最优答案


## 7. 当前版本哪些内容相对稳，可以继续沿用

### 7.1 method-level divide-and-conquer

这是整个项目里最有效的设计。

### 7.2 `combine_info` / `module_info` 这两层中间表示

虽然内部字段还可以继续整理，但这个层级本身是对的：

- `module_info`: 上游静态解析结果
- `combine_info`: 下游装配上下文

### 7.3 signal plan 驱动 wrapper/reference 对称生成

这是 snippet 能成立的关键。

### 7.4 完整 run_dir 留档

每轮都把 prompt / response / snippet / verify 产物落盘，
这对调试非常重要。


## 8. 新同学建议阅读顺序

建议按下面顺序读：

1. [run.py](/nfs_global/S/lvhanqi/project/sim2v/run.py)
   - 先知道流程怎么串起来
2. [prompt_builder.py](/nfs_global/S/lvhanqi/project/sim2v/prompt_builder.py)
   - 理解 prompt 和 combine 上下文怎么来
3. [bsd_analyzer.py](/nfs_global/S/lvhanqi/project/sim2v/bsd_analyzer.py)
   - 理解 `module_info`
4. [bsd_types.py](/nfs_global/S/lvhanqi/project/sim2v/bsd_types.py)
   - 理解类型系统来源
5. [snippet_stage.py](/nfs_global/S/lvhanqi/project/sim2v/snippet_stage.py)
   - 理解 snippet 调度逻辑
6. [snippet_harness.py](/nfs_global/S/lvhanqi/project/sim2v/snippet_harness.py)
   - 理解 signal plan、wrapper、reference
7. [verify.py](/nfs_global/S/lvhanqi/project/sim2v/verify.py)
   - 理解最终验证
8. [io_mapping.py](/nfs_global/S/lvhanqi/project/sim2v/io_mapping.py)
   - 理解 `pi/po` 来源
9. [signal_debug.py](/nfs_global/S/lvhanqi/project/sim2v/signal_debug.py)
   - 理解 read/write set 抽取


## 9. 哪些内容可以大胆改，哪些要谨慎

### 9.1 可以大胆改

- prompt 文案组织方式
- `io_mapping.py` 的内部解析方式
- `bsd_types.py` 的内部解析方式
- `signal_debug.py` 的 AST / fallback 实现细节
- snippet 调度策略
- verify 的 testbench 实现细节
- 文档和日志格式

前提是：

- 不破坏主流程边界
- 不破坏 `module_info` / `combine_info` / signal plan 的基本契约

### 9.2 要谨慎改

- 只翻 method body 这条边界
- active method 以 `io_generator_outer()` 为准
- snippet 先于 combine
- signal plan 同时驱动 SV wrapper 和 C++ reference
- combine 不偷偷引入语义

这些属于框架真正的骨架。


## 10. 当前分支的使用建议

当前分支建议仅把 `PRF / ROB` 当作日常示例和回归基线。

典型命令：

```bash
python run.py prompt
python run.py infer
python run.py snippet
python run.py combine
python run.py verify
```

或者：

```bash
python run.py pipeline
```

如果要做新 case，请先问自己两件事：

1. 这是在扩展**策略**，还是只是在修某个实现细节？
2. 这个改动会不会破坏 PRF / ROB 的现有稳定路径？

如果不能清楚回答，先不要动主流程。


## 11. 一句话总结

这套仓库当前最值得保留的不是某段具体代码，而是这三条策略：

1. **method 级翻译**
2. **snippet 级先验验证**
3. **框架负责壳，LLM 只负责 method body**

只要这三条不丢，很多实现细节都可以重写。  
如果这三条被打散，项目会很快重新回到“整模块难以调试”的状态。
