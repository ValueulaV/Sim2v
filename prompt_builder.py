"""Build LLM prompts for bsd module methods, output as JSONL.

Divide-and-conquer: each translated method body becomes an independent sub-task.
LLM is only asked to generate method-body statements (not modules/typedefs/framework code).
"""

import os
import json
import re

import io_mapping

from bsd_analyzer import (
    analyze_module, build_helper_db, generate_sv_typedefs, generate_sv_var_declarations,
    extract_method_helpers,
    parse_all_constants, get_struct_order_for_method, generate_cpp_type_sources,
    get_method_signal_width_hints, KNOWN_CONSTANTS, _try_eval_const_expr,
    _method_referenced_structs,
    project_context_for_logic,
)

# ---- Prompt constants ----

INFER_ROLE_INTRO = "You translate C++ cycle-level simulator logic into SystemVerilog."

FRAMEWORK_GUARANTEE_TEMPLATE = """\
Framework facts (do NOT re-implement):
- All `in_*`, `out_*`, and internal signals are already declared by the wrapper module.
- The wrapper `always_comb` already does: default-init of writable vars, input extraction from `pi[]`,
  calls methods in order, and output packing to `po[]`.
- Your output will be pasted directly inside an existing `begin : {method_name} ... end` wrapper block.
  Do NOT add another top-level `begin/end` wrapper (named or unnamed).
- You must only implement the logic that belongs to `{module_type}::{method_name}`."""

FRAMEWORK_SKELETON_TEMPLATE = """\
Framework skeleton (DO NOT output this wrapper; it's shown only to avoid misunderstanding):
```systemverilog
always_comb begin
    // ... framework defaults + input extraction already done ...
    begin : {method_name}
        // === YOUR OUTPUT IS INSERTED HERE ===
        // (statements only; no top-level begin/end)
    end
    // ... other methods + output packing handled by framework ...
end
```"""

def _infer_output_block(use_think):
    if use_think:
        return (
            "You MUST return two top-level XML blocks in this order:\n"
            "1) <think>...</think> : your private reasoning\n"
            "2) <answer>...</answer> : final answer only\n\n"
            "Inside <answer>, return a SystemVerilog code block (```systemverilog```) containing the logic body\n"
            "that goes INSIDE an `always_comb begin ... end` block.\n"
            "Do NOT include `always_comb`, `module`, `typedef`, `function`, or `localparam`.\n"
            "Do NOT wrap your entire output with a top-level `begin ... end` (named or unnamed)."
        )
    return (
        "Return ONLY a SystemVerilog code block (```systemverilog```) containing the logic body\n"
        "that goes INSIDE an `always_comb begin ... end` block.\n"
        "Do NOT include `always_comb`, `module`, `typedef`, `function`, or `localparam`.\n"
        "Do NOT wrap your entire output with a top-level `begin ... end` (named or unnamed)."
    )


SV_NAMING_RULES = """\
Only two prefix substitutions. Keep all other naming identical to C++:
- `in.X->` -> `in_X.` (example: `in.dis2rob->uop[i]` -> `in_dis2rob.uop[i]`)
- `out.X->` -> `out_X.` (example: `out.rob_bcast->flush` -> `out_rob_bcast.flush`)
- For pointer-style access in C++, use dot access in SystemVerilog.

**Keyword Escaping**: Some C++ field names are SystemVerilog reserved keywords.
These fields are renamed with `_v` suffix in the struct typedefs and all references:
- `type` → `type_v`   (e.g. `uop.type` → `uop.type_v`)
- `event` → `event_v`
The typedefs, variable declarations, and pi/po mappings already use the escaped names.
You MUST use the same escaped names in your logic."""

TRANSLATION_RULES = """\
1. Convert only this method body. Do not touch other methods.
2. Apply naming map: `in.X->Y` -> `in_X.Y`, `out.X->Y` -> `out_X.Y`.
3. Preserve control flow and ordering (`if/else/for/case`).
4. Translate helper macros/functions semantically.
5. Ignore prints/exits (`cout`, `printf`, `exit`).
6. Keep the result compile-clean (no WIDTHEXPAND/WIDTHTRUNC/LATCH).
6a. Stay within a yosys-friendly synthesizable SystemVerilog subset. Avoid simulator-only or highly dynamic constructs.
6b. Do not use `automatic` locals.
6b1. Do not use `break`, `continue`, or `disable` to exit loops. Rewrite with done-flags or gated execution.
6c. Do not use size-cast syntax like `4'(expr)` or `int'(expr)`. Use width-safe literals, concatenation, masks, or slices instead.
6d. Avoid direct dynamic field access on aggregate arrays such as `arr[idx].field` when `arr` is an array of structs/packed aggregates.
    Prefer one of these yosys/verilator-safe patterns instead:
    - Read: `tmp = arr[idx]; x = tmp.field;`
    - Write: `tmp = arr[idx]; tmp.field = ...; arr[idx] = tmp;`
    - Or use constant-bound loops / case statements that select whole elements, then access fields on the selected temporary.
6d1. For multi-dimensional aggregate arrays, never write forms like `arr[i][j].field` directly.
     Use a typed temporary for the selected element first, e.g.:
     - `req_tmp = in_dis2iss.req[i][j]; if (req_tmp.valid) ...`
     - Never use `in_dis2iss.req[i][j].valid` directly.
6d2. Never duplicate the container name after `in_*/out_*` mapping.
     Example: C++ `out.iss2dis->ready_num[i]` must become `out_iss2dis.ready_num[i]`,
     not `out_iss2dis.iss2dis.ready_num[i]`.
6d3. Yosys requires procedural `for` loops to have compile-time-constant init/condition/step expressions.
     If the original C++ loop starts or ends from a runtime signal, rewrite it as a constant-bound loop
     over the full legal range and guard the body with `if (...)`.
6e. Do not create extra named blocks such as `begin : helper_locals`.
6f. Do not invent new typedef names. Use only typedef names that already appear in the provided type context.
6f1. Do not guess queue/entry/uop typedef aliases from C++ class names.
     For Isu queue wakeup logic, use only declared aliases from the provided SV typedef section
     (e.g. `IqStoredEntry_t`, `IqStoredUop_t`, `IssPrfEntry_t`).
6g. Do not emit C++ container/member-method calls in SystemVerilog snippets.
    Names like `clear`, `push_back`, `reserve`, `resize`, `schedule`, `commit_issue`, `wakeup`, `tick`
    are C++ APIs, not synthesizable SV fields/methods in this framework.
6h. For Isu methods with internal queue state (`comb_enq`, `comb_issue`):
    preserve full C++ queue semantics, including side effects on dependency/wakeup bookkeeping
    when the C++ method performs them.
6i. For Isu `comb_issue`, preserve C++ scheduling + commit semantics exactly.
    Do not simplify away intermediate state updates that affect later cycles.
6j. For Isu `comb_enq`, preserve C++ enqueue semantics exactly, including dependency-matrix side effects.
    Do not reduce behavior to only `entry_1.valid/count_1` updates.
6k. Never reference undeclared members not present in provided SV declarations.
    Forbidden guessed names include: `num_ports`, `port_num`, `queue_depth`, `cfg`.
6l. For Isu queue scheduling methods, preserve C++ port-domain semantics exactly.
    If C++ uses `ports.size()`, map it to the declared SV port-array domain with constant-bound loops
    (typically `0..ISSUE_WIDTH-1`) plus guards as needed.
    Do not invent undeclared `num_ports`/`port_num` fields.
6m. If C++ source uses container size/length metadata, map it to the declared SV fields from context.
    Do not invent parallel metadata fields with similar names.
7. Preserve update semantics exactly:
   - `x++`, `++x`, `x += y`, `x--`, `--x`, `x -= y` update `x` itself.
   - If the C++ writes `x_1++`, translate that as an update to `x_1`, not `x`.
   - Do not rewrite a self-update like `x_1--` into `x_1 = x - 1` unless the C++ explicitly uses `x`.
8. Preserve untouched signals:
   - If the C++ method does not assign a signal on some path, do not invent a value for it from a similarly named signal.
   - Do not add whole-object copies such as `a_1 = a` or `state_next = state` unless the C++ method explicitly does that."""

OUTPUT_CONSTRAINTS = """\
Output rules (STRICT):
1. Output only statements valid inside an existing `always_comb` block.
2. Do NOT output `always_*`, `module`, `typedef`, `function`, `localparam`, or `assign`.
   Do NOT output a top-level `begin : ...` / `begin ... end` wrapper for the whole method body.
   Do NOT emit named procedural blocks (`begin : label ... end`) anywhere in the snippet.
3. You MAY declare local variables if needed, but:
   - do not redeclare/shadow framework-declared signals/fields
   - declare locals at the top of the method body before executable statements
   - do not declare array-typed locals (packed or unpacked) inside the method body
   - do not declare unpacked-array locals inside procedural code
   - unconditionally initialize locals before any branching; assign defaults on all paths to avoid latches
   - if a C++ local would otherwise be conditionally assigned before later use, give it an explicit safe default
     instead of relying on uninitialized behavior
   - for maximum yosys compatibility, declare loop variables separately as `integer` or `int` locals
     and then use `for (i = ... )`; avoid `for (int i = ... )`
   - if a loop bound or start depends on a runtime signal, do not put that runtime expression directly
     in the `for (...)` header; use a constant-bound loop plus `if (...)` inside the body
4. Do NOT emit framework-owned boilerplate (pi/po mapping, default init) unless it exists in the C++ method.
   Do NOT invent carry/copy/default behavior for unrelated signals just because names look similar
   (for example, do not assume `x_1` must copy `x`) unless the C++ method explicitly does that.
   If a checked signal also appears as an existing variable, treat it as an independent signal.
   When the C++ performs an in-place update on that signal, update that same signal directly.
   Example: C++ `count_1--;` means SystemVerilog `count_1 = count_1 - 1;`, not `count_1 = count - 1;`.
5. Keep widths explicit: use casts/slices/masks to avoid WIDTHEXPAND/WIDTHTRUNC.
   For power-of-two modulo on fixed-width vars, prefer `& mask` or slices.
5a. Keep field comparisons width-consistent:
    if a field is declared 8-bit (e.g. `*_preg`), compare/assign with 8-bit temporaries or slices.
    Avoid widening one side to 32-bit unless the destination/operation truly requires 32-bit."""


METHOD_EXTRA_RULES = {
    ("Isu", "comb_issue"): """\
Method-specific constraints:
- The compare targets are issue outputs (`out_iss2prf.iss_entry[*]`) only.
- Preserve full C++ `schedule()` + `commit_issue()` semantics, not a reduced approximation.
- Preserve C++ active-port semantics (`ports.size()`) exactly.
  In this fixed-array SV environment, model that domain with `ISSUE_WIDTH`-bounded
  scanning over `q.ports[p]` (plus normal first-fit/compatibility guards).
  Do not use `configs[i].dispatch_width` as a substitute for `q.ports.size()` in this method.
- Preserve state-source semantics exactly:
  - `schedule()` reads from `q.entry` (not `entry_1`);
  - `commit_issue()` updates `q.entry_1` and wake matrices based on committed indices.
- Preserve first-fit port binding semantics exactly:
  scan active ports from low to high index and pick the first free compatible port, using
  `q.ports[p].port_idx` and `q.ports[p].capability_mask` directly.
- `PortBinding_t` for this method has only the declared fields from SV context.
  Do not invent extra per-port metadata such as `port_valid`, `valid`, `enabled`, `present`,
  `port_num`, or `num_ports`.
- When scanning ports, treat the active domain as the framework-declared fixed `ISSUE_WIDTH`
  port array plus existing compatibility checks. Do not add an extra guessed `port_valid` gate.
- Preserve emit-stage port semantics exactly:
  after schedule returns `<entry_idx, phys_port>`, all downstream checks/outputs must index by
  `phys_port` (`in_exe2iss.ready[phys_port]`, `in_exe2iss.fu_ready_mask[phys_port]`,
  `out_iss2prf.iss_entry[phys_port]`).
  Never replace `phys_port` with schedule-loop indices (for example `p`, `issued_count`, or slot id).
- Preserve acceptance semantics exactly:
  only schedule results that pass emit-stage checks (`ready && fu_ready_mask bit && !flush && !mispred`)
  are committed in this cycle. Do not clear `entry_1`/decrement `count_1` for schedule-only candidates.
- Preserve commit read-source semantics exactly:
  when deciding whether to clear an issued slot, read validity/dependency source from pre-commit
  `entry_1` state for that same queue/slot (C++ `commit_issue` behavior).
- Preserve resource accounting semantics:
  increase `issued_count` and mark `port_busy[selected_port_idx]` only when a schedule candidate
  is actually accepted into schedule result (selected port exists).
  Do not consume port/issue budget for unscheduled entries.
- Forbidden anti-patterns for this method:
  - do not infer active port count by scanning `capability_mask != 0`.
  - do not build ad-hoc physical-port packing tables (e.g. `port_to_entry_packed`).
  - do not gate schedule selection directly by `exe2iss`; keep `exe2iss` checks in outer `comb_issue`
    emit stage after `schedule()` result selection, exactly like C++.
  - do not declare local array caches for scheduled/committed lists (examples: `sched_*[]`, `comm_*[]`,
    `accepted[]`, `*_indices[]`). Use scalar temporaries or existing framework state only.
  - do not declare any local unpacked arrays in this method (examples:
    `logic port_busy [0:11]; integer sched_entry_idx [0:11];` are forbidden).
  - do not index local arrays with runtime indices in this method.
    If temporary list behavior is needed, encode with fixed scalar slots (`slot0..slot11`) and explicit guards.
  - do not use dynamic chained field access on selected ports, such as
    `q.ports[selected_port_idx].port_idx` or `tmp_q.ports[sel_port_idx].port_idx`.
    Select the whole `PortBinding_t` into a temporary via constant-index `case`/guards first,
    then read `tmp_port.port_idx`.
  - when using `case (...)` for constant-index selection, each item must be plain SV syntax
    like `0: begin ... end`. Do not insert extra labels such as
    `0: some_label: begin ... end`.
  - do not write output fields through dynamic chained access such as
    `out_iss2prf.iss_entry[phys_port].valid` or
    `out_iss2prf.iss_entry[phys_port].uop.dest_preg`.
    For emit stage, select whole `IssPrfEntry_t` by constant-index `case` into a temporary,
    modify temporary fields, then write back the whole element with another constant-index `case`.
  - do not write to `configs` in this method.
  - do not mutate queue/config metadata fields in this method (`size`, `dispatch_width`, `ports[*].port_idx`);
    treat them as inputs only. Update only outputs and C++-equivalent queue state side effects
    (`out_iss2prf`, `entry_1`, `count_1`, wake matrices, committed indices buffer).
  - do not write `committed_indices_buf` in this method; keep committed tracking in local scalar temporaries only.
  - do not treat schedule result as committed by default.
    In C++, schedule result is filtered by emit-stage backpressure and flush/mispred before commit.
  - do not reuse the same loop variable name in nested loops (for example nested `for (i=...)` inside `for (i=...)`).
    Each nested loop must use a unique iterator variable.
""",
    ("Isu", "comb_enq"): """\
Method-specific constraints:
- The compare targets are queue occupancy/valid bits (`iqs[*].entry_1[*].valid`, `iqs[*].count_1`).
- Preserve conversion + wakeup-to-uop behavior for src busy bits before enqueue.
- On enqueue success, write one slot in `entry_1`, set `valid`, and increment `count_1`.
- Preserve enqueue side effects used by C++ queue model (including wake/dependency bookkeeping).
- Use only typedef names declared in the provided SV environment.
  Do not invent names like `IssAwakeEntry_t` or other undeclared aliases.
- Enqueue guard must mirror C++ queue precondition and must not write past free slots.
- Preserve enqueue search-domain semantics:
  scan slots over `0 .. q.size-1` (not hardcoded ISSUE_WIDTH/MAX_PORT loops),
  stop at first invalid slot, then set dependency bits for that exact slot.
- Use declared wake entry type names only (for example `WakeInfo_t` for wake bus entries).
  Do not invent aliases like `IssAwakeEntry_t`.
""",
    ("Isu", "comb_awake"): """\
Method-specific constraints:
- Use only typedef names declared in the provided SV environment.
- Do not invent undeclared wake-entry type aliases.
- Any loop over runtime counts (for example `preg_count`) must be rewritten as
  constant-bound loops (`0..MAX_WAKEUP_PORTS-1` / fixed IQ/slot bounds) plus
  internal `if (idx < runtime_count)` guards.
- Do not assign nested dynamic chain lvalues such as
  `iqs[q_idx].entry_1[slot_idx].uop.src1_busy = ...`.
  Use temporary element update pattern only:
  `tmp = iqs[q_idx].entry_1[slot_idx]; tmp.uop.src1_busy = ...; iqs[q_idx].entry_1[slot_idx] = tmp;`.
""",
    ("Isu", "init"): """\
Method-specific constraints:
- Translate C++ container-building logic into fixed-index SV initialization only.
- Rebuild `configs[*]` from `GLOBAL_IQ_CONFIG` and `GLOBAL_ISSUE_PORT_CONFIG` exactly.
- For each `configs[i].ports[p]`:
  `port_idx = iq_cfg.port_start_idx + p`, `capability_mask = GLOBAL_ISSUE_PORT_CONFIG[port_idx].support_mask`.
- Do not synthesize extra bit hacks/masks on `capability_mask`; keep direct table assignment semantics.
- Preserve `support_mask` bit pattern exactly; do not approximate with guessed hex constants.
- Set queue-side mirrors from config exactly: `iqs[i].id=size/dispatch_width` from the same config,
  and initialize `count/count_1` to zero.
- Preserve `add_iq(dynamic_cfg)` + `IssueQueue(cfg)` constructor semantics when building `iqs[i]`:
  - copy `ports` from `dynamic_cfg.ports` into `iqs[i].ports` (same indices and masks);
  - set `wake_words_per_row = (size + 63) / 64`;
  - clear `entry/entry_1` valid state and clear wake matrices (`wake_matrix_src1/src2`).
  Do not emit a partial IQ mirror that only writes id/size/dispatch/count fields.
- Avoid any module-instance hardcoded masks or hand-copied tables.
  Derive all `supported_ops`/`capability_mask` values only from the provided constants and source logic.
- Do not emit manually guessed hex literals for `supported_ops`/`capability_mask` in this method.
  Use table-driven expressions from provided global config context instead.
- Do not use `port_attributes` as a source for `supported_ops` or `capability_mask` in this method.
  Those values must come from global config tables (`GLOBAL_IQ_CONFIG`, `GLOBAL_ISSUE_PORT_CONFIG`) only.
- `dispatch_width` must be copied from `GLOBAL_IQ_CONFIG[i].dispatch_width` exactly.
  Do not derive `dispatch_width` from `port_num`, `ports.size`, or counted ports.
  In this project `GLOBAL_IQ_CONFIG[i].dispatch_width == DECODE_WIDTH`; use `DECODE_WIDTH`
  if needed, and never invent aliases like `GLOBAL_IQ_CONFIG_0_DISPATCH_WIDTH`.
- Use symbolic OP masks exactly (no numeric replacement) for this method:
  - Port0: `OP_MASK_ALU | OP_MASK_MUL | OP_MASK_CSR`
  - Port1: `OP_MASK_ALU | OP_MASK_DIV`
  - Port2: `OP_MASK_ALU | OP_MASK_FP`
  - Port3: `OP_MASK_ALU`
  - Port4/5: `OP_MASK_LD`
  - Port6/7: `OP_MASK_STA`
  - Port8/9: `OP_MASK_STD`
  - Port10/11: `OP_MASK_BR`
  and IQ supported ops:
  - IQ_INT: `OP_MASK_ALU | OP_MASK_MUL | OP_MASK_DIV | OP_MASK_CSR`
  - IQ_LD: `OP_MASK_LD`, IQ_STA: `OP_MASK_STA`, IQ_STD: `OP_MASK_STD`, IQ_BR: `OP_MASK_BR`.
- Forbidden literalization in this method:
  do not use numeric literals (such as `64'h...`, `64'd...`, `32'd4`) as replacements
  for `supported_ops`, `capability_mask`, or `dispatch_width` assignments.
- Forbidden invented constants in this method:
  do not reference undeclared names like `GLOBAL_IQ_CONFIG_0_DISPATCH_WIDTH`,
  `GLOBAL_IQ_CONFIG_1_DISPATCH_WIDTH`, etc.
- Required implementation skeleton (must be semantically equivalent):
  1) clear/reset `iqs`, `configs`, `latency_pipe`, `latency_pipe_1`, and `committed_indices_buf`.
  2) loop `i` over IQs and set:
     `configs[i].id/size/dispatch_width/supported_ops` directly from `GLOBAL_IQ_CONFIG[i]`.
  3) inside each IQ, loop `p` over ports, compute `global_idx = iq_cfg.port_start_idx + p`,
     then assign:
     `configs[i].ports[p].port_idx = global_idx;`
     `configs[i].ports[p].capability_mask = GLOBAL_ISSUE_PORT_CONFIG[global_idx].support_mask;`
  4) assign `iqs[i].id/size/dispatch_width` from `configs[i]` and zero `count/count_1`.
- Forbidden anti-patterns for this method:
  `capability_mask = {32'd0, port_attributes[...]}` (forbidden),
  `supported_ops` built from `port_attributes` (forbidden),
  `dispatch_width = 4/2/port_num` style derived constants (forbidden).
- Literal token guard for this method:
  the final SV snippet must not contain the token `port_attributes` anywhere.
  If you rely on `port_attributes` textually, the translation is considered incorrect.
""",
    ("Isu", "comb_calc_latency_next"): """\
Method-specific constraints:
- Compare targets focus on `latency_pipe_1[*].br_mask`; preserve exact C++ two-phase behavior:
  1) clear next-state, copy valid old entries with countdown>0 and decrement countdown;
  2) append new issued entries with `lat = get_latency(decode_uop_type(op))`, and only if `lat > 1`.
- For new entries, copy `br_mask` directly from `out_iss2prf.iss_entry[i].uop.br_mask` without modification.
- Use existing keyword-escaped field names from SV context exactly (e.g. `type_v` if present).
- In phase (1), preserve old-entry `br_mask` bitwise exactly when copying to next-state.
- Do not alter or re-encode `br_mask` in either phase.
- This method must not write `out_iss2prf` or any input interface signals.
- Match C++ signed-int semantics for `countdown`:
  evaluate old-entry keep condition as signed (`countdown > 0` in C++ int sense), then decrement by 1.
  Do not treat `countdown` as unsigned for this condition.
- Keep latency classification semantically equivalent to helper calls:
  `lat = get_latency(decode_uop_type(inst.uop.op))`.
  Do not replace this with guessed opcode-id literals (for example `op==13/14`) or ad-hoc tables.
- Forbidden patterns for this method:
  `if (entry.countdown > 32'h0)` or other unsigned-only comparisons on `countdown`,
  and any rewrite that can reorder preserved old entries relative to C++ push-back order.
- Do not emulate push-back with `for (w=0;w<N;w++) if (w==lp1_idx) ...` gated writes.
  Use direct `latency_pipe_1[lp1_idx]` read/modify/write with a typed temporary.
""",
    ("Isu", "comb_flush"): """\
Method-specific constraints:
- Preserve C++ `flush_br` / `clear_br` / latency-pipe behavior exactly.
- On `in.rob_bcast->flush`, clear all `iqs[*].entry_1[*].valid`, reset `count_1`,
  clear wake matrices, and clear both `latency_pipe` and `latency_pipe_1`.
- `flush_all()` has full-storage semantics: it clears every slot in `entry_1` (all 64 slots here),
  independent of runtime `q.size`. Do NOT guard flush-all slot clearing with `i < q.size`.
- Queue slot loops for this method are over IQ storage slots (up to 64 here), not over
  `ISSUE_WIDTH` / issue-port count. Never use 12 as the bound for `entry_1` traversal.
- `flush_br()` / `clear_br()` iterate `0..q.size-1` exactly (runtime queue size domain),
  while `flush_all()` clears the whole storage domain. Keep this distinction exactly.
- On `in.dec_bcast->mispred`, queue-side behavior must match C++ `flush_br(br_mask)` exactly.
- In queue-side `flush_br(br_mask)`, for each matching valid slot:
  clear dependency bits for that exact slot index, then clear `entry_1[i].valid`, then decrement `count_1`.
- For dependency-bit clearing, match `clear_dep_bits_for_slot(entry_1[i], i)` exactly:
  `slot_word = i >> 6`, `slot_bit = 1 << (i & 63)`,
  matrix row index = `preg * wake_words_per_row + slot_word`.
  Do NOT index wake matrices by `preg` alone.
- `wake_words_per_row` belongs to the live queue object/state (`q` / `iqs[qi]` / `IssueQueue_t`),
  not to `configs[qi]` / `IssueQueueConfig_t`.
  Do not read or invent `configs[qi].wake_words_per_row`.
- For `latency_pipe_1` in mispred path, preserve C++ `erase` semantics (order-preserving compaction):
  remove matching entries and shift later surviving entries down; do not only clear `valid` bits in place.
- In mispred-path compaction, preserve the exact C++ erase predicate:
  remove entries when `((entry.br_mask & br_mask) != 0)`, regardless of `valid`.
  Keep every entry whose `br_mask` does not match, even if `valid == 0`.
  Do not add an extra `valid` gate to the erase/keep decision.
- After flush/mispred handling, apply `clear_mask` with exact C++ scope:
  queue side via `clear_br(clear_mask)` (valid `entry_1` slots only), and
  latency-pipe side on every surviving `latency_pipe_1` element (do NOT gate by `valid`).
- For `latency_pipe_1`, preserve the exact C++ phase order:
  first erase/compact mispred-matching entries using the old `br_mask`,
  then run `entry.br_mask &= ~clear_mask` on every surviving compacted entry.
  Do not merge these phases, and do not leave surviving `br_mask` bits unchanged after `clear_mask`.
- In queue-side `clear_br(clear_mask)`, only valid `entry_1[i]` entries are updated,
  but the loop still scans all queue slots `0..q.size-1`.
- Keep full entry payload coherence during compaction:
  when moving surviving `latency_pipe_1` entries, move all fields together
  (`valid`, `countdown`, `dest_preg`, `br_mask`, `rob_idx`, `rob_flag`).
""",
    ("Csr", "comb_csr_read"): """\
	Method-specific constraints:
	- Do NOT call `cvt_number_to_csr()` as a function. It is a C++ switch-case that maps
	  CSR address numbers (e.g. 0x300) to enum array indices (e.g. csr_mstatus = 7).
	  Inline its logic as a case/lookup using the provided `number_*` and `csr_*` constants.
	- `CSR_RegFile[idx]` is a reg array indexed by enum_csr values, not RISC-V CSR addresses.
	""",
    ("Csr", "comb_csr_write"): """\
	Method-specific constraints:
	- Do NOT call `cvt_number_to_csr()` as a function. It is a C++ switch-case that maps
	  CSR address numbers (e.g. 0x300) to enum array indices (e.g. csr_mstatus = 7).
	  Inline its logic as a case/lookup using the provided `number_*` and `csr_*` constants.
	- `CSR_RegFile[idx]` is a reg array indexed by enum_csr values, not RISC-V CSR addresses.
	""",
    ("Csr", "comb_exception"): """\
	Method-specific constraints:
	- `CSR_RegFile[idx]` is a reg array indexed by enum_csr values (e.g. csr_mstatus = 7).
	  Use the `csr_*` constants directly as array indices. Do NOT call `cvt_number_to_csr()`.
	- `IrqState_t` is a packed struct with 6 boolean fields for interrupt states.
	  All fields: m_software_interrupt, m_timer_interrupt, m_external_interrupt,
	              s_software_interrupt, s_timer_interrupt, s_external_interrupt.
	- `eval_interrupts()` is provided as a helper function. Translate its logic faithfully.
	  The interrupt conditions use bitwise AND with mask constants (e.g. `mip_reg & MIP_MSIP`),
	  privilege level checks, and mstatus MIE/SIE bit gating. Preserve ALL conditions exactly.
	- `privilege` is a 2-bit signal [1:0]. `privilege < 3` is always true for 2 bits.
	  Replace `privilege < 3` with `1'b1` (always true). Replace `privilege == 3` with
	  `privilege == 2'b11` to avoid CMPCONST warnings.
	""",
    ("Csr", "comb_interrupt"): """\
	Method-specific constraints:
	- `CSR_RegFile[idx]` is a reg array indexed by enum_csr values.
	  Use the `csr_*` constants directly as array indices.
	- `IrqState_t` is a packed struct with 6 boolean fields for interrupt states.
	  All fields: m_software_interrupt, m_timer_interrupt, m_external_interrupt,
	              s_software_interrupt, s_timer_interrupt, s_external_interrupt.
	- `eval_interrupts()` is provided as a helper function. Translate its logic faithfully.
	  The interrupt conditions use bitwise AND with mask constants (e.g. `mip_reg & MIP_MSIP`),
	  privilege level checks, and mstatus MIE/SIE bit gating. Preserve ALL conditions exactly.
	- `privilege` is a 2-bit signal [1:0]. `privilege < 3` is always true for 2 bits.
	  Replace `privilege < 3` with `1'b1` (always true). Replace `privilege == 3` with
	  `privilege == 2'b11` to avoid CMPCONST warnings.
	""",
    ("Idu", "comb_decode"): """\
	Method-specific constraints:
	- CONFIG_BPU is NOT defined in this project. The C++ source contains `#ifndef CONFIG_BPU ... #else ... #endif`.
	  You MUST translate ONLY the `#ifndef CONFIG_BPU` branch (the non-BPU path). Ignore the `#else` BPU path entirely.
	- In the non-BPU path, `br_id` is always set to 0 and `br_mask` is always set to 0 for all decoded instructions.
	- Do NOT implement any branch tag allocation logic, running_mask computation, or br_num tracking.
	""",
    ("Idu", "comb_fire"): """\
	Method-specific constraints:
	- CONFIG_BPU is NOT defined in this project. The C++ source contains `#ifdef CONFIG_BPU` guards.
	  You MUST skip all code inside `#ifdef CONFIG_BPU ... #endif` blocks entirely.
	  Do NOT implement branch tag allocation (alloc_tag), br_num tracking, or checkpoint save logic.
	- Without CONFIG_BPU, the method only handles flush, matured_free release, clear_mask application,
	  misprediction recovery, and the basic fire/dispatch loop without branch tag management.
	- In step 3 (clear checkpoints), the C++ loop starts from `i = 1`, NOT `i = 0`.
	  `br_mask_cp_1[0]` is never modified by the clear loop.
	""",
}


METHOD_PROMPT_TEMPLATE = """\
## TASK (STRICT SCOPE)
{role_intro}
Target: `{module_type}::{method_name}` only.
Do NOT implement other methods. Do NOT modify framework code.
{framework_guarantee}
{framework_skeleton}
### END TASK ###

## OUTPUT (STRICT)
{output_block}
### END OUTPUT ###

## SV ENVIRONMENT (ALREADY DECLARED BY FRAMEWORK)
Use these existing variables directly. Do NOT redeclare them.

Inputs:
```systemverilog
{input_vars}
```

Internals:
```systemverilog
{internal_vars}
```

Outputs:
```systemverilog
{output_vars}
```
### END SV ENVIRONMENT ###

## CONTEXT
Naming rules:
{naming_rules}
{constants_block}
{signal_width_block}
{project_context_section}\
### END CONTEXT ###

{type_defs_section}\
## SOURCE OF TRUTH (C++)
```cpp
void {module_type}::{method_name}() {{
{method_body}
}}
```
{helpers_section}\
### END SOURCE OF TRUTH ###

## RULES
{translation_rules}
{output_constraints}
### END RULES ###"""


def _is_translated_method(name):
    return name == "init" or name.startswith("comb_")


def _format_helpers(helpers):
    if not helpers:
        return ""
    lines = ["Helper functions (C++ source):", "```cpp"]
    for src in helpers.values():
        lines.append(src)
        lines.append("")
    lines.append("```")
    lines.append("### END OF HELPER FUNCTIONS ###")
    return "\n".join(lines)


def filter_used_constants(all_constants, method_body, helpers):
    text = method_body + " " + " ".join(helpers.values())
    return {
        k: v for k, v in all_constants.items()
        if re.search(rf'\b{re.escape(k)}\b', text)
    }


def _strip_outer_parens(expr):
    expr = expr.strip()
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
    return expr


def _split_top_level(expr, operator):
    parts = []
    depth = 0
    start = 0
    i = 0
    while i < len(expr):
        ch = expr[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and expr.startswith(operator, i):
            parts.append(expr[start:i].strip())
            i += len(operator)
            start = i
            continue
        i += 1
    if parts:
        parts.append(expr[start:].strip())
    return [p for p in parts if p]


def _lower_sv_constant_expr(expr, known_constants):
    expr = (expr or "").strip()
    if not expr:
        return None

    val = _try_eval_const_expr(expr, known_constants)
    if val is not None:
        return str(val)

    expr = _strip_outer_parens(expr)

    and_parts = _split_top_level(expr, "&&")
    if and_parts:
        lowered = [_lower_sv_constant_expr(part, known_constants) for part in and_parts]
        if any(part == "0" for part in lowered):
            return "0"
        if all(part is not None for part in lowered):
            return "1" if all(int(part) != 0 for part in lowered) else "0"
        return None

    or_parts = _split_top_level(expr, "||")
    if or_parts:
        lowered = [_lower_sv_constant_expr(part, known_constants) for part in or_parts]
        if any(part is not None and int(part) != 0 for part in lowered):
            return "1"
        if all(part is not None for part in lowered):
            return "1" if any(int(part) != 0 for part in lowered) else "0"
        return None

    return None


def _select_sv_constants(all_constants, text):
    # prompt 里只保留“能安全降成 SV 常量”的项。
    # 这样可以避免把 C++ 环境专属表达式直接暴露给 LLM，
    # 也保证这些常量在 wrapper/combine 阶段能被稳定复用。
    used = []
    for name, value in sorted(all_constants.items()):
        if not re.search(rf"\b{re.escape(name)}\b", text):
            continue
        lowered = _lower_sv_constant_expr(value, KNOWN_CONSTANTS)
        if lowered is None:
            continue
        used.append((name, lowered))
    return used


def _all_sv_constants(all_constants):
    """Return all constants that can be safely lowered to SV literals."""
    used = []
    for name, value in sorted(all_constants.items()):
        lowered = _lower_sv_constant_expr(value, KNOWN_CONSTANTS)
        if lowered is None:
            continue
        used.append((name, lowered))
    return used


def select_prompt_constants(all_constants, text):
    return {name: value for name, value in _select_sv_constants(all_constants, text)}


def render_method_prompt(
    *,
    module_type,
    method_name,
    method_body,
    cpp_type_sources,
    sv_typedefs,
    var_decls,
    method_helpers,
    used_constants,
    signal_width_hints,
    project_context,
    role_intro,
    output_block,
):
    # infer prompt 的核心装配点。
    # 上游模块只负责提供上下文；这里统一拼接任务边界、变量环境、
    # 常量/宽度提示、类型定义和 C++ source of truth。
    input_vars, output_vars, internal_vars = var_decls
    const_section = "\n".join(f"{k} = {v}" for k, v in sorted(used_constants.items()))
    constants_block = (
        "Constants (C preprocessor defines, may be referenced by name):\n"
        "```text\n"
        f"{const_section}\n"
        "```\n"
    ) if const_section else ""
    if signal_width_hints:
        width_lines = "\n".join(f"- `{p}` : {w} bits" for p, w in signal_width_hints)
        signal_width_block = f"Signal width hints:\n{width_lines}\n"
    else:
        signal_width_block = ""
    if project_context.strip():
        project_context_section = (
            "Project config excerpts:\n"
            "```cpp\n"
            f"{project_context}\n"
            "```\n"
        )
    else:
        project_context_section = ""
    if cpp_type_sources.strip() or sv_typedefs.strip():
        type_defs_section = (
            "## TYPE DEFINITIONS (C++ -> SV)\n"
            "```cpp\n"
            f"{cpp_type_sources}\n"
            "```\n\n"
            "```systemverilog\n"
            f"{sv_typedefs}\n"
            "```\n"
            "### END OF TYPE DEFINITIONS ###\n\n"
        )
    else:
        type_defs_section = ""
    extra_method_rules = METHOD_EXTRA_RULES.get((module_type, method_name), "").strip()
    translation_rules = TRANSLATION_RULES
    if extra_method_rules:
        translation_rules = f"{TRANSLATION_RULES}\n{extra_method_rules}"

    return METHOD_PROMPT_TEMPLATE.format(
        role_intro=role_intro,
        module_type=module_type,
        method_name=method_name,
        framework_guarantee=FRAMEWORK_GUARANTEE_TEMPLATE.format(
            module_type=module_type,
            method_name=method_name,
        ),
        framework_skeleton=FRAMEWORK_SKELETON_TEMPLATE.format(
            method_name=method_name,
        ),
        naming_rules=SV_NAMING_RULES,
        constants_block=constants_block,
        signal_width_block=signal_width_block,
        project_context_section=project_context_section,
        type_defs_section=type_defs_section,
        input_vars="\n".join(input_vars),
        output_vars="\n".join(output_vars),
        internal_vars="\n".join(internal_vars),
        helpers_section=_format_helpers(method_helpers),
        method_body=method_body,
        translation_rules=translation_rules,
        output_block=output_block,
        output_constraints=OUTPUT_CONSTRAINTS,
    )


def render_infer_method_prompt(
    *,
    module_type,
    method_name,
    method_body,
    cpp_type_sources,
    sv_typedefs,
    var_decls,
    method_helpers,
    used_constants,
    signal_width_hints,
    project_context,
    use_think=True,
):
    return render_method_prompt(
        module_type=module_type,
        method_name=method_name,
        method_body=method_body,
        cpp_type_sources=cpp_type_sources,
        sv_typedefs=sv_typedefs,
        var_decls=var_decls,
        method_helpers=method_helpers,
        used_constants=used_constants,
        signal_width_hints=signal_width_hints,
        project_context=project_context,
        role_intro=INFER_ROLE_INTRO,
        output_block=_infer_output_block(use_think),
    )




def build_subtask_prompt(method, helpers_db, all_constants, module_info,
                         cpp_type_sources, sv_typedefs, var_decls, infer_use_think=True):
    """Build a prompt for one comb_* method sub-task using SV struct syntax."""
    # prompt 的上下文按“只够翻当前 method”为原则裁剪：
    # helper 只带被当前 method 直接/间接调用到的；
    # 常量只带 logic 文本实际引用到的；
    # 宽度提示只覆盖当前 method 出现过的信号。
    method_helpers = extract_method_helpers(method["body"], helpers_db)
    logic_text = method["body"] + "\n" + "\n".join(method_helpers.values())
    used_consts = select_prompt_constants(all_constants, logic_text)
    width_hints = get_method_signal_width_hints(
        method["body"], method_helpers,
        module_info["structs"], module_info["module_type"], module_info["type_widths"],
    )
    project_context = project_context_for_logic(logic_text)
    content = render_infer_method_prompt(
        module_type=module_info["module_type"],
        method_name=method["name"],
        method_body=method["body"],
        cpp_type_sources=cpp_type_sources,
        sv_typedefs=sv_typedefs,
        var_decls=var_decls,
        method_helpers=method_helpers,
        used_constants=used_consts,
        signal_width_hints=width_hints,
        project_context=project_context,
        use_think=infer_use_think,
    )

    return {
        "task": f"{module_info['module_type']}_{method['name']}",
        "module_type": module_info["module_type"],
        "method": method["name"],
        "messages": [{"role": "user", "content": content}],
    }


def _read_text(path):
    with open(path) as f:
        return f.read()


def _find_io_generator_outer_header(bsd_dir):
    """Find the generated <dir_base>.h that defines io_generator_outer()."""
    dir_base = os.path.basename(bsd_dir.rstrip("/"))
    preferred = os.path.join(bsd_dir, f"{dir_base}.h")
    if os.path.exists(preferred):
        return preferred

    try:
        for fname in sorted(os.listdir(bsd_dir)):
            if not fname.endswith(".h"):
                continue
            fpath = os.path.join(bsd_dir, fname)
            try:
                text = _read_text(fpath)
            except OSError:
                continue
            if "io_generator_outer" in text:
                return fpath
    except OSError:
        return None
    return None


def _parse_comb_call_order_from_outer(header_text):
    """Extract translated method call order from io_generator_outer() wrapper."""
    region = header_text
    m = re.search(r"//please add code below(.*?)//end of code add", header_text, re.DOTALL)
    if m:
        region = m.group(1)

    # Ignore commented-out pseudo calls (for example `// isu.init();`) so we only
    # keep real executable calls from the wrapper body.
    region = re.sub(r"/\*.*?\*/", "", region, flags=re.DOTALL)
    cleaned_lines = []
    for line in region.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("//"):
            continue
        if "//" in line:
            line = line.split("//", 1)[0]
        cleaned_lines.append(line)
    region = "\n".join(cleaned_lines)

    calls = re.findall(
        r"(?:\.|->)\s*((?:init|comb_[A-Za-z0-9_]+))\s*\(\s*\)\s*;",
        region,
    )
    out = []
    seen = set()
    for name in calls:
        if name in seen:
            continue
        seen.add(name)
        out.append(name)
    return out


def resolve_active_method_order(module_info, outer_header_text=None):
    # active methods 以 io_generator_outer() 的真实调用顺序为准，
    # 这是框架级策略：prompt/snippet/combine/full verify 必须看到同一组 method。
    parsed_method_order = [
        m["name"] for m in module_info["methods"] if _is_translated_method(m["name"])
    ]
    if outer_header_text:
        outer_order = _parse_comb_call_order_from_outer(outer_header_text)
        if outer_order:
            parsed_set = set(parsed_method_order)
            active_order = [m for m in outer_order if m in parsed_set]
            # Keep init task generation stable even when wrapper call is absent/commented.
            # Downstream stages rely on init for table-driven state/config construction.
            if "init" in parsed_set and "init" not in active_order:
                init_pos = parsed_method_order.index("init")
                insert_pos = len(active_order)
                for idx, name in enumerate(active_order):
                    if parsed_method_order.index(name) > init_pos:
                        insert_pos = idx
                        break
                active_order.insert(insert_pos, "init")
            return active_order
    return parsed_method_order


def get_combine_info(bsd_dir, base_dir=".", mapping_provider=None):
    """Collect everything needed to assemble per-bsd-file SV modules.

    Returns dict:
      module_type, pi_width, po_width,
      method_order, sv_typedefs, var_decls,
      pi_sv_code, po_sv_code,
      bsd_files: [{module_name, pi_width, po_width}]
    """
    if mapping_provider is None:
        mapping_provider = io_mapping.get_mapping_provider()

    full_dir = os.path.join(base_dir, bsd_dir) if base_dir != "." else bsd_dir
    module_info = analyze_module(full_dir, mapping_provider=mapping_provider)
    module_type = module_info["module_type"]

    # 优先采用 wrapper 中的真实调用顺序。
    # 这样可以保证 combine 出来的 always_comb 调用顺序与最终验证壳一致。
    method_order = None
    outer_h = _find_io_generator_outer_header(full_dir)
    if outer_h:
        outer_text = _read_text(outer_h)
        method_order = resolve_active_method_order(module_info, outer_text)
    if method_order is None:
        method_order = resolve_active_method_order(module_info)

    # combine_info 是各阶段共享的“模块装配上下文”：
    # prompt 用它准备环境，snippet 用它生成单方法 wrapper，
    # combine 用它装配完整模块。
    # Scan all method bodies for struct references not reachable from the
    # module type itself (e.g. IrqState used as local variables in methods).
    all_method_text = "\n".join(m.get("body", "") for m in module_info.get("methods", []))
    extra_struct_roots = _method_referenced_structs(module_info["structs"], all_method_text)
    sv_typedefs = generate_sv_typedefs(
        module_info["structs"], module_info["type_widths"], module_type=module_type,
        expand_depth=-1, extra_roots=extra_struct_roots,
    )
    var_decls = generate_sv_var_declarations(module_info["structs"], module_type)
    all_constants = parse_all_constants()
    # combine/snippet wrappers should be robust to model-generated references
    # to project constants that may not appear in the extracted logic text.
    # Emit all safely-lowerable constants to avoid undefined symbol compile errors.
    used_constants = _all_sv_constants(all_constants)

    pi_lines = mapping_provider.generate_pi_sv(
        module_info["inputs"], max_width=module_info["pi_width"], module_info=module_info
    )
    pi_code = "\n".join(pi_lines)

    bsd_entries = []
    for bf in module_info["bsd_files"]:
        module_name = os.path.splitext(bf["filename"])[0]
        out_signals = [{"path": sig, "width": w} for sig, w in bf["unpack_lines"]]
        po_lines = mapping_provider.generate_po_sv(
            out_signals, max_width=bf["po_width"], module_info=module_info
        )

        bsd_entries.append({
            "module_name": module_name,
            "pi_width": module_info["pi_width"],
            "po_width": bf["po_width"],
            "pi_code": pi_code,
            "po_code": "\n".join(po_lines),
        })

    return {
        "module_type": module_type,
        "pi_width": module_info["pi_width"],
        "method_order": method_order,
        "sv_typedefs": sv_typedefs,
        "sv_constants": used_constants,
        "var_decls": var_decls,
        "bsd_files": bsd_entries,
    }


def build_prompts(input_dirs, output_path, base_dir=".", struct_expand_depth=2,
                  infer_use_think=True, mapping_provider=None):
    """Generate per-method sub-task prompts for bsd modules.

    Only processes *_bsd directories. Each translated method becomes one prompt entry.
    """
    # 这一层只做静态任务生成，不调用 LLM。
    # 粒度是：input_dir -> module -> active method -> one JSONL prompt entry。
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    count = 0

    if mapping_provider is None:
        mapping_provider = io_mapping.get_mapping_provider()

    with open(output_path, "w") as f:
        for d in input_dirs:
            full_dir = os.path.join(base_dir, d)
            if not os.path.isdir(full_dir) or not os.path.basename(d).endswith("_bsd"):
                continue

            module_info = analyze_module(full_dir, mapping_provider=mapping_provider)
            helpers_db = build_helper_db(module_info)
            all_constants = parse_all_constants()
            var_decls = generate_sv_var_declarations(
                module_info["structs"], module_info["module_type"],
            )
            combine_info = get_combine_info(d, base_dir=base_dir, mapping_provider=mapping_provider)
            ordered_methods = {m["name"]: m for m in module_info["methods"]}
            # 这里只遍历 combine_info 给出的 method_order，
            # 避免把 wrapper 不会调用的方法也送去翻译。
            for method_name in combine_info["method_order"]:
                method = ordered_methods.get(method_name)
                if not method:
                    continue
                method_helpers = extract_method_helpers(method["body"], helpers_db)
                logic_text = method["body"] + "\n" + "\n".join(method_helpers.values())
                ordered = get_struct_order_for_method(
                    module_info["structs"],
                    module_info["module_type"],
                    method_body=logic_text,
                    expand_depth=struct_expand_depth,
                )
                sv_typedefs = generate_sv_typedefs(
                    module_info["structs"], module_info["type_widths"],
                    ordered_structs=ordered,
                )
                entry = build_subtask_prompt(
                    method, helpers_db, all_constants, module_info,
                    "", sv_typedefs, var_decls, infer_use_think=infer_use_think,
                )
                f.write(json.dumps(entry, ensure_ascii=False) + "\n")
                count += 1

    return count
