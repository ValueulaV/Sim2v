#ifndef PRF_CPP_H
#define PRF_CPP_H
#include "Prf.h"
#include "IO.h"
#include "config.h"
#include "util.h"
#include <cstring>
#include <fstream>
#include <iostream>

// static const char *const MISMATCH_LOG_PATH = "prf_mismatch.log";

namespace {
static inline bool is_killed(const ExePrfIO::ExePrfWbUop &uop, const DecBroadcastIO *db) {
  if (!db->mispred) return false;
  return (uop.br_mask & db->br_mask) != 0;
}
template <typename T1, typename T2>
inline uint32_t read_operand_with_bypass(
    uint32_t preg, bool src_en, const reg<32> reg_file_data, const ExePrfIO::ExePrfEntry *inst_r,
    const ExePrfIO *exe2prf, const T1 &writeb_equal_row,
    const T2 &bypass_equal_row) {
  if (!src_en) {
    return 0;
  }

  // uint32_t data = reg_file[preg];
  uint32_t data = reg_file_data;

  // 写回级旁路：优先于寄存器堆读值。
  for (int j = 0; j < ISSUE_WIDTH; j++) {
    // if (inst_r[j].valid && inst_r[j].uop.dest_en && inst_r[j].uop.dest_preg == preg) {
    if (inst_r[j].valid && inst_r[j].uop.dest_en && writeb_equal_row[j]) {
      data = inst_r[j].uop.result;
    }
  }

  // Exu 广播旁路：同拍 FU 结果可直接使用。
  for (int k = 0; k < TOTAL_FU_COUNT; k++) {
    if (exe2prf->bypass[k].valid) {
      const auto &bypass_uop = exe2prf->bypass[k].uop;
      // if (bypass_uop.dest_en && bypass_uop.dest_preg == preg) {
        if (bypass_uop.dest_en && bypass_equal_row[k]) {
        data = bypass_uop.result;
        break;
      }
    }
  }

  return data;
}
static inline bool is_load_wb(const ExePrfIO::ExePrfWbUop &uop) {
    return decode_uop_type(uop.op) == UOP_LOAD;
}
} // namespace

void Prf::init() {
  // for (int i = 0; i < PRF_NUM; i++) {
  //   reg_file[i] = 0;
  //   reg_file_1[i] = 0;
  // }
  // for (int i = 0; i < ISSUE_WIDTH; i++) {
  //   inst_r[i] = {};
  //   inst_r_1[i] = {};
  // }
  for (int i = 0; i < ISSUE_WIDTH; i++) {
    out.reg_file_addr_0[i] = in.iss2prf->iss_entry[i].uop.src1_preg;
    out.reg_file_addr_1[i] = in.iss2prf->iss_entry[i].uop.src2_preg;
    for(int j = 0; j < ISSUE_WIDTH; j++) {
      out.writeb_equal_logic_out_1[i][j] = inst_r[j].uop.dest_preg == in.iss2prf->iss_entry[i].uop.src1_preg;
      out.writeb_equal_logic_out_2[i][j] = inst_r[j].uop.dest_preg == in.iss2prf->iss_entry[i].uop.src2_preg;
    }
    for(int j = 0; j < TOTAL_FU_COUNT; j++) {
      out.bypass_equal_logic_out_1[i][j] = in.exe2prf->bypass[j].uop.dest_preg == in.iss2prf->iss_entry[i].uop.src1_preg;
      out.bypass_equal_logic_out_2[i][j] = in.exe2prf->bypass[j].uop.dest_preg == in.iss2prf->iss_entry[i].uop.src2_preg;
    }
  }
}

// ==========================================
// 1. 寄存器读取（发射前）+ 旁路
// ==========================================
void Prf::comb_read() {
  for (int i = 0; i < ISSUE_WIDTH; i++) {
    out.prf2exe->iss_entry[i].valid = in.iss2prf->iss_entry[i].valid;
    if (!out.prf2exe->iss_entry[i].valid)
      continue;

    auto &entry = out.prf2exe->iss_entry[i];
    entry.uop = PrfExeIO::PrfExeUop::from_iss_prf_uop(in.iss2prf->iss_entry[i].uop);
    entry.uop.src1_rdata =
        read_operand_with_bypass(entry.uop.src1_preg, entry.uop.src1_en,
                                //  reg_file, inst_r, in.exe2prf,
                                 in.reg_file_data_0[i], inst_r, in.exe2prf,
                                 in.writeb_equal_give_in_1[i],
                                 in.bypass_equal_give_in_1[i]);
    entry.uop.src2_rdata =
        read_operand_with_bypass(entry.uop.src2_preg, entry.uop.src2_en,
                                //  reg_file, inst_r, in.exe2prf,
                                 in.reg_file_data_1[i], inst_r, in.exe2prf,
                                 in.writeb_equal_give_in_2[i],
                                 in.bypass_equal_give_in_2[i]);
  }
}

// ==========================================
// 2. 唤醒逻辑
// ==========================================
void Prf::comb_awake() {
  for (int i = 0; i < LSU_LOAD_WB_WIDTH; i++) {
    out.prf_awake->wake[i].valid = false;
  }

  int awake_idx = 0;
  // 遍历寻找有效的 Load 唤醒 (支持多端口)
  for (int i = 0; i < ISSUE_WIDTH; i++) {
    if (inst_r[i].valid && inst_r[i].uop.dest_en && is_load_wb(inst_r[i].uop)) {
      bool is_squashed = is_killed(inst_r[i].uop, in.dec_bcast);
    // bool is_squashed = in.is_killed_0[i];
      if (is_squashed) {
        continue;
      }

      if (awake_idx < LSU_LOAD_WB_WIDTH) {
        out.prf_awake->wake[awake_idx].valid = true;
        out.prf_awake->wake[awake_idx].preg = inst_r[i].uop.dest_preg;
        awake_idx++;
      }
    }
  }
}

void Prf::comb_complete() {
  // 保留接口：当前 PRF 不承载额外 complete 组合逻辑。
}

// ==========================================
// 3. 写物理寄存器
// ==========================================
void Prf::comb_write() {
  // 将写回级结果写入寄存器堆，x0 始终保持为 0。
  for (int i = 0; i < ISSUE_WIDTH; i++) {
    if (inst_r[i].valid && inst_r[i].uop.dest_en && inst_r[i].uop.dest_preg != 0) {
      reg_file_1[inst_r[i].uop.dest_preg] = inst_r[i].uop.result;
      out.reg_file_2_en[i] = 1;
      out.reg_file_2_addr[i] = inst_r[i].uop.dest_preg;
      out.reg_file_2_data[i] = inst_r[i].uop.result;
      if (out.reg_file_2_addr[i] == 0){
        out.reg_file_2_data[i] = 0;
      }
    }
  }
  reg_file_1[0] = 0;
}

// ==========================================
// 4. 流水寄存器更新
// ==========================================
void Prf::comb_pipeline() {
  bool global_flush = in.rob_bcast->flush;
  wire<BR_MASK_WIDTH> clear = in.dec_bcast->clear_mask;
  for (int i = 0; i < ISSUE_WIDTH; i++) {
    if (global_flush) { // 5431
      inst_r_1[i].valid = false;
    } else if (in.exe2prf->entry[i].valid) { // 2292
      inst_r_1[i].valid = true;
      inst_r_1[i].uop = in.exe2prf->entry[i].uop;
      if (is_killed(inst_r_1[i].uop, in.dec_bcast)) { // 5289
    //   if (in.is_killed_1[i]) {
        inst_r_1[i].valid = false;
      } else if (inst_r_1[i].valid && clear) {
        inst_r_1[i].uop.br_mask &= ~clear;
      }
    } else {
      inst_r_1[i].valid = false;
    }
  }
}

void Prf::seq() {
  for (int i = 0; i < PRF_NUM; i++) {
    reg_file[i] = reg_file_1[i];
  }
  reg_file[0] = 0;
  reg_file_1[0] = 0;

  for (int i = 0; i < ISSUE_WIDTH; i++) {
    inst_r[i] = inst_r_1[i];
  }
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

void Prf::pi_to_simulator(bool* pi) {
    const bool* cursor = pi;
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].valid = pack_bits<bool>(cursor, 1); //0
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.dest_preg = pack_bits<uint8_t>(cursor, 8); //12
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.src1_preg = pack_bits<uint8_t>(cursor, 8); //108
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.src2_preg = pack_bits<uint8_t>(cursor, 8); //204
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.ftq_idx = pack_bits<uint8_t>(cursor, 6); //300
        cursor += 6;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.ftq_offset = pack_bits<uint8_t>(cursor, 4); //372
        cursor += 4;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.is_atomic = pack_bits<bool>(cursor, 1); //420
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.dest_en = pack_bits<bool>(cursor, 1); //432
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.src1_en = pack_bits<bool>(cursor, 1); //444
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.src2_en = pack_bits<bool>(cursor, 1); //456
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //468
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //480
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.func3 = pack_bits<uint8_t>(cursor, 3); //492
        cursor += 3;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.func7 = pack_bits<uint8_t>(cursor, 7); //528
        cursor += 7;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.imm = pack_bits<uint32_t>(cursor, 32); //612
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.br_id = pack_bits<uint8_t>(cursor, 6); //996
        cursor += 6;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.br_mask = pack_bits<uint64_t>(cursor, 64); //1068
        cursor += 64;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //1836
        cursor += 12;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.rob_idx = pack_bits<uint8_t>(cursor, 7); //1980
        cursor += 7;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.stq_idx = pack_bits<uint8_t>(cursor, 6); //2064
        cursor += 6;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.stq_flag = pack_bits<bool>(cursor, 1); //2136
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.ldq_idx = pack_bits<uint8_t>(cursor, 6); //2148
        cursor += 6;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.rob_flag = pack_bits<bool>(cursor, 1); //2220
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.iss2prf->iss_entry[i0].uop.op = decode_uop_type(pack_bits<uint8_t>(cursor, 5)); //2232
        cursor += 5;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.exe2prf->entry[i0].valid = pack_bits<bool>(cursor, 1); //2292
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.exe2prf->entry[i0].uop.dest_preg = pack_bits<uint8_t>(cursor, 8); //2304
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.exe2prf->entry[i0].uop.result = pack_bits<uint32_t>(cursor, 32); //2400
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.exe2prf->entry[i0].uop.br_mask = pack_bits<uint64_t>(cursor, 64); //2784
        cursor += 64;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.exe2prf->entry[i0].uop.dest_en = pack_bits<bool>(cursor, 1); //3552
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.exe2prf->entry[i0].uop.op = decode_uop_type(pack_bits<uint8_t>(cursor, 5)); //3564
        cursor += 5;
    }
    for(int i0 = 0; i0 < 16; i0++) {
        in.exe2prf->bypass[i0].valid = pack_bits<bool>(cursor, 1); //3624
        cursor += 1;
    }
    for(int i0 = 0; i0 < 16; i0++) {
        in.exe2prf->bypass[i0].uop.dest_preg = pack_bits<uint8_t>(cursor, 8); //3640
        cursor += 8;
    }
    for(int i0 = 0; i0 < 16; i0++) {
        in.exe2prf->bypass[i0].uop.result = pack_bits<uint32_t>(cursor, 32); //3768
        cursor += 32;
    }
    for(int i0 = 0; i0 < 16; i0++) {
        in.exe2prf->bypass[i0].uop.br_mask = pack_bits<uint64_t>(cursor, 64); //4280
        cursor += 64;
    }
    for(int i0 = 0; i0 < 16; i0++) {
        in.exe2prf->bypass[i0].uop.dest_en = pack_bits<bool>(cursor, 1); //5304
        cursor += 1;
    }
    for(int i0 = 0; i0 < 16; i0++) {
        in.exe2prf->bypass[i0].uop.op = decode_uop_type(pack_bits<uint8_t>(cursor, 5)); //5320
        cursor += 5;
    }
    in.dec_bcast->mispred = pack_bits<bool>(cursor, 1); //5400
    cursor += 1;
    in.dec_bcast->br_mask = pack_bits<uint64_t>(cursor, 64); //5401
    cursor += 64;
    in.dec_bcast->br_id = pack_bits<uint8_t>(cursor, 6); //5465
    cursor += 6;
    in.dec_bcast->redirect_rob_idx = pack_bits<uint8_t>(cursor, 7); //5471
    cursor += 7;
    in.dec_bcast->clear_mask = pack_bits<uint64_t>(cursor, 64); //5478
    cursor += 64;
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); //5542
    cursor += 1;
    in.rob_bcast->mret = pack_bits<bool>(cursor, 1); //5543
    cursor += 1;
    in.rob_bcast->sret = pack_bits<bool>(cursor, 1); //5544
    cursor += 1;
    in.rob_bcast->ecall = pack_bits<bool>(cursor, 1); //5545
    cursor += 1;
    in.rob_bcast->exception = pack_bits<bool>(cursor, 1); //5546
    cursor += 1;
    in.rob_bcast->fence = pack_bits<bool>(cursor, 1); //5547
    cursor += 1;
    in.rob_bcast->fence_i = pack_bits<bool>(cursor, 1); //5548
    cursor += 1;
    in.rob_bcast->page_fault_inst = pack_bits<bool>(cursor, 1); //5549
    cursor += 1;
    in.rob_bcast->page_fault_load = pack_bits<bool>(cursor, 1); //5550
    cursor += 1;
    in.rob_bcast->page_fault_store = pack_bits<bool>(cursor, 1); //5551
    cursor += 1;
    in.rob_bcast->illegal_inst = pack_bits<bool>(cursor, 1); //5552
    cursor += 1;
    in.rob_bcast->interrupt = pack_bits<bool>(cursor, 1); //5553
    cursor += 1;
    in.rob_bcast->trap_val = pack_bits<uint32_t>(cursor, 32); //5554
    cursor += 32;
    in.rob_bcast->pc = pack_bits<uint32_t>(cursor, 32); //5586
    cursor += 32;
    in.rob_bcast->head_rob_idx = pack_bits<uint8_t>(cursor, 7); //5618
    cursor += 7;
    in.rob_bcast->head_valid = pack_bits<bool>(cursor, 1); //5625
    cursor += 1;
    in.rob_bcast->head_incomplete_rob_idx = pack_bits<uint8_t>(cursor, 7); //5626
    cursor += 7;
    in.rob_bcast->head_incomplete_valid = pack_bits<bool>(cursor, 1); //5633
    cursor += 1;
    for(int i0 = 0; i0 < 12; i0++) {
        in.reg_file_data_0[i0] = pack_bits<uint32_t>(cursor, 32); //5634
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        in.reg_file_data_1[i0] = pack_bits<uint32_t>(cursor, 32); //6018
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 12; i1++) {
            in.writeb_equal_give_in_1[i0][i1] = pack_bits<bool>(cursor, 1); //6402
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            in.bypass_equal_give_in_1[i0][i1] = pack_bits<bool>(cursor, 1); //6546
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 12; i1++) {
            in.writeb_equal_give_in_2[i0][i1] = pack_bits<bool>(cursor, 1); //6738
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            in.bypass_equal_give_in_2[i0][i1] = pack_bits<bool>(cursor, 1); //6882
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        inst_r[i0].valid = pack_bits<bool>(cursor, 1); //7074
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        inst_r[i0].uop.dest_preg = pack_bits<uint8_t>(cursor, 8); //7086
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        inst_r[i0].uop.result = pack_bits<uint32_t>(cursor, 32); //7182
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        inst_r[i0].uop.br_mask = pack_bits<uint64_t>(cursor, 64); //7566
        cursor += 64;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        inst_r[i0].uop.dest_en = pack_bits<bool>(cursor, 1); //8334
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        inst_r[i0].uop.op = decode_uop_type(pack_bits<uint8_t>(cursor, 5)); //8346
        cursor += 5;
    }
}
void Prf::out_initial_detect() {
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].valid != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.dest_preg != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.src1_preg != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.src1_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.src2_preg != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.src2_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.src1_rdata != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.src1_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.src2_rdata != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.src2_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.ftq_idx != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.ftq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.ftq_offset != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.ftq_offset error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.is_atomic != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.is_atomic error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.dest_en != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.dest_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.src1_en != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.src1_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.src2_en != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.src2_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.src1_is_pc != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.src1_is_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.src2_is_imm != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.src2_is_imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.func3 != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.func3 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.func7 != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.func7 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.imm != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.br_id != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.br_id error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.br_mask != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.br_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.csr_idx != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.csr_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.rob_idx != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.rob_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.stq_idx != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.stq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.stq_flag != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.stq_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.ldq_idx != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.ldq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.rob_flag != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.rob_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.prf2exe->iss_entry[i0].uop.op != 0) {
            std::cout << "out.prf2exe->iss_entry[i0].uop.op error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 2; i0++) {
        if(out.prf_awake->wake[i0].valid != 0) {
            std::cout << "out.prf_awake->wake[i0].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 2; i0++) {
        if(out.prf_awake->wake[i0].preg != 0) {
            std::cout << "out.prf_awake->wake[i0].preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.reg_file_addr_0[i0] != 0) {
            std::cout << "out.reg_file_addr_0[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.reg_file_addr_1[i0] != 0) {
            std::cout << "out.reg_file_addr_1[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.reg_file_2_en[i0] != 0) {
            std::cout << "out.reg_file_2_en[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.reg_file_2_addr[i0] != 0) {
            std::cout << "out.reg_file_2_addr[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(out.reg_file_2_data[i0] != 0) {
            std::cout << "out.reg_file_2_data[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 12; i1++) {
            if(out.writeb_equal_logic_out_1[i0][i1] != 0) {
                std::cout << "out.writeb_equal_logic_out_1[i0][i1] error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(out.bypass_equal_logic_out_1[i0][i1] != 0) {
                std::cout << "out.bypass_equal_logic_out_1[i0][i1] error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 12; i1++) {
            if(out.writeb_equal_logic_out_2[i0][i1] != 0) {
                std::cout << "out.writeb_equal_logic_out_2[i0][i1] error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(out.bypass_equal_logic_out_2[i0][i1] != 0) {
                std::cout << "out.bypass_equal_logic_out_2[i0][i1] error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(inst_r_1[i0].valid != 0) {
            std::cout << "inst_r_1[i0].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(inst_r_1[i0].uop.dest_preg != 0) {
            std::cout << "inst_r_1[i0].uop.dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(inst_r_1[i0].uop.result != 0) {
            std::cout << "inst_r_1[i0].uop.result error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(inst_r_1[i0].uop.br_mask != 0) {
            std::cout << "inst_r_1[i0].uop.br_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(inst_r_1[i0].uop.dest_en != 0) {
            std::cout << "inst_r_1[i0].uop.dest_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        if(inst_r_1[i0].uop.op != 0) {
            std::cout << "inst_r_1[i0].uop.op error, not 0!" << std::endl;
            exit(1);
        }
    }
}
void Prf::simulator_to_po(bool* po) {
    bool* cursor = po;
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].valid, 1); //0
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.dest_preg, 8); //12
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.src1_preg, 8); //108
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.src2_preg, 8); //204
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.src1_rdata, 32); //300
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.src2_rdata, 32); //684
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.ftq_idx, 6); //1068
        cursor += 6;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.ftq_offset, 4); //1140
        cursor += 4;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.is_atomic, 1); //1188
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.dest_en, 1); //1200
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.src1_en, 1); //1212
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.src2_en, 1); //1224
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.src1_is_pc, 1); //1236
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.src2_is_imm, 1); //1248
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.func3, 3); //1260
        cursor += 3;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.func7, 7); //1296
        cursor += 7;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.imm, 32); //1380
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.br_id, 6); //1764
        cursor += 6;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.br_mask, 64); //1836
        cursor += 64;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.csr_idx, 12); //2604
        cursor += 12;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.rob_idx, 7); //2748
        cursor += 7;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.stq_idx, 6); //2832
        cursor += 6;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.stq_flag, 1); //2904
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.ldq_idx, 6); //2916
        cursor += 6;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.rob_flag, 1); //2988
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i0].uop.op, 5); //3000
        cursor += 5;
    }
    for(int i0 = 0; i0 < 2; i0++) {
        unpack_bits(cursor, out.prf_awake->wake[i0].valid, 1); //3060
        cursor += 1;
    }
    for(int i0 = 0; i0 < 2; i0++) {
        unpack_bits(cursor, out.prf_awake->wake[i0].preg, 8); //3062
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.reg_file_addr_0[i0], 7); //3078
        cursor += 7;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.reg_file_addr_1[i0], 7); //3162
        cursor += 7;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.reg_file_2_en[i0], 1); //3246
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.reg_file_2_addr[i0], 7); //3258
        cursor += 7;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, out.reg_file_2_data[i0], 32); //3342
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 12; i1++) {
            unpack_bits(cursor, out.writeb_equal_logic_out_1[i0][i1], 1); //3726
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, out.bypass_equal_logic_out_1[i0][i1], 1); //3870
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 12; i1++) {
            unpack_bits(cursor, out.writeb_equal_logic_out_2[i0][i1], 1); //4062
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, out.bypass_equal_logic_out_2[i0][i1], 1); //4206
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, inst_r_1[i0].valid, 1); //4398
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, inst_r_1[i0].uop.dest_preg, 8); //4410
        cursor += 8;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, inst_r_1[i0].uop.result, 32); //4506
        cursor += 32;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, inst_r_1[i0].uop.br_mask, 64); //4890
        cursor += 64;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, inst_r_1[i0].uop.dest_en, 1); //5658
        cursor += 1;
    }
    for(int i0 = 0; i0 < 12; i0++) {
        unpack_bits(cursor, inst_r_1[i0].uop.op, 5); //5670
        cursor += 5;
    }
}
// void Prf::simulator_with_bsd() {
//     bool pi [8406];
//     bool po [5730];
//     bool* cursor_pi = pi;
//     bool* cursor_po = po;
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].valid, 1); //0
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.dest_preg, 8); //12
//         cursor_pi += 8;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.src1_preg, 8); //108
//         cursor_pi += 8;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.src2_preg, 8); //204
//         cursor_pi += 8;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.ftq_idx, 6); //300
//         cursor_pi += 6;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.ftq_offset, 4); //372
//         cursor_pi += 4;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.is_atomic, 1); //420
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.dest_en, 1); //432
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.src1_en, 1); //444
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.src2_en, 1); //456
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.src1_is_pc, 1); //468
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.src2_is_imm, 1); //480
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.func3, 3); //492
//         cursor_pi += 3;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.func7, 7); //528
//         cursor_pi += 7;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.imm, 32); //612
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.br_id, 6); //996
//         cursor_pi += 6;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.br_mask, 64); //1068
//         cursor_pi += 64;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.csr_idx, 12); //1836
//         cursor_pi += 12;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.rob_idx, 7); //1980
//         cursor_pi += 7;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.stq_idx, 6); //2064
//         cursor_pi += 6;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.stq_flag, 1); //2136
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.ldq_idx, 6); //2148
//         cursor_pi += 6;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.rob_flag, 1); //2220
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i0].uop.op, 5); //2232
//         cursor_pi += 5;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i0].valid, 1); //2292
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i0].uop.dest_preg, 8); //2304
//         cursor_pi += 8;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i0].uop.result, 32); //2400
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i0].uop.br_mask, 64); //2784
//         cursor_pi += 64;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i0].uop.dest_en, 1); //3552
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i0].uop.op, 5); //3564
//         cursor_pi += 5;
//     }
//     for(int i0 = 0; i0 < 16; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->bypass[i0].valid, 1); //3624
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 16; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->bypass[i0].uop.dest_preg, 8); //3640
//         cursor_pi += 8;
//     }
//     for(int i0 = 0; i0 < 16; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->bypass[i0].uop.result, 32); //3768
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 16; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->bypass[i0].uop.br_mask, 64); //4280
//         cursor_pi += 64;
//     }
//     for(int i0 = 0; i0 < 16; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->bypass[i0].uop.dest_en, 1); //5304
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 16; i0++) {
//         unpack_bits(cursor_pi, in.exe2prf->bypass[i0].uop.op, 5); //5320
//         cursor_pi += 5;
//     }
//     unpack_bits(cursor_pi, in.dec_bcast->mispred, 1); //5400
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.dec_bcast->br_mask, 64); //5401
//     cursor_pi += 64;
//     unpack_bits(cursor_pi, in.dec_bcast->br_id, 6); //5465
//     cursor_pi += 6;
//     unpack_bits(cursor_pi, in.dec_bcast->redirect_rob_idx, 7); //5471
//     cursor_pi += 7;
//     unpack_bits(cursor_pi, in.dec_bcast->clear_mask, 64); //5478
//     cursor_pi += 64;
//     unpack_bits(cursor_pi, in.rob_bcast->flush, 1); //5542
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->mret, 1); //5543
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->sret, 1); //5544
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->ecall, 1); //5545
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->exception, 1); //5546
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->fence, 1); //5547
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->fence_i, 1); //5548
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_inst, 1); //5549
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_load, 1); //5550
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_store, 1); //5551
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->illegal_inst, 1); //5552
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->interrupt, 1); //5553
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->trap_val, 32); //5554
//     cursor_pi += 32;
//     unpack_bits(cursor_pi, in.rob_bcast->pc, 32); //5586
//     cursor_pi += 32;
//     unpack_bits(cursor_pi, in.rob_bcast->head_rob_idx, 7); //5618
//     cursor_pi += 7;
//     unpack_bits(cursor_pi, in.rob_bcast->head_valid, 1); //5625
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->head_incomplete_rob_idx, 7); //5626
//     cursor_pi += 7;
//     unpack_bits(cursor_pi, in.rob_bcast->head_incomplete_valid, 1); //5633
//     cursor_pi += 1;
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.reg_file_data_0[i0], 32); //5634
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, in.reg_file_data_1[i0], 32); //6018
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         for(int i1 = 0; i1 < 12; i1++) {
//             unpack_bits(cursor_pi, in.writeb_equal_give_in_1[i0][i1], 1); //6402
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, in.bypass_equal_give_in_1[i0][i1], 1); //6546
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         for(int i1 = 0; i1 < 12; i1++) {
//             unpack_bits(cursor_pi, in.writeb_equal_give_in_2[i0][i1], 1); //6738
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, in.bypass_equal_give_in_2[i0][i1], 1); //6882
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, inst_r[i0].valid, 1); //7074
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, inst_r[i0].uop.dest_preg, 8); //7086
//         cursor_pi += 8;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, inst_r[i0].uop.result, 32); //7182
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, inst_r[i0].uop.br_mask, 64); //7566
//         cursor_pi += 64;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, inst_r[i0].uop.dest_en, 1); //8334
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         unpack_bits(cursor_pi, inst_r[i0].uop.op, 5); //8346
//         cursor_pi += 5;
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
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].valid, 1, "out.prf2exe->iss_entry[i0].valid", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.dest_preg, 8, "out.prf2exe->iss_entry[i0].uop.dest_preg", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.src1_preg, 8, "out.prf2exe->iss_entry[i0].uop.src1_preg", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.src2_preg, 8, "out.prf2exe->iss_entry[i0].uop.src2_preg", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.src1_rdata, 32, "out.prf2exe->iss_entry[i0].uop.src1_rdata", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.src2_rdata, 32, "out.prf2exe->iss_entry[i0].uop.src2_rdata", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.ftq_idx, 6, "out.prf2exe->iss_entry[i0].uop.ftq_idx", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.ftq_offset, 4, "out.prf2exe->iss_entry[i0].uop.ftq_offset", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.is_atomic, 1, "out.prf2exe->iss_entry[i0].uop.is_atomic", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.dest_en, 1, "out.prf2exe->iss_entry[i0].uop.dest_en", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.src1_en, 1, "out.prf2exe->iss_entry[i0].uop.src1_en", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.src2_en, 1, "out.prf2exe->iss_entry[i0].uop.src2_en", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.src1_is_pc, 1, "out.prf2exe->iss_entry[i0].uop.src1_is_pc", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.src2_is_imm, 1, "out.prf2exe->iss_entry[i0].uop.src2_is_imm", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.func3, 3, "out.prf2exe->iss_entry[i0].uop.func3", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.func7, 7, "out.prf2exe->iss_entry[i0].uop.func7", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.imm, 32, "out.prf2exe->iss_entry[i0].uop.imm", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.br_id, 6, "out.prf2exe->iss_entry[i0].uop.br_id", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.br_mask, 64, "out.prf2exe->iss_entry[i0].uop.br_mask", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.csr_idx, 12, "out.prf2exe->iss_entry[i0].uop.csr_idx", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.rob_idx, 7, "out.prf2exe->iss_entry[i0].uop.rob_idx", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.stq_idx, 6, "out.prf2exe->iss_entry[i0].uop.stq_idx", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.stq_flag, 1, "out.prf2exe->iss_entry[i0].uop.stq_flag", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.ldq_idx, 6, "out.prf2exe->iss_entry[i0].uop.ldq_idx", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.rob_flag, 1, "out.prf2exe->iss_entry[i0].uop.rob_flag", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i0].uop.op, 5, "out.prf2exe->iss_entry[i0].uop.op", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 2; i0++) {
//         compare_and_pack(cursor_po, out.prf_awake->wake[i0].valid, 1, "out.prf_awake->wake[i0].valid", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 2; i0++) {
//         compare_and_pack(cursor_po, out.prf_awake->wake[i0].preg, 8, "out.prf_awake->wake[i0].preg", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.reg_file_addr_0[i0], 7, "out.reg_file_addr_0[i0]", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.reg_file_addr_1[i0], 7, "out.reg_file_addr_1[i0]", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.reg_file_2_en[i0], 1, "out.reg_file_2_en[i0]", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.reg_file_2_addr[i0], 7, "out.reg_file_2_addr[i0]", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, out.reg_file_2_data[i0], 32, "out.reg_file_2_data[i0]", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         for(int i1 = 0; i1 < 12; i1++) {
//             compare_and_pack(cursor_po, out.writeb_equal_logic_out_1[i0][i1], 1, "out.writeb_equal_logic_out_1[i0][i1]", pi, 8406);
//         }
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, out.bypass_equal_logic_out_1[i0][i1], 1, "out.bypass_equal_logic_out_1[i0][i1]", pi, 8406);
//         }
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         for(int i1 = 0; i1 < 12; i1++) {
//             compare_and_pack(cursor_po, out.writeb_equal_logic_out_2[i0][i1], 1, "out.writeb_equal_logic_out_2[i0][i1]", pi, 8406);
//         }
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, out.bypass_equal_logic_out_2[i0][i1], 1, "out.bypass_equal_logic_out_2[i0][i1]", pi, 8406);
//         }
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, inst_r_1[i0].valid, 1, "inst_r_1[i0].valid", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, inst_r_1[i0].uop.dest_preg, 8, "inst_r_1[i0].uop.dest_preg", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, inst_r_1[i0].uop.result, 32, "inst_r_1[i0].uop.result", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, inst_r_1[i0].uop.br_mask, 64, "inst_r_1[i0].uop.br_mask", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, inst_r_1[i0].uop.dest_en, 1, "inst_r_1[i0].uop.dest_en", pi, 8406);
//     }
//     for(int i0 = 0; i0 < 12; i0++) {
//         compare_and_pack(cursor_po, inst_r_1[i0].uop.op, 5, "inst_r_1[i0].uop.op", pi, 8406);
//     }
// }

#endif
