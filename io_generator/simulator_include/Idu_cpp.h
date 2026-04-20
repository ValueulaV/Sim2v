#ifndef IDU_CPP_H
#define IDU_CPP_H
#include "Idu.h"
#include "Csr.h"
#include "RISCV.h"
#include "IO.h"
#include "config.h"
// #include "ref.h"
#include "util.h"
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include </nfs_global/S/zhengyuxin1/new_bsd/src/cvt.h>

static inline uint32_t BITS(uint32_t x, int hi, int lo) {
  return (x >> lo) & ((1u << (hi - lo + 1)) - 1);
}

static inline int32_t signext(uint32_t x, int bits) {
  uint32_t m = 1u << (bits - 1);
  return (int32_t)((x ^ m) - m);
}

static inline int32_t immI(uint32_t inst) {
  return signext(inst >> 20, 12);
}

static inline int32_t immS(uint32_t inst) {
  uint32_t imm = ((inst >> 25) << 5) | ((inst >> 7) & 0x1f);
  return signext(imm, 12);
}

static inline int32_t immB(uint32_t inst) {
  uint32_t imm =
      (((inst >> 31) & 0x1) << 12) |
      (((inst >> 7)  & 0x1) << 11) |
      (((inst >> 25) & 0x3f) << 5) |
      (((inst >> 8)  & 0xf) << 1);
  return signext(imm, 13);
}

static inline int32_t immU(uint32_t inst) {
  return (int32_t)(inst & 0xfffff000u);
}

static inline int32_t immJ(uint32_t inst) {
  uint32_t imm =
      (((inst >> 31) & 0x1) << 20) |
      (((inst >> 12) & 0xff) << 12) |
      (((inst >> 20) & 0x1) << 11) |
      (((inst >> 21) & 0x3ff) << 1);
  return signext(imm, 21);
}


#define VIRTUAL_MEMORY_LENGTH (1024 * 1024 * 1024)  // 4B
#define PHYSICAL_MEMORY_LENGTH (1024 * 1024 * 1024) // 4B

#define INST_EBREAK 0x00100073
#define INST_ECALL 0x00000073
#define INST_MRET 0x30200073
#define INST_SRET 0x10200073
#define INST_WFI 0x10500073
#define INST_NOP 0x00000013




// 中间信号
static wire<BR_TAG_WIDTH> alloc_tag[DECODE_WIDTH]; // 分配的新 Tag

void Idu::init() {
  for (int i = 0; i < MAX_BR_NUM; i++) {
    tag_vec[i] = true;
    tag_vec_1[i] = true;
    br_mask_cp[i] = 0;
    br_mask_cp_1[i] = 0;
  }
  tag_vec[0] = false;
  tag_vec_1[0] = false;
  now_br_mask = 0;
  now_br_mask_1 = 0;
  pending_free_mask = 0;
  pending_free_mask_1 = 0;
  br_latch = {};
}

// 译码并分配 Tag
void Idu::comb_decode() {
  wire<1> alloc_valid[DECODE_WIDTH];
  int alloc_num = 0;
  for (int i = 0; i < MAX_BR_NUM && alloc_num < max_br_per_cycle; i++) {
    if (tag_vec[i]) {
      alloc_tag[alloc_num] = i;
      alloc_valid[alloc_num] = true;
      alloc_num++;
    }
  }
  for (int i = alloc_num; i < DECODE_WIDTH; i++) {
    alloc_tag[i] = 0;
    alloc_valid[i] = false;
  }

  for (int i = 0; i < DECODE_WIDTH; i++) {
    out.dec2ren->valid[i] = false;
    out.dec2ren->uop[i] = {};
  }

  // Assert(in.issue != nullptr && "Idu::comb_decode: issue input is null");
  for (int i = 0; i < DECODE_WIDTH; i++) {
    const InstructionBufferEntry &entry = in.issue->entries[i];
    if (!entry.valid)
      continue;

    out.dec2ren->valid[i] = true;
    InstInfo decoded = {};
    if (entry.page_fault_inst) {
      decoded.diag_val = entry.inst;
      decoded.uop_num = 1;
      decoded.page_fault_inst = true;
      decoded.page_fault_load = false;
      decoded.page_fault_store = false;
      decoded.type = NOP;
      decoded.src1_en = false;
      decoded.src2_en = false;
      decoded.dest_en = false;
      decoded.dbg.instruction = entry.inst;
    } else {
      decode(decoded, entry.inst);
    }
    decoded.dbg.pc = in.issue->pc[i];
    decoded.ftq_idx = entry.ftq_idx;
    decoded.ftq_offset = entry.ftq_offset;
    decoded.ftq_is_last = entry.ftq_is_last;
    out.dec2ren->uop[i] = DecRenIO::DecRenInst::from_inst_info(decoded);
  }

  int br_num = 0;
#ifdef CONFIG_BPU
#else
  // Oracle mode: disable branch-tag resource pressure.
  auto needs_br_tag = [&](InstType) { return false; };
#endif
  // ID 阶段旁路清理：本拍已解析分支的 bit 不应继续传播到新译码指令。
  // clear_mask 来自上拍锁存的 BRU 解析结果（br_latch）。
  wire<BR_MASK_WIDTH> clear = br_latch.clear_mask;
  wire<BR_MASK_WIDTH> running_mask = now_br_mask & ~clear;
  bool stall = false;
  int i = 0;
  for (; i < DECODE_WIDTH; i++) {
    if (!out.dec2ren->valid[i]) {
      out.dec2ren->uop[i].br_id = 0;
      out.dec2ren->uop[i].br_mask = running_mask;
      continue;
    }

    if (is_branch(out.dec2ren->uop[i].type)) {
      if (!alloc_valid[br_num]) {
// #ifdef CONFIG_PERF_COUNTER
//         ctx->perf.idu_tag_stall++;
//         ctx->perf.stall_br_id_cycles++;
// #endif
        stall = true;
        break;
      }
      wire<BR_TAG_WIDTH> new_tag = alloc_tag[br_num];
      out.dec2ren->uop[i].br_id = new_tag;
      // 分支自身不依赖自己；self bit 只作用于后续更年轻指令。
      out.dec2ren->uop[i].br_mask = running_mask;
      running_mask |= (wire<BR_MASK_WIDTH>(1) << new_tag);
      br_num++;
    } else {
      out.dec2ren->uop[i].br_id = 0;
      out.dec2ren->uop[i].br_mask = running_mask;
    }
  }

  if (stall) {
    for (; i < DECODE_WIDTH; i++) {
      out.dec2ren->valid[i] = false;
      out.dec2ren->uop[i].br_id = 0;
      out.dec2ren->uop[i].br_mask = 0;
    }
  }
}

void Idu::comb_branch() {
  // Init next state
  for (int i = 0; i < MAX_BR_NUM; i++) {
    tag_vec_1[i] = tag_vec[i];
    br_mask_cp_1[i] = br_mask_cp[i];
  }
  now_br_mask_1 = now_br_mask;
  pending_free_mask_1 = pending_free_mask;

  // 0. 先应用上拍累积的释放请求（延迟一拍生效）
  wire<BR_MASK_WIDTH> matured_free = pending_free_mask;
  for (int i = 1; i < MAX_BR_NUM; i++) {
    if ((matured_free >> i) & 1) {
      tag_vec_1[i] = true;
      pending_free_mask_1 &= ~(wire<BR_MASK_WIDTH>(1) << i);
    }
  }

  // 1. 处理 clear_mask: 所有已解析的 branch 立即释放 (IDU 本地状态)
  wire<BR_MASK_WIDTH> clear = br_latch.clear_mask;
  for (int i = 1; i < MAX_BR_NUM; i++) {
    if ((clear >> i) & 1) {
      // 延迟到下一拍再真正释放 tag_vec，避免同拍复用
      pending_free_mask_1 |= (wire<BR_MASK_WIDTH>(1) << i);
      now_br_mask_1 &= ~(wire<BR_MASK_WIDTH>(1) << i);
    }
  }

  // 1.5. 全局更新 br_mask_cp：已解析分支的 bit 从所有快照中清除
  //      硬件实现：每个 br_mask_cp 寄存器加一个 AND 门，清除 clear_mask
  //      对应的位 这防止了 tag 被复用后，旧快照仍然"保护"新指令的问题
  if (clear != 0) {
    for (int i = 1; i < MAX_BR_NUM; i++) {
      br_mask_cp_1[i] &= ~clear;
    }
  }

  // 2. 处理误预测
  if (br_latch.mispred) {
    out.dec_bcast->mispred = true;
    out.dec_bcast->br_id = br_latch.br_id;
    out.dec_bcast->redirect_rob_idx = br_latch.redirect_rob_idx;
    out.dec_bcast->br_mask = 1ULL << br_latch.br_id;

    // 释放误预测分支之后分配的更年轻的 tag
    wire<BR_MASK_WIDTH> tags_to_free = now_br_mask & ~br_mask_cp[br_latch.br_id];
    now_br_mask_1 &= ~tags_to_free;
    // 同样延迟到下一拍释放空闲位图
    pending_free_mask_1 |= tags_to_free;
  } else {
    out.dec_bcast->br_mask = 0;
    out.dec_bcast->mispred = false;
    out.dec_bcast->br_id = 0;
  }

  // 广播 clear_mask（包含误预测分支的 bit）
  // 下游模块负责: 先 flush，再对存活条目清除 bit
  out.dec_bcast->clear_mask = clear;
}

void Idu::comb_flush() {
  // Assert(in.rob_bcast != nullptr && "Idu::comb_flush: rob_bcast is null");
  if (in.rob_bcast->flush) {
    for (int i = 1; i < MAX_BR_NUM; i++) {
      tag_vec_1[i] = true;
    }
    tag_vec_1[0] = false;
    now_br_mask_1 = 0;
    pending_free_mask_1 = 0;
  }
}

void Idu::comb_fire() {
  // Assert(in.ren2dec != nullptr && "Idu::comb_fire: ren2dec is null");
  // Assert(in.rob_bcast != nullptr && "Idu::comb_fire: rob_bcast is null");
  if (br_latch.mispred || in.rob_bcast->flush) {
    for (int i = 0; i < DECODE_WIDTH; i++) {
      out.dec2ren->valid[i] = false;
    }
    return;
  }

  int br_num = 0;
#ifdef CONFIG_BPU
#else
  // Oracle mode: no branch-tag allocation in fire path.
  auto needs_br_tag = [&](InstType) { return false; };
#endif
  for (int i = 0; i < DECODE_WIDTH; i++) {
    wire<1> fire = out.dec2ren->valid[i] && in.ren2dec->ready;
    if (fire && is_branch(out.dec2ren->uop[i].type)) {
      wire<BR_TAG_WIDTH> new_tag = alloc_tag[br_num];
      tag_vec_1[new_tag] = false;
      now_br_mask_1 |= (wire<BR_MASK_WIDTH>(1) << new_tag);
      br_mask_cp_1[new_tag] = now_br_mask_1;
      br_num++;
    }
  }

}

void Idu::seq() {
  now_br_mask = now_br_mask_1;
  pending_free_mask = pending_free_mask_1;
  for (int i = 0; i < MAX_BR_NUM; i++) {
    tag_vec[i] = tag_vec_1[i];
    br_mask_cp[i] = br_mask_cp_1[i];
  }

  // Latch Exu Branch Result
  // Assert(in.rob_bcast != nullptr && "Idu::seq: rob_bcast is null");
  // Assert(in.exu2id != nullptr && "Idu::seq: exu2id is null");
  if (!in.rob_bcast->flush) {
    br_latch.mispred = in.exu2id->mispred;
    br_latch.redirect_pc = in.exu2id->redirect_pc;
    br_latch.redirect_rob_idx = in.exu2id->redirect_rob_idx;
    br_latch.br_id = in.exu2id->br_id;
    br_latch.ftq_idx = in.exu2id->ftq_idx;
    br_latch.clear_mask = in.exu2id->clear_mask;
  } else {
    br_latch = {};
  }
}

void Idu::decode(InstInfo &uop, uint32_t inst) {
  // 操作数来源以及type
  // uint32_t imm;
  int uop_num = 1;
  uop.dbg.instruction = inst;
  uop.dbg.difftest_skip = false;

  uint32_t opcode = BITS(inst, 6, 0);
  uint32_t number_funct3_unsigned = BITS(inst, 14, 12);
  uint32_t number_funct7_unsigned = BITS(inst, 31, 25);
  uint32_t reg_d_index = BITS(inst, 11, 7);
  uint32_t reg_a_index = BITS(inst, 19, 15);
  uint32_t reg_b_index = BITS(inst, 24, 20);
  uint32_t csr_idx = inst >> 20;

  // 准备立即数
  uop.diag_val = inst;
  uop.dest_areg = reg_d_index;
  uop.src1_areg = reg_a_index;
  uop.src2_areg = reg_b_index;
  uop.src1_is_pc = false;
  uop.src2_is_imm = true;
  uop.func3 = number_funct3_unsigned;
  uop.func7 = number_funct7_unsigned;
  uop.csr_idx = csr_idx;
  uop.page_fault_inst = false;
  uop.page_fault_load = false;
  uop.page_fault_store = false;
  uop.illegal_inst = false;
  uop.type = NOP;
  uop.tma.is_cache_miss = false;
  uop.tma.is_ret = false;
  uop.tma.mem_commit_is_load = false;
  uop.tma.mem_commit_is_store = false;
  uop.dbg.mem_align_mask = 0;
  static uint64_t global_inst_idx = 0;
  uop.dbg.inst_idx = global_inst_idx++;

  switch (opcode) {
  case number_0_opcode_lui: { // lui
    uop.dest_en = true;
    uop.src1_en = true;
    uop.src1_areg = 0;
    uop.src2_en = false;
    uop.type = ADD;
    uop.func3 = 0;
    uop.imm = immU(inst);
    break;
  }
  case number_1_opcode_auipc: { // auipc
    uop.dest_en = true;
    uop.src1_en = false;
    uop.src2_en = false;
    uop.src1_is_pc = true;
    uop.type = ADD;
    uop.func3 = 0;
    uop.imm = immU(inst);
    break;
  }
  case number_2_opcode_jal: { // jal
    uop_num = 2;              // 前端pre-decode预先解决jal
    uop.dest_en = true;
    uop.src1_en = false;
    uop.src2_en = false;
    uop.src1_is_pc = true;
    uop.src2_is_imm = true;
    uop.func3 = 0;
    uop.type = JAL;
    uop.imm = immJ(inst);
    break;
  }
  case number_3_opcode_jalr: { // jalr
    uop_num = 2;
    uop.dest_en = true;
    uop.src1_en = true;
    uop.src2_en = false;
    uop.src1_is_pc = true;
    uop.src2_is_imm = true;
    uop.func3 = 0;
    uop.type = JALR;
    uop.imm = immI(inst);

    break;
  }
  case number_4_opcode_beq: { // beq, bne, blt, bge, bltu, bgeu
    uop.dest_en = false;
    uop.src1_en = true;
    uop.src2_en = true;
    uop.type = BR;
    uop.imm = immB(inst);
    break;
  }
  case number_5_opcode_lb: { // lb, lh, lw, lbu, lhu
    uop.dest_en = true;
    uop.src1_en = true;
    uop.src2_en = false;
    uop.type = LOAD;
    uop.imm = immI(inst);
    break;
  }
  case number_6_opcode_sb: { // sb, sh, sw
    uop_num = 2;
    uop.dest_en = false;
    uop.src1_en = true;
    uop.src2_en = true;
    uop.type = STORE;
    uop.imm = immS(inst);
    break;
  }
  case number_7_opcode_addi: { // addi, slti, sltiu, xori, ori, andi, slli,
    // srli, srai, AND Zbb/Zbs imm extensions (clz, ctz, pcnt, sext, bseti,
    // bclri, binvi)
    uop.dest_en = true;
    uop.src1_en = true;
    uop.src2_en = false;
    uop.type = ADD;
    uop.imm = immI(inst);
    break;
  }
  case number_8_opcode_add: { // add, sub, sll, slt, sltu, xor, srl, sra, or,
    // AND Zba/Zbb/Zbc/Zbs extensions (sh1add, clmul, xnor, pack, min, max, etc)
    uop.dest_en = true;
    uop.src1_en = true;
    uop.src2_en = true;
    uop.src2_is_imm = false;
    if (number_funct7_unsigned == 1) { // mul div
      if (number_funct3_unsigned & 0b100) {
        uop.type = DIV;
      } else {
        uop.type = MUL;
      }
    } else {
      uop.type = ADD;
    }
    break;
  }
  case number_9_opcode_fence: { // fence, fence.i
    uop.dest_en = false;
    uop.src1_en = false;
    uop.src2_en = false;

    // Check funct3 for FENCE.I (001)
    if (number_funct3_unsigned == 0b001) {
      uop.type = FENCE_I; // Strict separation
    } else {
      uop.type = NOP; // Ordinary FENCE is NOP
    }
    break;
  }
  case number_10_opcode_ecall: { // ecall, ebreak, csrrw, csrrs, csrrc,
                                 // csrrwi, csrrsi, csrrci
    uop.src2_is_imm =
        number_funct3_unsigned & 0b100 &&
        (number_funct3_unsigned & 0b001 || number_funct3_unsigned & 0b010);

    if (number_funct3_unsigned & 0b001 || number_funct3_unsigned & 0b010) {
      if (csr_idx != number_mtvec && csr_idx != number_mepc &&
          csr_idx != number_mcause && csr_idx != number_mie &&
          csr_idx != number_mip && csr_idx != number_mtval &&
          csr_idx != number_mscratch && csr_idx != number_mstatus &&
          csr_idx != number_mideleg && csr_idx != number_medeleg &&
          csr_idx != number_sepc && csr_idx != number_stvec &&
          csr_idx != number_scause && csr_idx != number_sscratch &&
          csr_idx != number_stval && csr_idx != number_sstatus &&
          csr_idx != number_sie && csr_idx != number_sip &&
          csr_idx != number_satp && csr_idx != number_mhartid &&
          csr_idx != number_misa) {
        uop.type = NOP;
        uop.dest_en = false;
        uop.src1_en = false;
        uop.src2_en = false;

        if (csr_idx == number_time || csr_idx == number_timeh)
          uop.illegal_inst = true;

      } else {
        uop.type = CSR;
        uop.dest_en = true;
        uop.src1_en = true;
        uop.src2_en = !uop.src2_is_imm;
        uop.imm = reg_a_index;
      }
    } else {
      uop.dest_en = false;
      uop.src1_en = false;
      uop.src2_en = false;

      if (inst == INST_ECALL) {
        uop.type = ECALL;
      } else if (inst == INST_EBREAK) {
        uop.type = EBREAK;
      } else if (inst == INST_MRET) {
        uop.type = MRET;
      } else if (inst == INST_WFI) {
        uop.type = WFI;
      } else if (inst == INST_SRET) {
        uop.type = SRET;
      } else if (number_funct7_unsigned == 0b0001001 &&
                 number_funct3_unsigned == 0 && reg_d_index == 0) {
        uop.type = SFENCE_VMA;
        uop.src1_en = true;
        uop.src2_en = true;
      } else {
        uop.type = NOP;
        /*uop[0].illegal_inst = true;*/
        /*cout << hex << inst << endl;*/
        /*Assert(0);*/
      }
    }
    break;
  }

  case number_11_opcode_lrw: {
    uop_num = 3;
    uop.dest_en = true;
    uop.src1_en = true;
    uop.src2_en = true;
    uop.imm = 0;
    uop.type = AMO;
    uop.is_atomic = true;

    if ((number_funct7_unsigned >> 2) == AmoOp::LR) {
      uop_num = 1;
      uop.src2_en = false;
    }

    break;
  }

  case number_12_opcode_float: {
    uop.dest_en = true;
    uop.src1_en = true;
    uop.src2_en = true;
    uop.src2_is_imm = false;
    uop.type = FP;
    break;
  }

  default: {
    uop.dest_en = false;
    uop.src1_en = false;
    uop.src2_en = false;
    uop.type = NOP;
    uop.illegal_inst = true;
    break;
  }
  }

  uop.uop_num = uop_num;
  uop.tma.is_ret =
      (uop.type == JALR && uop.src1_areg == 1 && uop.dest_areg == 0 &&
       uop.imm == 0);
  uop.tma.mem_commit_is_load =
      (uop.type == LOAD || (uop.type == AMO && (uop.func7 >> 2) != AmoOp::SC));
  uop.tma.mem_commit_is_store =
      (uop.type == STORE || (uop.type == AMO && (uop.func7 >> 2) != AmoOp::LR));
  if (uop.tma.mem_commit_is_load) {
    uop.dbg.mem_align_mask =
        (uop.func3 & 0x3) == 0   ? 0
        : (uop.func3 & 0x3) == 1 ? 1
                                 : 3;
  }

  if (uop.type == AMO && uop.dest_areg == 0 && (uop.func7 >> 2) != AmoOp::LR &&
      (uop.func7 >> 2) != AmoOp::SC) {
    uop.dest_areg = 32;
  }

  if (uop.dest_areg == 0)
    uop.dest_en = false;
}

template <typename T>
inline T pack_bits(const bool* cursor, int width) {
    T val = 0;
    // 编译器会自动展开这个循环，对于 width=1, 4, 32 等常数非常快
    for (int i = 0; i < width; i++) {
        // 使用 | 而不是 +=，且不需要 if 判断，利用 bool 为 0/1 的特性
        val |= (static_cast<T>(cursor[i]) << i);
    }
    return val;
}
// 辅助函数：将整数 val 的低 width 位拆解为 bool 写入 cursor 指向的内存
// cursor 会自动向后移动 width 步
template <typename T>
inline void unpack_bits(bool* cursor, T val, int width) {
    // 编译器会自动展开这个循环 (Loop Unrolling)
    for (int i = 0; i < width; i++) {
        // (val >> i) & 1 取出第 i 位，直接赋值给 bool
        // 相比于原始代码的 (val & (1<<i)) != 0，这种写法对 CPU 流水线更友好
        cursor[i] = (val >> i) & 1;
    }
}
// class MismatchLogger {
//     public:
//         static MismatchLogger& getInstance() {
//             static MismatchLogger instance;
//             return instance;
//         }
//         void log(const bool* pi, int pi_size) {
//             if (lines_written_ >= MAX_LOG_LINES) {
//                 return;
//             }
//             if (!file_opened_) {
//                 log_file_.open(MISMATCH_LOG_PATH);
//                 file_opened_ = true;
//                 if (!log_file_.is_open()) {
//                     std::cerr << "\n[ERROR] 无法打开或创建日志文件: " << MISMATCH_LOG_PATH
//                               << "\n        请检查目标文件夹是否存在！\n\n";
//                     return; // 打开失败，直接返回
//                 }
//             }
//             if (log_file_.is_open()) {
//                 for (int j = 0; j < pi_size; ++j) {
//                     log_file_ << pi[j];
//                 }
//                 log_file_ << "\n";
//                 lines_written_++;
//                 if (lines_written_ >= MAX_LOG_LINES) {
//                     log_file_.close();
//                 }
//             }
//         }
//     private:
//         MismatchLogger() : lines_written_(0), file_opened_(false) {}
//         ~MismatchLogger() {
//             if (log_file_.is_open()) {
//                 log_file_.close();
//             }
//         }
//         std::ofstream log_file_;
//         int lines_written_;
//         bool file_opened_;
//         const int MAX_LOG_LINES = 1000;//在此处修改保留错误数量 
// };

// template <typename T>
// void compare_and_pack(bool*& cursor_po, T actual_val, int bit_width, const std::string& var_name, const bool* pi, int pi_size) {
//     bool mismatch_found = false;
//     unsigned char raw_actual = *reinterpret_cast<unsigned char*>(&actual_val);
//     unsigned char safe_actual_val = (raw_actual >= 1) ? 1 : 0;
//     for (int i = 0; i < bit_width; ++i) {
//         unsigned char clean_actual_bit = (safe_actual_val >> i) & 1;
//         bool actual_bit = (actual_val >> i) & 1;
//         bool predicted_bit = cursor_po[i];
//         if(bit_width == 1) {
//             if (clean_actual_bit != predicted_bit) {
//                 mismatch_found = true;
//             }
//         }
//         else {
//             if (actual_bit != predicted_bit) {
//                 mismatch_found = true;
//             }
//         }
//     }
//     if (mismatch_found) {
//         MismatchLogger::getInstance().log(pi, pi_size);
//     }
//     cursor_po += bit_width;
// }
void Idu::pi_to_simulator(bool* pi) {
    const bool* cursor = pi;
    for(int i0 = 0; i0 < 8; i0++) {
        in.issue->entries[i0].valid = pack_bits<bool>(cursor, 1); //0
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        in.issue->entries[i0].inst = pack_bits<uint32_t>(cursor, 32); //8
        cursor += 32;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        in.issue->entries[i0].page_fault_inst = pack_bits<bool>(cursor, 1); //264
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        in.issue->entries[i0].ftq_idx = pack_bits<uint8_t>(cursor, 6); //272
        cursor += 6;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        in.issue->entries[i0].ftq_offset = pack_bits<uint8_t>(cursor, 4); //320
        cursor += 4;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        in.issue->entries[i0].ftq_is_last = pack_bits<bool>(cursor, 1); //352
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        in.issue->pc[i0] = pack_bits<uint32_t>(cursor, 32); //360
        cursor += 32;
    }
    in.ren2dec->ready = pack_bits<bool>(cursor, 1); //616
    cursor += 1;
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); //617
    cursor += 1;
    in.rob_bcast->mret = pack_bits<bool>(cursor, 1); //618
    cursor += 1;
    in.rob_bcast->sret = pack_bits<bool>(cursor, 1); //619
    cursor += 1;
    in.rob_bcast->ecall = pack_bits<bool>(cursor, 1); //620
    cursor += 1;
    in.rob_bcast->exception = pack_bits<bool>(cursor, 1); //621
    cursor += 1;
    in.rob_bcast->fence = pack_bits<bool>(cursor, 1); //622
    cursor += 1;
    in.rob_bcast->fence_i = pack_bits<bool>(cursor, 1); //623
    cursor += 1;
    in.rob_bcast->page_fault_inst = pack_bits<bool>(cursor, 1); //624
    cursor += 1;
    in.rob_bcast->page_fault_load = pack_bits<bool>(cursor, 1); //625
    cursor += 1;
    in.rob_bcast->page_fault_store = pack_bits<bool>(cursor, 1); //626
    cursor += 1;
    in.rob_bcast->illegal_inst = pack_bits<bool>(cursor, 1); //627
    cursor += 1;
    in.rob_bcast->interrupt = pack_bits<bool>(cursor, 1); //628
    cursor += 1;
    in.rob_bcast->trap_val = pack_bits<uint32_t>(cursor, 32); //629
    cursor += 32;
    in.rob_bcast->pc = pack_bits<uint32_t>(cursor, 32); //661
    cursor += 32;
    in.exu2id->mispred = pack_bits<bool>(cursor, 1); //693
    cursor += 1;
    in.exu2id->redirect_pc = pack_bits<uint32_t>(cursor, 32); //694
    cursor += 32;
    in.exu2id->redirect_rob_idx = pack_bits<uint8_t>(cursor, 7); //726
    cursor += 7;
    in.exu2id->br_id = pack_bits<uint8_t>(cursor, 6); //733
    cursor += 6;
    in.exu2id->ftq_idx = pack_bits<uint8_t>(cursor, 6); //739
    cursor += 6;
    in.exu2id->clear_mask = pack_bits<uint64_t>(cursor, 64); //745
    cursor += 64;
    now_br_mask = pack_bits<uint64_t>(cursor, 64); //809
    cursor += 64;
    for(int i0 = 0; i0 < 64; i0++) {
        br_mask_cp[i0] = pack_bits<uint64_t>(cursor, 64); //873
        cursor += 64;
    }
    pending_free_mask = pack_bits<uint64_t>(cursor, 64); //4969
    cursor += 64;
    for(int i0 = 0; i0 < 64; i0++) {
        tag_vec[i0] = pack_bits<bool>(cursor, 1); //5033
        cursor += 1;
    }
}
void Idu::out_initial_detect() {
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].diag_val != 0) {
            std::cout << "out.dec2ren->uop[i0].diag_val error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].dest_areg != 0) {
            std::cout << "out.dec2ren->uop[i0].dest_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].src1_areg != 0) {
            std::cout << "out.dec2ren->uop[i0].src1_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].src2_areg != 0) {
            std::cout << "out.dec2ren->uop[i0].src2_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].ftq_idx != 0) {
            std::cout << "out.dec2ren->uop[i0].ftq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].ftq_offset != 0) {
            std::cout << "out.dec2ren->uop[i0].ftq_offset error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].ftq_is_last != 0) {
            std::cout << "out.dec2ren->uop[i0].ftq_is_last error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].dest_en != 0) {
            std::cout << "out.dec2ren->uop[i0].dest_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].src1_en != 0) {
            std::cout << "out.dec2ren->uop[i0].src1_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].src2_en != 0) {
            std::cout << "out.dec2ren->uop[i0].src2_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].is_atomic != 0) {
            std::cout << "out.dec2ren->uop[i0].is_atomic error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].src1_is_pc != 0) {
            std::cout << "out.dec2ren->uop[i0].src1_is_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].src2_is_imm != 0) {
            std::cout << "out.dec2ren->uop[i0].src2_is_imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].func3 != 0) {
            std::cout << "out.dec2ren->uop[i0].func3 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].func7 != 0) {
            std::cout << "out.dec2ren->uop[i0].func7 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].imm != 0) {
            std::cout << "out.dec2ren->uop[i0].imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].br_id != 0) {
            std::cout << "out.dec2ren->uop[i0].br_id error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].br_mask != 0) {
            std::cout << "out.dec2ren->uop[i0].br_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].csr_idx != 0) {
            std::cout << "out.dec2ren->uop[i0].csr_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].uop_num != 0) {
            std::cout << "out.dec2ren->uop[i0].uop_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].page_fault_inst != 0) {
            std::cout << "out.dec2ren->uop[i0].page_fault_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].illegal_inst != 0) {
            std::cout << "out.dec2ren->uop[i0].illegal_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->uop[i0].type != 0) {
            std::cout << "out.dec2ren->uop[i0].type error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 8; i0++) {
        if(out.dec2ren->valid[i0] != 0) {
            std::cout << "out.dec2ren->valid[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
    if(out.dec_bcast->mispred != 0) {
        std::cout << "out.dec_bcast->mispred error, not 0!" << std::endl;
        exit(1);
    }
    if(out.dec_bcast->br_mask != 0) {
        std::cout << "out.dec_bcast->br_mask error, not 0!" << std::endl;
        exit(1);
    }
    if(out.dec_bcast->br_id != 0) {
        std::cout << "out.dec_bcast->br_id error, not 0!" << std::endl;
        exit(1);
    }
    if(out.dec_bcast->redirect_rob_idx != 0) {
        std::cout << "out.dec_bcast->redirect_rob_idx error, not 0!" << std::endl;
        exit(1);
    }
    if(out.dec_bcast->clear_mask != 0) {
        std::cout << "out.dec_bcast->clear_mask error, not 0!" << std::endl;
        exit(1);
    }
    if(now_br_mask_1 != 0) {
        std::cout << "now_br_mask_1 error, not 0!" << std::endl;
        exit(1);
    }
    for(int i0 = 0; i0 < 64; i0++) {
        if(br_mask_cp_1[i0] != 0) {
            std::cout << "br_mask_cp_1[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
    if(pending_free_mask_1 != 0) {
        std::cout << "pending_free_mask_1 error, not 0!" << std::endl;
        exit(1);
    }
    for(int i0 = 0; i0 < 64; i0++) {
        if(tag_vec_1[i0] != 0) {
            std::cout << "tag_vec_1[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
}
void Idu::simulator_to_po(bool* po) {
    bool* cursor = po;
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].diag_val, 32); //0
        cursor += 32;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].dest_areg, 6); //256
        cursor += 6;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].src1_areg, 6); //304
        cursor += 6;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].src2_areg, 6); //352
        cursor += 6;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].ftq_idx, 6); //400
        cursor += 6;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].ftq_offset, 4); //448
        cursor += 4;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].ftq_is_last, 1); //480
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].dest_en, 1); //488
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].src1_en, 1); //496
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].src2_en, 1); //504
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].is_atomic, 1); //512
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].src1_is_pc, 1); //520
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].src2_is_imm, 1); //528
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].func3, 3); //536
        cursor += 3;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].func7, 7); //560
        cursor += 7;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].imm, 32); //616
        cursor += 32;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].br_id, 6); //872
        cursor += 6;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].br_mask, 64); //920
        cursor += 64;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].csr_idx, 12); //1432
        cursor += 12;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].uop_num, 2); //1528
        cursor += 2;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].page_fault_inst, 1); //1544
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].illegal_inst, 1); //1552
        cursor += 1;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->uop[i0].type, 5); //1560
        cursor += 5;
    }
    for(int i0 = 0; i0 < 8; i0++) {
        unpack_bits(cursor, out.dec2ren->valid[i0], 1); //1600
        cursor += 1;
    }
    unpack_bits(cursor, out.dec_bcast->mispred, 1); //1608
    cursor += 1;
    unpack_bits(cursor, out.dec_bcast->br_mask, 64); //1609
    cursor += 64;
    unpack_bits(cursor, out.dec_bcast->br_id, 6); //1673
    cursor += 6;
    unpack_bits(cursor, out.dec_bcast->redirect_rob_idx, 7); //1679
    cursor += 7;
    unpack_bits(cursor, out.dec_bcast->clear_mask, 64); //1686
    cursor += 64;
    unpack_bits(cursor, now_br_mask_1, 64); //1750
    cursor += 64;
    for(int i0 = 0; i0 < 64; i0++) {
        unpack_bits(cursor, br_mask_cp_1[i0], 64); //1814
        cursor += 64;
    }
    unpack_bits(cursor, pending_free_mask_1, 64); //5910
    cursor += 64;
    for(int i0 = 0; i0 < 64; i0++) {
        unpack_bits(cursor, tag_vec_1[i0], 1); //5974
        cursor += 1;
    }
}
// void Idu::simulator_with_bsd() {
//     bool pi [5097];
//     bool po [6038];
//     bool* cursor_pi = pi;
//     bool* cursor_po = po;
//     for(int i0 = 0; i0 < 8; i0++) {
//         unpack_bits(cursor_pi, in.issue->entries[i0].valid, 1); //0
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         unpack_bits(cursor_pi, in.issue->entries[i0].inst, 32); //8
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         unpack_bits(cursor_pi, in.issue->entries[i0].page_fault_inst, 1); //264
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         unpack_bits(cursor_pi, in.issue->entries[i0].ftq_idx, 6); //272
//         cursor_pi += 6;
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         unpack_bits(cursor_pi, in.issue->entries[i0].ftq_offset, 4); //320
//         cursor_pi += 4;
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         unpack_bits(cursor_pi, in.issue->entries[i0].ftq_is_last, 1); //352
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         unpack_bits(cursor_pi, in.issue->pc[i0], 32); //360
//         cursor_pi += 32;
//     }
//     unpack_bits(cursor_pi, in.ren2dec->ready, 1); //616
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->flush, 1); //617
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->mret, 1); //618
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->sret, 1); //619
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->ecall, 1); //620
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->exception, 1); //621
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->fence, 1); //622
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->fence_i, 1); //623
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_inst, 1); //624
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_load, 1); //625
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_store, 1); //626
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->illegal_inst, 1); //627
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->interrupt, 1); //628
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->trap_val, 32); //629
//     cursor_pi += 32;
//     unpack_bits(cursor_pi, in.rob_bcast->pc, 32); //661
//     cursor_pi += 32;
//     unpack_bits(cursor_pi, in.exu2id->mispred, 1); //693
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.exu2id->redirect_pc, 32); //694
//     cursor_pi += 32;
//     unpack_bits(cursor_pi, in.exu2id->redirect_rob_idx, 7); //726
//     cursor_pi += 7;
//     unpack_bits(cursor_pi, in.exu2id->br_id, 6); //733
//     cursor_pi += 6;
//     unpack_bits(cursor_pi, in.exu2id->ftq_idx, 6); //739
//     cursor_pi += 6;
//     unpack_bits(cursor_pi, in.exu2id->clear_mask, 64); //745
//     cursor_pi += 64;
//     unpack_bits(cursor_pi, now_br_mask, 64); //809
//     cursor_pi += 64;
//     for(int i0 = 0; i0 < 64; i0++) {
//         unpack_bits(cursor_pi, br_mask_cp[i0], 64); //873
//         cursor_pi += 64;
//     }
//     unpack_bits(cursor_pi, pending_free_mask, 64); //4969
//     cursor_pi += 64;
//     for(int i0 = 0; i0 < 64; i0++) {
//         unpack_bits(cursor_pi, tag_vec[i0], 1); //5033
//         cursor_pi += 1;
//     }
//     #ifndef BEGIN_IDX
//     #define BEGIN_IDX 0
//     #endif
//     cursor_po = cursor_po + BEGIN_IDX;
//     io_generator_outer(pi, po);
//     #ifndef CHECK_HEADER
//     #define CHECK_HEADER "check_layer_0_0.h"
//     #endif
//     #include CHECK_HEADER
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].diag_val, 32, "out.dec2ren->uop[i0].diag_val", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].dest_areg, 6, "out.dec2ren->uop[i0].dest_areg", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].src1_areg, 6, "out.dec2ren->uop[i0].src1_areg", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].src2_areg, 6, "out.dec2ren->uop[i0].src2_areg", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].ftq_idx, 6, "out.dec2ren->uop[i0].ftq_idx", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].ftq_offset, 4, "out.dec2ren->uop[i0].ftq_offset", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].ftq_is_last, 1, "out.dec2ren->uop[i0].ftq_is_last", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].dest_en, 1, "out.dec2ren->uop[i0].dest_en", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].src1_en, 1, "out.dec2ren->uop[i0].src1_en", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].src2_en, 1, "out.dec2ren->uop[i0].src2_en", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].is_atomic, 1, "out.dec2ren->uop[i0].is_atomic", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].src1_is_pc, 1, "out.dec2ren->uop[i0].src1_is_pc", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].src2_is_imm, 1, "out.dec2ren->uop[i0].src2_is_imm", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].func3, 3, "out.dec2ren->uop[i0].func3", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].func7, 7, "out.dec2ren->uop[i0].func7", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].imm, 32, "out.dec2ren->uop[i0].imm", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].br_id, 6, "out.dec2ren->uop[i0].br_id", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].br_mask, 64, "out.dec2ren->uop[i0].br_mask", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].csr_idx, 12, "out.dec2ren->uop[i0].csr_idx", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].uop_num, 2, "out.dec2ren->uop[i0].uop_num", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].page_fault_inst, 1, "out.dec2ren->uop[i0].page_fault_inst", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].illegal_inst, 1, "out.dec2ren->uop[i0].illegal_inst", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->uop[i0].type, 5, "out.dec2ren->uop[i0].type", pi, 5097);
//     }
//     for(int i0 = 0; i0 < 8; i0++) {
//         compare_and_pack(cursor_po, out.dec2ren->valid[i0], 1, "out.dec2ren->valid[i0]", pi, 5097);
//     }
//     compare_and_pack(cursor_po, out.dec_bcast->mispred, 1, "out.dec_bcast->mispred", pi, 5097);
//     compare_and_pack(cursor_po, out.dec_bcast->br_mask, 64, "out.dec_bcast->br_mask", pi, 5097);
//     compare_and_pack(cursor_po, out.dec_bcast->br_id, 6, "out.dec_bcast->br_id", pi, 5097);
//     compare_and_pack(cursor_po, out.dec_bcast->redirect_rob_idx, 7, "out.dec_bcast->redirect_rob_idx", pi, 5097);
//     compare_and_pack(cursor_po, out.dec_bcast->clear_mask, 64, "out.dec_bcast->clear_mask", pi, 5097);
//     compare_and_pack(cursor_po, now_br_mask_1, 64, "now_br_mask_1", pi, 5097);
//     for(int i0 = 0; i0 < 64; i0++) {
//         compare_and_pack(cursor_po, br_mask_cp_1[i0], 64, "br_mask_cp_1[i0]", pi, 5097);
//     }
//     compare_and_pack(cursor_po, pending_free_mask_1, 64, "pending_free_mask_1", pi, 5097);
//     for(int i0 = 0; i0 < 64; i0++) {
//         compare_and_pack(cursor_po, tag_vec_1[i0], 1, "tag_vec_1[i0]", pi, 5097);
//     }
// }
#endif