#ifndef PRF_CPP_H
#define PRF_CPP_H
#include "config.h"
#include <IO.h>
#include <PRF.h>
#include <cstring>
#include <iostream>
#include <util.h>
#include <map>
#include <iomanip>
#include <cstdint>
void PRF::init() {
  for (int i = 0; i < ISSUE_WAY; i++)
  {  out.prf2exe->ready[i] = true;}
  std::map<wire7_t, wire32_t> cycle_val_map; 
    for (int i = 0; i < ISSUE_WAY; i++) {
        // 检查这个地址在本轮循环是否已经有值了
        // 如果有，强制使用之前的一致值，覆盖当前的随机输入
        // 如果没有，将当前的随机值记录下来
        out.reg_file_addr_0[i] = in.iss2prf->iss_entry[i].uop.src1_preg;
        out.reg_file_addr_1[i] = in.iss2prf->iss_entry[i].uop.src2_preg;
        if (cycle_val_map.find(out.reg_file_addr_0[i]) != cycle_val_map.end()) {
            reg_file     [out.reg_file_addr_0[i]] = cycle_val_map[out.reg_file_addr_0[i]];
        } else {
            reg_file     [out.reg_file_addr_0[i]] = in.reg_file_data_0[i];
            cycle_val_map[out.reg_file_addr_0[i]] = in.reg_file_data_0[i];
        }

        if (cycle_val_map.find(out.reg_file_addr_1[i]) != cycle_val_map.end()) {
            reg_file     [out.reg_file_addr_1[i]] = cycle_val_map[out.reg_file_addr_1[i]];
        } else {
            reg_file     [out.reg_file_addr_1[i]] = in.reg_file_data_1[i];
            cycle_val_map[out.reg_file_addr_1[i]] = in.reg_file_data_1[i];
        }
    }
  for (int i = 0; i < ALU_NUM + 1; i++) {
    out.reg_file_2_en[i] = inst_r[i].valid && inst_r[i].uop.dest_en && !is_page_fault(inst_r[i].uop);
    out.reg_file_2_addr[i] = inst_r[i].uop.dest_preg;
    out.reg_file_2_data[i] = inst_r[i].uop.result;
  }
}

void PRF::comb_br_check() {
  // 根据分支结果向前端返回信息
  bool mispred = false;
  Inst_uop *mispred_uop;

  for (int i = 0; i < BRU_NUM; i++) {
    int iq_br = IQ_BR0 + i;
    if (inst_r[iq_br].valid && inst_r[iq_br].uop.mispred) {
      if (LOG) {
        cout << hex << inst_r[iq_br].uop.pc << endl;
        cout << hex << inst_r[iq_br].uop.pc_next << endl;
        cout << dec << (int)inst_r[iq_br].uop.rob_idx << endl;
        cout << dec << (int)inst_r[iq_br].uop.rob_flag << endl;
      }
      if (!mispred) {
        mispred = true;
        mispred_uop = &inst_r[iq_br].uop;
      } else if (cmp_inst_age(*mispred_uop, inst_r[iq_br].uop)) {
        mispred_uop = &inst_r[iq_br].uop;
      }
    }
  }

  out.prf2dec->mispred = mispred;
  if (mispred) {
    out.prf2dec->redirect_pc = mispred_uop->pc_next;
    out.prf2dec->redirect_rob_idx = mispred_uop->rob_idx;
    out.prf2dec->br_tag = mispred_uop->tag;
    if (LOG)
      cout << "PC " << hex << mispred_uop->pc << " mispredictinn redirect_pc 0x"
           << hex << out.prf2dec->redirect_pc << endl;
  } else {
    // 任意，以代码简单为准
  }
}

void PRF::comb_read() {
  // bypass

  for (int i = 0; i < ISSUE_WAY; i++) {
    out.prf2exe->iss_entry[i] = in.iss2prf->iss_entry[i];
    Inst_entry *entry = &out.prf2exe->iss_entry[i];

    if (entry->valid) {
      if (entry->uop.src1_en) {
        entry->uop.src1_rdata = reg_file[entry->uop.src1_preg];
        for (int j = 0; j < ALU_NUM + 1; j++) {
          if (inst_r[j].valid && inst_r[j].uop.dest_en &&
              inst_r[j].uop.dest_preg == entry->uop.src1_preg)
            entry->uop.src1_rdata = inst_r[j].uop.result;
        }

        for (int j = 0; j < ALU_NUM + 1; j++) {
          if (in.exe2prf->entry[j].valid && in.exe2prf->entry[j].uop.dest_en &&
              in.exe2prf->entry[j].uop.dest_preg == entry->uop.src1_preg)
            entry->uop.src1_rdata = in.exe2prf->entry[j].uop.result;
        }
      }

      if (entry->uop.src2_en) {
        entry->uop.src2_rdata = reg_file[entry->uop.src2_preg];
        for (int j = 0; j < ALU_NUM + 1; j++) {
          if (inst_r[j].valid && inst_r[j].uop.dest_en &&
              inst_r[j].uop.dest_preg == entry->uop.src2_preg)
            entry->uop.src2_rdata = inst_r[j].uop.result;
        }

        for (int j = 0; j < ALU_NUM + 1; j++) {
          if (in.exe2prf->entry[j].valid && in.exe2prf->entry[j].uop.dest_en &&
              in.exe2prf->entry[j].uop.dest_preg == entry->uop.src2_preg)
            entry->uop.src2_rdata = in.exe2prf->entry[j].uop.result;
        }
      }
    }
  }
}

void PRF::comb_complete() {
  for (int i = 0; i < ISSUE_WAY; i++) {
    if (inst_r[i].valid)
      out.prf2rob->entry[i] = inst_r[i];
    else
      out.prf2rob->entry[i].valid = false;
  }
}

void PRF::comb_awake() {
  if (inst_r[IQ_LD].valid && inst_r[IQ_LD].uop.dest_en &&
      !inst_r[IQ_LD].uop.page_fault_load) {
    out.prf_awake->wake.valid = true;
    out.prf_awake->wake.preg = inst_r[IQ_LD].uop.dest_preg;
  } else {
    out.prf_awake->wake.valid = false;
  }
}

void PRF::comb_branch() {
  if (in.dec_bcast->mispred) {
    for (int i = 0; i < ISSUE_WAY; i++) {
      if (inst_r[i].valid &&
          (in.dec_bcast->br_mask & (1 << inst_r[i].uop.tag))) {
        inst_r_1[i].valid = false;
      }
    }
  }
}

void PRF::comb_flush() {
  if (in.rob_bcast->flush) {
    for (int i = 0; i < ISSUE_WAY; i++) {
      inst_r_1[i].valid = false;
    }
  }
}

void PRF::comb_write() {
  for (int i = 0; i < ALU_NUM + 1; i++) {
    if (inst_r[i].valid && inst_r[i].uop.dest_en &&
        !is_page_fault(inst_r[i].uop)) {
      reg_file_2[inst_r[i].uop.dest_preg] = inst_r[i].uop.result;
    }
  }
}

void PRF::comb_pipeline() {
  for (int i = 0; i < ISSUE_WAY; i++) {
    if (in.exe2prf->entry[i].valid && out.prf2exe->ready[i]) {
      inst_r_1[i] = in.exe2prf->entry[i];
    } else {
      inst_r_1[i].valid = false;
    }
  }
}

void PRF::seq() {
  for (int i = 0; i < PRF_NUM; i++) {
    reg_file[i] = reg_file_2[i];
  }

  for (int i = 0; i < ISSUE_WAY; i++) {
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
    // cursor += width; // 游标自动前进
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
        // cursor[i] = (val >> i) & 1;
        cursor[i] = (val >> i) & 1;
    }
    // cursor += width; // 游标前进
}

// 1. 错误打印函数 (避免模板膨胀，单独抽出来)
void print_mismatch_debug(const char* var_name, int bit_offset, int width, uint64_t expected, uint64_t actual, const bool* pi_array, int pi_size) {
    std::cerr << "\n[Mismatch Detected] Variable: " << var_name << "\n";
    std::cerr << "  Width    : " << width << " bits\n";
    std::cerr << "  Expected : 0x" << std::hex << expected << " (From Struct)\n";
    std::cerr << "  Actual   : 0x" << std::hex << actual   << " (From PO)\n";
    std::cerr << "  Diff Bit : " << std::dec << bit_offset << "\n"; // 这里的 offset 是相对于该变量的内部偏移

    std::cerr << "-------- PI Binary Sequence Dump (" << pi_size << " bits) --------\n";
    for (int i = 0; i < pi_size; i++) {
        std::cerr << pi_array[i];
        if ((i + 1) % 64 == 0) std::cerr << "\n"; // 每64位换行，方便查看
    }
    std::cerr << "\n----------------------------------------------------------\n" << std::dec;
}

// 2. 带比对功能的打包函数
// 参数：cursor(po指针), target_var(变量引用), width(位宽), name(变量名), pi_head(pi数组头), pi_size(大小)
template <typename T>
inline void compare_and_pack(bool*& cursor, T& target_var, int width, const char* var_name, const bool* pi_array, int pi_size) {
    // A. 从 PO 游标读取实际值 (Actual)
    T actual_val = 0;
    for (int i = 0; i < width; i++) {
        actual_val |= (static_cast<T>(cursor[i]) << i);
    }

    // B. 获取预期值 (Expected)
    // 注意：必须把预期值的高位垃圾清理掉，只保留 width 位
    T mask = (width == 64) ? static_cast<T>(-1) : ((static_cast<T>(1) << width) - 1);
    T expected_val = target_var & mask;

    // C. 比对 (Bit-by-Bit Check)
    if (expected_val != actual_val) {
    // 找到第一个不匹配的 bit
        for (int i = 0; i < width; i++) {
            bool bit_exp = (expected_val >> i) & 1;
            bool bit_act = (actual_val >> i) & 1;
            if (bit_exp != bit_act) {
                // 触发 Dump
                print_mismatch_debug(var_name, i, width, static_cast<uint64_t>(expected_val), static_cast<uint64_t>(actual_val), pi_array, pi_size);
                // 通常我们只报第一个错，或者你可以选择 continue 报所有错
                break; 
            }
        }
    }
    // D. 赋值 (Update)
    // 将 PO 的值写入变量 (如果你只做 Check 不想覆盖，注释掉这一行)
    target_var = actual_val;
    // E. 游标前进
    cursor += width;
}

void PRF::pi_to_simulator(bool* pi) {
    const bool* cursor = pi;
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].valid = pack_bits<bool>(cursor, 1); //0
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.instruction = pack_bits<uint32_t>(cursor, 32); //7
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.dest_areg = pack_bits<uint8_t>(cursor, 6); //231
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src1_areg = pack_bits<uint8_t>(cursor, 6); //273
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src2_areg = pack_bits<uint8_t>(cursor, 6); //315
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.dest_preg = pack_bits<uint8_t>(cursor, 7); //357
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src1_preg = pack_bits<uint8_t>(cursor, 7); //406
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src2_preg = pack_bits<uint8_t>(cursor, 7); //455
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.old_dest_preg = pack_bits<uint8_t>(cursor, 7); //504
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src1_rdata = pack_bits<uint32_t>(cursor, 32); //553
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src2_rdata = pack_bits<uint32_t>(cursor, 32); //777
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.result = pack_bits<uint32_t>(cursor, 32); //1001
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.pred_br_taken = pack_bits<bool>(cursor, 1); //1225
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.alt_pred = pack_bits<bool>(cursor, 1); //1232
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.altpcpn = pack_bits<uint8_t>(cursor, 8); //1239
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.pcpn = pack_bits<uint8_t>(cursor, 8); //1295
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.pred_br_pc = pack_bits<uint32_t>(cursor, 32); //1351
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.mispred = pack_bits<bool>(cursor, 1); //1575
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.br_taken = pack_bits<bool>(cursor, 1); //1582
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.pc_next = pack_bits<uint32_t>(cursor, 32); //1589
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.dest_en = pack_bits<bool>(cursor, 1); //1813
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src1_en = pack_bits<bool>(cursor, 1); //1820
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src2_en = pack_bits<bool>(cursor, 1); //1827
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src1_busy = pack_bits<bool>(cursor, 1); //1834
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src2_busy = pack_bits<bool>(cursor, 1); //1841
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src1_latency = pack_bits<uint8_t>(cursor, 4); //1848
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src2_latency = pack_bits<uint8_t>(cursor, 4); //1876
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //1904
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //1911
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.func3 = pack_bits<uint8_t>(cursor, 3); //1918
        cursor += 3;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.func7_5 = pack_bits<bool>(cursor, 1); //1939
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.imm = pack_bits<uint32_t>(cursor, 32); //1946
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.pc = pack_bits<uint32_t>(cursor, 32); //2170
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.tag = pack_bits<uint8_t>(cursor, 4); //2394
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //2422
        cursor += 12;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.rob_idx = pack_bits<uint8_t>(cursor, 7); //2506
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //2555
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.pre_sta_mask = pack_bits<uint16_t>(cursor, 16); //2583
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.pre_std_mask = pack_bits<uint16_t>(cursor, 16); //2695
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.uop_num = pack_bits<uint8_t>(cursor, 2); //2807
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.cplt_num = pack_bits<uint8_t>(cursor, 2); //2821
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.rob_flag = pack_bits<bool>(cursor, 1); //2835
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.page_fault_inst = pack_bits<bool>(cursor, 1); //2842
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.page_fault_load = pack_bits<bool>(cursor, 1); //2849
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.page_fault_store = pack_bits<bool>(cursor, 1); //2856
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.illegal_inst = pack_bits<bool>(cursor, 1); //2863
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.type = pack_bits<uint8_t>(cursor, 4); //2870
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.op = pack_bits<uint8_t>(cursor, 4); //2898
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.iss2prf->iss_entry[i].uop.amoop = pack_bits<uint8_t>(cursor, 4); //2926
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].valid = pack_bits<bool>(cursor, 1); //2954
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.instruction = pack_bits<uint32_t>(cursor, 32); //2961
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.dest_areg = pack_bits<uint8_t>(cursor, 6); //3185
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src1_areg = pack_bits<uint8_t>(cursor, 6); //3227
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src2_areg = pack_bits<uint8_t>(cursor, 6); //3269
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.dest_preg = pack_bits<uint8_t>(cursor, 7); //3311
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src1_preg = pack_bits<uint8_t>(cursor, 7); //3360
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src2_preg = pack_bits<uint8_t>(cursor, 7); //3409
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.old_dest_preg = pack_bits<uint8_t>(cursor, 7); //3458
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src1_rdata = pack_bits<uint32_t>(cursor, 32); //3507
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src2_rdata = pack_bits<uint32_t>(cursor, 32); //3731
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.result = pack_bits<uint32_t>(cursor, 32); //3955
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.pred_br_taken = pack_bits<bool>(cursor, 1); //4179
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.alt_pred = pack_bits<bool>(cursor, 1); //4186
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.altpcpn = pack_bits<uint8_t>(cursor, 8); //4193
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.pcpn = pack_bits<uint8_t>(cursor, 8); //4249
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.pred_br_pc = pack_bits<uint32_t>(cursor, 32); //4305
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.mispred = pack_bits<bool>(cursor, 1); //4529
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.br_taken = pack_bits<bool>(cursor, 1); //4536
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.pc_next = pack_bits<uint32_t>(cursor, 32); //4543
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.dest_en = pack_bits<bool>(cursor, 1); //4767
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src1_en = pack_bits<bool>(cursor, 1); //4774
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src2_en = pack_bits<bool>(cursor, 1); //4781
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src1_busy = pack_bits<bool>(cursor, 1); //4788
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src2_busy = pack_bits<bool>(cursor, 1); //4795
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src1_latency = pack_bits<uint8_t>(cursor, 4); //4802
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src2_latency = pack_bits<uint8_t>(cursor, 4); //4830
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //4858
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //4865
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.func3 = pack_bits<uint8_t>(cursor, 3); //4872
        cursor += 3;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.func7_5 = pack_bits<bool>(cursor, 1); //4893
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.imm = pack_bits<uint32_t>(cursor, 32); //4900
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.pc = pack_bits<uint32_t>(cursor, 32); //5124
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.tag = pack_bits<uint8_t>(cursor, 4); //5348
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //5376
        cursor += 12;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.rob_idx = pack_bits<uint8_t>(cursor, 7); //5460
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //5509
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.pre_sta_mask = pack_bits<uint16_t>(cursor, 16); //5537
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.pre_std_mask = pack_bits<uint16_t>(cursor, 16); //5649
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.uop_num = pack_bits<uint8_t>(cursor, 2); //5761
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.cplt_num = pack_bits<uint8_t>(cursor, 2); //5775
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.rob_flag = pack_bits<bool>(cursor, 1); //5789
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.page_fault_inst = pack_bits<bool>(cursor, 1); //5796
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.page_fault_load = pack_bits<bool>(cursor, 1); //5803
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.page_fault_store = pack_bits<bool>(cursor, 1); //5810
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.illegal_inst = pack_bits<bool>(cursor, 1); //5817
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.type = pack_bits<uint8_t>(cursor, 4); //5824
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.op = pack_bits<uint8_t>(cursor, 4); //5852
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        in.exe2prf->entry[i].uop.amoop = pack_bits<uint8_t>(cursor, 4); //5880
        cursor += 4;
    }
    in.dec_bcast->mispred = pack_bits<bool>(cursor, 1); //5908
    cursor += 1;
    in.dec_bcast->br_mask = pack_bits<uint16_t>(cursor, 16); //5909
    cursor += 16;
    in.dec_bcast->br_tag = pack_bits<uint8_t>(cursor, 4); //5925
    cursor += 4;
    in.dec_bcast->redirect_rob_idx = pack_bits<uint8_t>(cursor, 7); //5929
    cursor += 7;
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); //5936
    cursor += 1;
    in.rob_bcast->mret = pack_bits<bool>(cursor, 1); //5937
    cursor += 1;
    in.rob_bcast->sret = pack_bits<bool>(cursor, 1); //5938
    cursor += 1;
    in.rob_bcast->ecall = pack_bits<bool>(cursor, 1); //5939
    cursor += 1;
    in.rob_bcast->exception = pack_bits<bool>(cursor, 1); //5940
    cursor += 1;
    in.rob_bcast->page_fault_inst = pack_bits<bool>(cursor, 1); //5941
    cursor += 1;
    in.rob_bcast->page_fault_load = pack_bits<bool>(cursor, 1); //5942
    cursor += 1;
    in.rob_bcast->page_fault_store = pack_bits<bool>(cursor, 1); //5943
    cursor += 1;
    in.rob_bcast->illegal_inst = pack_bits<bool>(cursor, 1); //5944
    cursor += 1;
    in.rob_bcast->interrupt = pack_bits<bool>(cursor, 1); //5945
    cursor += 1;
    in.rob_bcast->trap_val = pack_bits<uint32_t>(cursor, 32); //5946
    cursor += 32;
    in.rob_bcast->pc = pack_bits<uint32_t>(cursor, 32); //5978
    cursor += 32;
    for(int i = 0; i < 7; i++) {
        in.reg_file_data_0[i] = pack_bits<uint32_t>(cursor, 32); //6010
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        in.reg_file_data_1[i] = pack_bits<uint32_t>(cursor, 32); //6234
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].valid = pack_bits<bool>(cursor, 1); //6458
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.instruction = pack_bits<uint32_t>(cursor, 32); //6465
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.dest_areg = pack_bits<uint8_t>(cursor, 6); //6689
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src1_areg = pack_bits<uint8_t>(cursor, 6); //6731
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src2_areg = pack_bits<uint8_t>(cursor, 6); //6773
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.dest_preg = pack_bits<uint8_t>(cursor, 7); //6815
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src1_preg = pack_bits<uint8_t>(cursor, 7); //6864
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src2_preg = pack_bits<uint8_t>(cursor, 7); //6913
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.old_dest_preg = pack_bits<uint8_t>(cursor, 7); //6962
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src1_rdata = pack_bits<uint32_t>(cursor, 32); //7011
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src2_rdata = pack_bits<uint32_t>(cursor, 32); //7235
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.result = pack_bits<uint32_t>(cursor, 32); //7459
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.pred_br_taken = pack_bits<bool>(cursor, 1); //7683
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.alt_pred = pack_bits<bool>(cursor, 1); //7690
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.altpcpn = pack_bits<uint8_t>(cursor, 8); //7697
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.pcpn = pack_bits<uint8_t>(cursor, 8); //7753
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.pred_br_pc = pack_bits<uint32_t>(cursor, 32); //7809
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.mispred = pack_bits<bool>(cursor, 1); //8033
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.br_taken = pack_bits<bool>(cursor, 1); //8040
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.pc_next = pack_bits<uint32_t>(cursor, 32); //8047
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.dest_en = pack_bits<bool>(cursor, 1); //8271
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src1_en = pack_bits<bool>(cursor, 1); //8278
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src2_en = pack_bits<bool>(cursor, 1); //8285
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src1_busy = pack_bits<bool>(cursor, 1); //8292
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src2_busy = pack_bits<bool>(cursor, 1); //8299
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src1_latency = pack_bits<uint8_t>(cursor, 4); //8306
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src2_latency = pack_bits<uint8_t>(cursor, 4); //8334
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //8362
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //8369
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.func3 = pack_bits<uint8_t>(cursor, 3); //8376
        cursor += 3;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.func7_5 = pack_bits<bool>(cursor, 1); //8397
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.imm = pack_bits<uint32_t>(cursor, 32); //8404
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.pc = pack_bits<uint32_t>(cursor, 32); //8628
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.tag = pack_bits<uint8_t>(cursor, 4); //8852
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //8880
        cursor += 12;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.rob_idx = pack_bits<uint8_t>(cursor, 7); //8964
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //9013
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.pre_sta_mask = pack_bits<uint16_t>(cursor, 16); //9041
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.pre_std_mask = pack_bits<uint16_t>(cursor, 16); //9153
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.uop_num = pack_bits<uint8_t>(cursor, 2); //9265
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.cplt_num = pack_bits<uint8_t>(cursor, 2); //9279
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.rob_flag = pack_bits<bool>(cursor, 1); //9293
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.page_fault_inst = pack_bits<bool>(cursor, 1); //9300
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.page_fault_load = pack_bits<bool>(cursor, 1); //9307
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.page_fault_store = pack_bits<bool>(cursor, 1); //9314
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.illegal_inst = pack_bits<bool>(cursor, 1); //9321
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.type = pack_bits<uint8_t>(cursor, 4); //9328
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.op = pack_bits<uint8_t>(cursor, 4); //9356
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        inst_r[i].uop.amoop = pack_bits<uint8_t>(cursor, 4); //9384
        cursor += 4;
    }
}
void PRF::out_initial_detect() {
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].valid != 0) {
            std::cout << "out.prf2exe->iss_entry[i].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.instruction != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.instruction error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.dest_areg != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.dest_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src1_areg != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src1_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src2_areg != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src2_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.dest_preg != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src1_preg != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src1_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src2_preg != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src2_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.old_dest_preg != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.old_dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src1_rdata != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src1_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src2_rdata != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src2_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.result != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.result error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.pred_br_taken != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.pred_br_taken error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.alt_pred != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.alt_pred error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.altpcpn != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.altpcpn error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.pcpn != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.pcpn error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.pred_br_pc != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.pred_br_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.mispred != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.mispred error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.br_taken != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.br_taken error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.pc_next != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.pc_next error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.dest_en != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.dest_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src1_en != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src1_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src2_en != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src2_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src1_busy != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src1_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src2_busy != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src2_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src1_latency != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src1_latency error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src2_latency != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src2_latency error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src1_is_pc != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src1_is_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.src2_is_imm != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.src2_is_imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.func3 != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.func3 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.func7_5 != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.func7_5 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.imm != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.pc != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.tag != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.tag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.csr_idx != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.csr_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.rob_idx != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.rob_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.stq_idx != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.stq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.pre_sta_mask != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.pre_sta_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.pre_std_mask != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.pre_std_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.uop_num != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.uop_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.cplt_num != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.cplt_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.rob_flag != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.rob_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.page_fault_inst != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.page_fault_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.page_fault_load != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.page_fault_load error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.page_fault_store != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.page_fault_store error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.illegal_inst != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.illegal_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.type != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.type error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.op != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.op error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->iss_entry[i].uop.amoop != 0) {
            std::cout << "out.prf2exe->iss_entry[i].uop.amoop error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2exe->ready[i] != 0) {
            std::cout << "out.prf2exe->ready[i] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].valid != 0) {
            std::cout << "out.prf2rob->entry[i].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.instruction != 0) {
            std::cout << "out.prf2rob->entry[i].uop.instruction error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.dest_areg != 0) {
            std::cout << "out.prf2rob->entry[i].uop.dest_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src1_areg != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src1_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src2_areg != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src2_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.dest_preg != 0) {
            std::cout << "out.prf2rob->entry[i].uop.dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src1_preg != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src1_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src2_preg != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src2_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.old_dest_preg != 0) {
            std::cout << "out.prf2rob->entry[i].uop.old_dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src1_rdata != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src1_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src2_rdata != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src2_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.result != 0) {
            std::cout << "out.prf2rob->entry[i].uop.result error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.pred_br_taken != 0) {
            std::cout << "out.prf2rob->entry[i].uop.pred_br_taken error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.alt_pred != 0) {
            std::cout << "out.prf2rob->entry[i].uop.alt_pred error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.altpcpn != 0) {
            std::cout << "out.prf2rob->entry[i].uop.altpcpn error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.pcpn != 0) {
            std::cout << "out.prf2rob->entry[i].uop.pcpn error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.pred_br_pc != 0) {
            std::cout << "out.prf2rob->entry[i].uop.pred_br_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.mispred != 0) {
            std::cout << "out.prf2rob->entry[i].uop.mispred error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.br_taken != 0) {
            std::cout << "out.prf2rob->entry[i].uop.br_taken error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.pc_next != 0) {
            std::cout << "out.prf2rob->entry[i].uop.pc_next error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.dest_en != 0) {
            std::cout << "out.prf2rob->entry[i].uop.dest_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src1_en != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src1_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src2_en != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src2_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src1_busy != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src1_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src2_busy != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src2_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src1_latency != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src1_latency error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src2_latency != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src2_latency error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src1_is_pc != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src1_is_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.src2_is_imm != 0) {
            std::cout << "out.prf2rob->entry[i].uop.src2_is_imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.func3 != 0) {
            std::cout << "out.prf2rob->entry[i].uop.func3 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.func7_5 != 0) {
            std::cout << "out.prf2rob->entry[i].uop.func7_5 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.imm != 0) {
            std::cout << "out.prf2rob->entry[i].uop.imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.pc != 0) {
            std::cout << "out.prf2rob->entry[i].uop.pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.tag != 0) {
            std::cout << "out.prf2rob->entry[i].uop.tag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.csr_idx != 0) {
            std::cout << "out.prf2rob->entry[i].uop.csr_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.rob_idx != 0) {
            std::cout << "out.prf2rob->entry[i].uop.rob_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.stq_idx != 0) {
            std::cout << "out.prf2rob->entry[i].uop.stq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.pre_sta_mask != 0) {
            std::cout << "out.prf2rob->entry[i].uop.pre_sta_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.pre_std_mask != 0) {
            std::cout << "out.prf2rob->entry[i].uop.pre_std_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.uop_num != 0) {
            std::cout << "out.prf2rob->entry[i].uop.uop_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.cplt_num != 0) {
            std::cout << "out.prf2rob->entry[i].uop.cplt_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.rob_flag != 0) {
            std::cout << "out.prf2rob->entry[i].uop.rob_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.page_fault_inst != 0) {
            std::cout << "out.prf2rob->entry[i].uop.page_fault_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.page_fault_load != 0) {
            std::cout << "out.prf2rob->entry[i].uop.page_fault_load error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.page_fault_store != 0) {
            std::cout << "out.prf2rob->entry[i].uop.page_fault_store error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.illegal_inst != 0) {
            std::cout << "out.prf2rob->entry[i].uop.illegal_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.type != 0) {
            std::cout << "out.prf2rob->entry[i].uop.type error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.op != 0) {
            std::cout << "out.prf2rob->entry[i].uop.op error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.prf2rob->entry[i].uop.amoop != 0) {
            std::cout << "out.prf2rob->entry[i].uop.amoop error, not 0!" << std::endl;
            exit(1);
        }
    }
    if(out.prf2dec->mispred != 0) {
        std::cout << "out.prf2dec->mispred error, not 0!" << std::endl;
        exit(1);
    }
    if(out.prf2dec->redirect_pc != 0) {
        std::cout << "out.prf2dec->redirect_pc error, not 0!" << std::endl;
        exit(1);
    }
    if(out.prf2dec->redirect_rob_idx != 0) {
        std::cout << "out.prf2dec->redirect_rob_idx error, not 0!" << std::endl;
        exit(1);
    }
    if(out.prf2dec->br_tag != 0) {
        std::cout << "out.prf2dec->br_tag error, not 0!" << std::endl;
        exit(1);
    }
    if(out.prf_awake->wake.valid != 0) {
        std::cout << "out.prf_awake->wake.valid error, not 0!" << std::endl;
        exit(1);
    }
    if(out.prf_awake->wake.preg != 0) {
        std::cout << "out.prf_awake->wake.preg error, not 0!" << std::endl;
        exit(1);
    }
    if(out.prf_awake->wake.latency != 0) {
        std::cout << "out.prf_awake->wake.latency error, not 0!" << std::endl;
        exit(1);
    }
    for(int i = 0; i < 7; i++) {
        if(out.reg_file_addr_0[i] != 0) {
            std::cout << "out.reg_file_addr_0[i] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(out.reg_file_addr_1[i] != 0) {
            std::cout << "out.reg_file_addr_1[i] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 3; i++) {
        if(out.reg_file_2_en[i] != 0) {
            std::cout << "out.reg_file_2_en[i] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 3; i++) {
        if(out.reg_file_2_addr[i] != 0) {
            std::cout << "out.reg_file_2_addr[i] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 3; i++) {
        if(out.reg_file_2_data[i] != 0) {
            std::cout << "out.reg_file_2_data[i] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].valid != 0) {
            std::cout << "inst_r_1[i].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.instruction != 0) {
            std::cout << "inst_r_1[i].uop.instruction error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.dest_areg != 0) {
            std::cout << "inst_r_1[i].uop.dest_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src1_areg != 0) {
            std::cout << "inst_r_1[i].uop.src1_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src2_areg != 0) {
            std::cout << "inst_r_1[i].uop.src2_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.dest_preg != 0) {
            std::cout << "inst_r_1[i].uop.dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src1_preg != 0) {
            std::cout << "inst_r_1[i].uop.src1_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src2_preg != 0) {
            std::cout << "inst_r_1[i].uop.src2_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.old_dest_preg != 0) {
            std::cout << "inst_r_1[i].uop.old_dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src1_rdata != 0) {
            std::cout << "inst_r_1[i].uop.src1_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src2_rdata != 0) {
            std::cout << "inst_r_1[i].uop.src2_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.result != 0) {
            std::cout << "inst_r_1[i].uop.result error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.pred_br_taken != 0) {
            std::cout << "inst_r_1[i].uop.pred_br_taken error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.alt_pred != 0) {
            std::cout << "inst_r_1[i].uop.alt_pred error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.altpcpn != 0) {
            std::cout << "inst_r_1[i].uop.altpcpn error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.pcpn != 0) {
            std::cout << "inst_r_1[i].uop.pcpn error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.pred_br_pc != 0) {
            std::cout << "inst_r_1[i].uop.pred_br_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.mispred != 0) {
            std::cout << "inst_r_1[i].uop.mispred error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.br_taken != 0) {
            std::cout << "inst_r_1[i].uop.br_taken error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.pc_next != 0) {
            std::cout << "inst_r_1[i].uop.pc_next error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.dest_en != 0) {
            std::cout << "inst_r_1[i].uop.dest_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src1_en != 0) {
            std::cout << "inst_r_1[i].uop.src1_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src2_en != 0) {
            std::cout << "inst_r_1[i].uop.src2_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src1_busy != 0) {
            std::cout << "inst_r_1[i].uop.src1_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src2_busy != 0) {
            std::cout << "inst_r_1[i].uop.src2_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src1_latency != 0) {
            std::cout << "inst_r_1[i].uop.src1_latency error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src2_latency != 0) {
            std::cout << "inst_r_1[i].uop.src2_latency error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src1_is_pc != 0) {
            std::cout << "inst_r_1[i].uop.src1_is_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.src2_is_imm != 0) {
            std::cout << "inst_r_1[i].uop.src2_is_imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.func3 != 0) {
            std::cout << "inst_r_1[i].uop.func3 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.func7_5 != 0) {
            std::cout << "inst_r_1[i].uop.func7_5 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.imm != 0) {
            std::cout << "inst_r_1[i].uop.imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.pc != 0) {
            std::cout << "inst_r_1[i].uop.pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.tag != 0) {
            std::cout << "inst_r_1[i].uop.tag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.csr_idx != 0) {
            std::cout << "inst_r_1[i].uop.csr_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.rob_idx != 0) {
            std::cout << "inst_r_1[i].uop.rob_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.stq_idx != 0) {
            std::cout << "inst_r_1[i].uop.stq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.pre_sta_mask != 0) {
            std::cout << "inst_r_1[i].uop.pre_sta_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.pre_std_mask != 0) {
            std::cout << "inst_r_1[i].uop.pre_std_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.uop_num != 0) {
            std::cout << "inst_r_1[i].uop.uop_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.cplt_num != 0) {
            std::cout << "inst_r_1[i].uop.cplt_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.rob_flag != 0) {
            std::cout << "inst_r_1[i].uop.rob_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.page_fault_inst != 0) {
            std::cout << "inst_r_1[i].uop.page_fault_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.page_fault_load != 0) {
            std::cout << "inst_r_1[i].uop.page_fault_load error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.page_fault_store != 0) {
            std::cout << "inst_r_1[i].uop.page_fault_store error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.illegal_inst != 0) {
            std::cout << "inst_r_1[i].uop.illegal_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.type != 0) {
            std::cout << "inst_r_1[i].uop.type error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.op != 0) {
            std::cout << "inst_r_1[i].uop.op error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 7; i++) {
        if(inst_r_1[i].uop.amoop != 0) {
            std::cout << "inst_r_1[i].uop.amoop error, not 0!" << std::endl;
            exit(1);
        }
    }
}
void PRF::simulator_to_po(bool* po) {
    bool* cursor = po;
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].valid, 1); //0
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.instruction, 32); //7
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.dest_areg, 6); //231
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src1_areg, 6); //273
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src2_areg, 6); //315
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.dest_preg, 7); //357
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src1_preg, 7); //406
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src2_preg, 7); //455
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.old_dest_preg, 7); //504
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src1_rdata, 32); //553
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src2_rdata, 32); //777
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.result, 32); //1001
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.pred_br_taken, 1); //1225
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.alt_pred, 1); //1232
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.altpcpn, 8); //1239
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.pcpn, 8); //1295
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.pred_br_pc, 32); //1351
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.mispred, 1); //1575
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.br_taken, 1); //1582
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.pc_next, 32); //1589
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.dest_en, 1); //1813
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src1_en, 1); //1820
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src2_en, 1); //1827
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src1_busy, 1); //1834
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src2_busy, 1); //1841
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src1_latency, 4); //1848
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src2_latency, 4); //1876
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src1_is_pc, 1); //1904
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.src2_is_imm, 1); //1911
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.func3, 3); //1918
        cursor += 3;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.func7_5, 1); //1939
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.imm, 32); //1946
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.pc, 32); //2170
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.tag, 4); //2394
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.csr_idx, 12); //2422
        cursor += 12;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.rob_idx, 7); //2506
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.stq_idx, 4); //2555
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.pre_sta_mask, 16); //2583
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.pre_std_mask, 16); //2695
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.uop_num, 2); //2807
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.cplt_num, 2); //2821
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.rob_flag, 1); //2835
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.page_fault_inst, 1); //2842
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.page_fault_load, 1); //2849
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.page_fault_store, 1); //2856
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.illegal_inst, 1); //2863
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.type, 4); //2870
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.op, 4); //2898
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->iss_entry[i].uop.amoop, 4); //2926
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2exe->ready[i], 1); //2954
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].valid, 1); //2961
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.instruction, 32); //2968
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.dest_areg, 6); //3192
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src1_areg, 6); //3234
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src2_areg, 6); //3276
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.dest_preg, 7); //3318
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src1_preg, 7); //3367
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src2_preg, 7); //3416
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.old_dest_preg, 7); //3465
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src1_rdata, 32); //3514
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src2_rdata, 32); //3738
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.result, 32); //3962
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.pred_br_taken, 1); //4186
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.alt_pred, 1); //4193
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.altpcpn, 8); //4200
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.pcpn, 8); //4256
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.pred_br_pc, 32); //4312
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.mispred, 1); //4536
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.br_taken, 1); //4543
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.pc_next, 32); //4550
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.dest_en, 1); //4774
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src1_en, 1); //4781
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src2_en, 1); //4788
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src1_busy, 1); //4795
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src2_busy, 1); //4802
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src1_latency, 4); //4809
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src2_latency, 4); //4837
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src1_is_pc, 1); //4865
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.src2_is_imm, 1); //4872
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.func3, 3); //4879
        cursor += 3;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.func7_5, 1); //4900
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.imm, 32); //4907
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.pc, 32); //5131
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.tag, 4); //5355
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.csr_idx, 12); //5383
        cursor += 12;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.rob_idx, 7); //5467
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.stq_idx, 4); //5516
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.pre_sta_mask, 16); //5544
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.pre_std_mask, 16); //5656
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.uop_num, 2); //5768
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.cplt_num, 2); //5782
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.rob_flag, 1); //5796
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.page_fault_inst, 1); //5803
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.page_fault_load, 1); //5810
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.page_fault_store, 1); //5817
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.illegal_inst, 1); //5824
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.type, 4); //5831
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.op, 4); //5859
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.prf2rob->entry[i].uop.amoop, 4); //5887
        cursor += 4;
    }
    unpack_bits(cursor, out.prf2dec->mispred, 1); //5915
    cursor += 1;
    unpack_bits(cursor, out.prf2dec->redirect_pc, 32); //5916
    cursor += 32;
    unpack_bits(cursor, out.prf2dec->redirect_rob_idx, 7); //5948
    cursor += 7;
    unpack_bits(cursor, out.prf2dec->br_tag, 4); //5955
    cursor += 4;
    unpack_bits(cursor, out.prf_awake->wake.valid, 1); //5959
    cursor += 1;
    unpack_bits(cursor, out.prf_awake->wake.preg, 7); //5960
    cursor += 7;
    unpack_bits(cursor, out.prf_awake->wake.latency, 2); //5967
    cursor += 2;
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.reg_file_addr_0[i], 7); //5969
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, out.reg_file_addr_1[i], 7); //6018
        cursor += 7;
    }
    for(int i = 0; i < 3; i++) {
        unpack_bits(cursor, out.reg_file_2_en[i], 1); //6067
        cursor += 1;
    }
    for(int i = 0; i < 3; i++) {
        unpack_bits(cursor, out.reg_file_2_addr[i], 7); //6070
        cursor += 7;
    }
    for(int i = 0; i < 3; i++) {
        unpack_bits(cursor, out.reg_file_2_data[i], 32); //6091
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].valid, 1); //6187
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.instruction, 32); //6194
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.dest_areg, 6); //6418
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src1_areg, 6); //6460
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src2_areg, 6); //6502
        cursor += 6;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.dest_preg, 7); //6544
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src1_preg, 7); //6593
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src2_preg, 7); //6642
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.old_dest_preg, 7); //6691
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src1_rdata, 32); //6740
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src2_rdata, 32); //6964
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.result, 32); //7188
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.pred_br_taken, 1); //7412
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.alt_pred, 1); //7419
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.altpcpn, 8); //7426
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.pcpn, 8); //7482
        cursor += 8;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.pred_br_pc, 32); //7538
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.mispred, 1); //7762
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.br_taken, 1); //7769
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.pc_next, 32); //7776
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.dest_en, 1); //8000
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src1_en, 1); //8007
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src2_en, 1); //8014
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src1_busy, 1); //8021
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src2_busy, 1); //8028
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src1_latency, 4); //8035
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src2_latency, 4); //8063
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src1_is_pc, 1); //8091
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.src2_is_imm, 1); //8098
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.func3, 3); //8105
        cursor += 3;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.func7_5, 1); //8126
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.imm, 32); //8133
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.pc, 32); //8357
        cursor += 32;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.tag, 4); //8581
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.csr_idx, 12); //8609
        cursor += 12;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.rob_idx, 7); //8693
        cursor += 7;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.stq_idx, 4); //8742
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.pre_sta_mask, 16); //8770
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.pre_std_mask, 16); //8882
        cursor += 16;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.uop_num, 2); //8994
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.cplt_num, 2); //9008
        cursor += 2;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.rob_flag, 1); //9022
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.page_fault_inst, 1); //9029
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.page_fault_load, 1); //9036
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.page_fault_store, 1); //9043
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.illegal_inst, 1); //9050
        cursor += 1;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.type, 4); //9057
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.op, 4); //9085
        cursor += 4;
    }
    for(int i = 0; i < 7; i++) {
        unpack_bits(cursor, inst_r_1[i].uop.amoop, 4); //9113
        cursor += 4;
    }
}
// void PRF::simulator_with_bsd() {
//     bool pi [9412];
//     bool po [9141];
//     bool* cursor_pi = pi;
//     bool* cursor_po = po;
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].valid, 1); //0
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.instruction, 32); //7
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.dest_areg, 6); //231
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src1_areg, 6); //273
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src2_areg, 6); //315
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.dest_preg, 7); //357
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src1_preg, 7); //406
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src2_preg, 7); //455
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.old_dest_preg, 7); //504
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src1_rdata, 32); //553
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src2_rdata, 32); //777
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.result, 32); //1001
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.pred_br_taken, 1); //1225
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.alt_pred, 1); //1232
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.altpcpn, 8); //1239
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.pcpn, 8); //1295
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.pred_br_pc, 32); //1351
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.mispred, 1); //1575
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.br_taken, 1); //1582
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.pc_next, 32); //1589
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.dest_en, 1); //1813
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src1_en, 1); //1820
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src2_en, 1); //1827
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src1_busy, 1); //1834
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src2_busy, 1); //1841
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src1_latency, 4); //1848
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src2_latency, 4); //1876
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src1_is_pc, 1); //1904
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.src2_is_imm, 1); //1911
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.func3, 3); //1918
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.func7_5, 1); //1939
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.imm, 32); //1946
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.pc, 32); //2170
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.tag, 4); //2394
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.csr_idx, 12); //2422
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.rob_idx, 7); //2506
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.stq_idx, 4); //2555
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.pre_sta_mask, 16); //2583
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.pre_std_mask, 16); //2695
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.uop_num, 2); //2807
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.cplt_num, 2); //2821
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.rob_flag, 1); //2835
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.page_fault_inst, 1); //2842
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.page_fault_load, 1); //2849
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.page_fault_store, 1); //2856
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.illegal_inst, 1); //2863
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.type, 4); //2870
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.op, 4); //2898
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.iss2prf->iss_entry[i].uop.amoop, 4); //2926
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].valid, 1); //2954
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.instruction, 32); //2961
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.dest_areg, 6); //3185
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src1_areg, 6); //3227
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src2_areg, 6); //3269
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.dest_preg, 7); //3311
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src1_preg, 7); //3360
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src2_preg, 7); //3409
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.old_dest_preg, 7); //3458
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src1_rdata, 32); //3507
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src2_rdata, 32); //3731
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.result, 32); //3955
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.pred_br_taken, 1); //4179
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.alt_pred, 1); //4186
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.altpcpn, 8); //4193
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.pcpn, 8); //4249
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.pred_br_pc, 32); //4305
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.mispred, 1); //4529
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.br_taken, 1); //4536
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.pc_next, 32); //4543
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.dest_en, 1); //4767
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src1_en, 1); //4774
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src2_en, 1); //4781
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src1_busy, 1); //4788
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src2_busy, 1); //4795
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src1_latency, 4); //4802
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src2_latency, 4); //4830
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src1_is_pc, 1); //4858
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.src2_is_imm, 1); //4865
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.func3, 3); //4872
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.func7_5, 1); //4893
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.imm, 32); //4900
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.pc, 32); //5124
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.tag, 4); //5348
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.csr_idx, 12); //5376
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.rob_idx, 7); //5460
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.stq_idx, 4); //5509
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.pre_sta_mask, 16); //5537
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.pre_std_mask, 16); //5649
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.uop_num, 2); //5761
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.cplt_num, 2); //5775
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.rob_flag, 1); //5789
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.page_fault_inst, 1); //5796
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.page_fault_load, 1); //5803
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.page_fault_store, 1); //5810
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.illegal_inst, 1); //5817
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.type, 4); //5824
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.op, 4); //5852
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.exe2prf->entry[i].uop.amoop, 4); //5880
//     }
//     unpack_bits(cursor_pi, in.dec_bcast->mispred, 1); //5908
//     unpack_bits(cursor_pi, in.dec_bcast->br_mask, 16); //5909
//     unpack_bits(cursor_pi, in.dec_bcast->br_tag, 4); //5925
//     unpack_bits(cursor_pi, in.dec_bcast->redirect_rob_idx, 7); //5929
//     unpack_bits(cursor_pi, in.rob_bcast->flush, 1); //5936
//     unpack_bits(cursor_pi, in.rob_bcast->mret, 1); //5937
//     unpack_bits(cursor_pi, in.rob_bcast->sret, 1); //5938
//     unpack_bits(cursor_pi, in.rob_bcast->ecall, 1); //5939
//     unpack_bits(cursor_pi, in.rob_bcast->exception, 1); //5940
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_inst, 1); //5941
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_load, 1); //5942
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_store, 1); //5943
//     unpack_bits(cursor_pi, in.rob_bcast->illegal_inst, 1); //5944
//     unpack_bits(cursor_pi, in.rob_bcast->interrupt, 1); //5945
//     unpack_bits(cursor_pi, in.rob_bcast->trap_val, 32); //5946
//     unpack_bits(cursor_pi, in.rob_bcast->pc, 32); //5978
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.reg_file_data_0[i], 32); //6010
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, in.reg_file_data_1[i], 32); //6234
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].valid, 1); //6458
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.instruction, 32); //6465
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.dest_areg, 6); //6689
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src1_areg, 6); //6731
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src2_areg, 6); //6773
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.dest_preg, 7); //6815
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src1_preg, 7); //6864
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src2_preg, 7); //6913
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.old_dest_preg, 7); //6962
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src1_rdata, 32); //7011
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src2_rdata, 32); //7235
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.result, 32); //7459
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.pred_br_taken, 1); //7683
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.alt_pred, 1); //7690
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.altpcpn, 8); //7697
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.pcpn, 8); //7753
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.pred_br_pc, 32); //7809
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.mispred, 1); //8033
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.br_taken, 1); //8040
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.pc_next, 32); //8047
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.dest_en, 1); //8271
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src1_en, 1); //8278
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src2_en, 1); //8285
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src1_busy, 1); //8292
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src2_busy, 1); //8299
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src1_latency, 4); //8306
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src2_latency, 4); //8334
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src1_is_pc, 1); //8362
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.src2_is_imm, 1); //8369
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.func3, 3); //8376
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.func7_5, 1); //8397
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.imm, 32); //8404
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.pc, 32); //8628
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.tag, 4); //8852
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.csr_idx, 12); //8880
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.rob_idx, 7); //8964
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.stq_idx, 4); //9013
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.pre_sta_mask, 16); //9041
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.pre_std_mask, 16); //9153
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.uop_num, 2); //9265
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.cplt_num, 2); //9279
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.rob_flag, 1); //9293
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.page_fault_inst, 1); //9300
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.page_fault_load, 1); //9307
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.page_fault_store, 1); //9314
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.illegal_inst, 1); //9321
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.type, 4); //9328
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.op, 4); //9356
//     }
//     for(int i = 0; i < 7; i++) {
//         unpack_bits(cursor_pi, inst_r[i].uop.amoop, 4); //9384
//     }
//     io_generator_outer(pi, po);
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].valid, 1, "out.prf2exe->iss_entry[i].valid", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.instruction, 32, "out.prf2exe->iss_entry[i].uop.instruction", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.dest_areg, 6, "out.prf2exe->iss_entry[i].uop.dest_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src1_areg, 6, "out.prf2exe->iss_entry[i].uop.src1_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src2_areg, 6, "out.prf2exe->iss_entry[i].uop.src2_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.dest_preg, 7, "out.prf2exe->iss_entry[i].uop.dest_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src1_preg, 7, "out.prf2exe->iss_entry[i].uop.src1_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src2_preg, 7, "out.prf2exe->iss_entry[i].uop.src2_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.old_dest_preg, 7, "out.prf2exe->iss_entry[i].uop.old_dest_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src1_rdata, 32, "out.prf2exe->iss_entry[i].uop.src1_rdata", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src2_rdata, 32, "out.prf2exe->iss_entry[i].uop.src2_rdata", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.result, 32, "out.prf2exe->iss_entry[i].uop.result", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.pred_br_taken, 1, "out.prf2exe->iss_entry[i].uop.pred_br_taken", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.alt_pred, 1, "out.prf2exe->iss_entry[i].uop.alt_pred", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.altpcpn, 8, "out.prf2exe->iss_entry[i].uop.altpcpn", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.pcpn, 8, "out.prf2exe->iss_entry[i].uop.pcpn", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.pred_br_pc, 32, "out.prf2exe->iss_entry[i].uop.pred_br_pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.mispred, 1, "out.prf2exe->iss_entry[i].uop.mispred", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.br_taken, 1, "out.prf2exe->iss_entry[i].uop.br_taken", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.pc_next, 32, "out.prf2exe->iss_entry[i].uop.pc_next", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.dest_en, 1, "out.prf2exe->iss_entry[i].uop.dest_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src1_en, 1, "out.prf2exe->iss_entry[i].uop.src1_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src2_en, 1, "out.prf2exe->iss_entry[i].uop.src2_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src1_busy, 1, "out.prf2exe->iss_entry[i].uop.src1_busy", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src2_busy, 1, "out.prf2exe->iss_entry[i].uop.src2_busy", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src1_latency, 4, "out.prf2exe->iss_entry[i].uop.src1_latency", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src2_latency, 4, "out.prf2exe->iss_entry[i].uop.src2_latency", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src1_is_pc, 1, "out.prf2exe->iss_entry[i].uop.src1_is_pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.src2_is_imm, 1, "out.prf2exe->iss_entry[i].uop.src2_is_imm", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.func3, 3, "out.prf2exe->iss_entry[i].uop.func3", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.func7_5, 1, "out.prf2exe->iss_entry[i].uop.func7_5", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.imm, 32, "out.prf2exe->iss_entry[i].uop.imm", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.pc, 32, "out.prf2exe->iss_entry[i].uop.pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.tag, 4, "out.prf2exe->iss_entry[i].uop.tag", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.csr_idx, 12, "out.prf2exe->iss_entry[i].uop.csr_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.rob_idx, 7, "out.prf2exe->iss_entry[i].uop.rob_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.stq_idx, 4, "out.prf2exe->iss_entry[i].uop.stq_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.pre_sta_mask, 16, "out.prf2exe->iss_entry[i].uop.pre_sta_mask", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.pre_std_mask, 16, "out.prf2exe->iss_entry[i].uop.pre_std_mask", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.uop_num, 2, "out.prf2exe->iss_entry[i].uop.uop_num", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.cplt_num, 2, "out.prf2exe->iss_entry[i].uop.cplt_num", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.rob_flag, 1, "out.prf2exe->iss_entry[i].uop.rob_flag", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.page_fault_inst, 1, "out.prf2exe->iss_entry[i].uop.page_fault_inst", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.page_fault_load, 1, "out.prf2exe->iss_entry[i].uop.page_fault_load", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.page_fault_store, 1, "out.prf2exe->iss_entry[i].uop.page_fault_store", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.illegal_inst, 1, "out.prf2exe->iss_entry[i].uop.illegal_inst", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.type, 4, "out.prf2exe->iss_entry[i].uop.type", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.op, 4, "out.prf2exe->iss_entry[i].uop.op", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->iss_entry[i].uop.amoop, 4, "out.prf2exe->iss_entry[i].uop.amoop", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2exe->ready[i], 1, "out.prf2exe->ready[i]", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].valid, 1, "out.prf2rob->entry[i].valid", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.instruction, 32, "out.prf2rob->entry[i].uop.instruction", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.dest_areg, 6, "out.prf2rob->entry[i].uop.dest_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src1_areg, 6, "out.prf2rob->entry[i].uop.src1_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src2_areg, 6, "out.prf2rob->entry[i].uop.src2_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.dest_preg, 7, "out.prf2rob->entry[i].uop.dest_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src1_preg, 7, "out.prf2rob->entry[i].uop.src1_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src2_preg, 7, "out.prf2rob->entry[i].uop.src2_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.old_dest_preg, 7, "out.prf2rob->entry[i].uop.old_dest_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src1_rdata, 32, "out.prf2rob->entry[i].uop.src1_rdata", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src2_rdata, 32, "out.prf2rob->entry[i].uop.src2_rdata", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.result, 32, "out.prf2rob->entry[i].uop.result", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.pred_br_taken, 1, "out.prf2rob->entry[i].uop.pred_br_taken", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.alt_pred, 1, "out.prf2rob->entry[i].uop.alt_pred", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.altpcpn, 8, "out.prf2rob->entry[i].uop.altpcpn", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.pcpn, 8, "out.prf2rob->entry[i].uop.pcpn", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.pred_br_pc, 32, "out.prf2rob->entry[i].uop.pred_br_pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.mispred, 1, "out.prf2rob->entry[i].uop.mispred", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.br_taken, 1, "out.prf2rob->entry[i].uop.br_taken", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.pc_next, 32, "out.prf2rob->entry[i].uop.pc_next", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.dest_en, 1, "out.prf2rob->entry[i].uop.dest_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src1_en, 1, "out.prf2rob->entry[i].uop.src1_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src2_en, 1, "out.prf2rob->entry[i].uop.src2_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src1_busy, 1, "out.prf2rob->entry[i].uop.src1_busy", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src2_busy, 1, "out.prf2rob->entry[i].uop.src2_busy", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src1_latency, 4, "out.prf2rob->entry[i].uop.src1_latency", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src2_latency, 4, "out.prf2rob->entry[i].uop.src2_latency", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src1_is_pc, 1, "out.prf2rob->entry[i].uop.src1_is_pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.src2_is_imm, 1, "out.prf2rob->entry[i].uop.src2_is_imm", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.func3, 3, "out.prf2rob->entry[i].uop.func3", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.func7_5, 1, "out.prf2rob->entry[i].uop.func7_5", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.imm, 32, "out.prf2rob->entry[i].uop.imm", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.pc, 32, "out.prf2rob->entry[i].uop.pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.tag, 4, "out.prf2rob->entry[i].uop.tag", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.csr_idx, 12, "out.prf2rob->entry[i].uop.csr_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.rob_idx, 7, "out.prf2rob->entry[i].uop.rob_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.stq_idx, 4, "out.prf2rob->entry[i].uop.stq_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.pre_sta_mask, 16, "out.prf2rob->entry[i].uop.pre_sta_mask", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.pre_std_mask, 16, "out.prf2rob->entry[i].uop.pre_std_mask", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.uop_num, 2, "out.prf2rob->entry[i].uop.uop_num", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.cplt_num, 2, "out.prf2rob->entry[i].uop.cplt_num", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.rob_flag, 1, "out.prf2rob->entry[i].uop.rob_flag", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.page_fault_inst, 1, "out.prf2rob->entry[i].uop.page_fault_inst", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.page_fault_load, 1, "out.prf2rob->entry[i].uop.page_fault_load", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.page_fault_store, 1, "out.prf2rob->entry[i].uop.page_fault_store", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.illegal_inst, 1, "out.prf2rob->entry[i].uop.illegal_inst", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.type, 4, "out.prf2rob->entry[i].uop.type", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.op, 4, "out.prf2rob->entry[i].uop.op", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.prf2rob->entry[i].uop.amoop, 4, "out.prf2rob->entry[i].uop.amoop", pi, 9412);
//     }
//     compare_and_pack(cursor_po, out.prf2dec->mispred, 1, "out.prf2dec->mispred", pi, 9412);
//     compare_and_pack(cursor_po, out.prf2dec->redirect_pc, 32, "out.prf2dec->redirect_pc", pi, 9412);
//     compare_and_pack(cursor_po, out.prf2dec->redirect_rob_idx, 7, "out.prf2dec->redirect_rob_idx", pi, 9412);
//     compare_and_pack(cursor_po, out.prf2dec->br_tag, 4, "out.prf2dec->br_tag", pi, 9412);
//     compare_and_pack(cursor_po, out.prf_awake->wake.valid, 1, "out.prf_awake->wake.valid", pi, 9412);
//     compare_and_pack(cursor_po, out.prf_awake->wake.preg, 7, "out.prf_awake->wake.preg", pi, 9412);
//     compare_and_pack(cursor_po, out.prf_awake->wake.latency, 2, "out.prf_awake->wake.latency", pi, 9412);
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.reg_file_addr_0[i], 7, "out.reg_file_addr_0[i]", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, out.reg_file_addr_1[i], 7, "out.reg_file_addr_1[i]", pi, 9412);
//     }
//     for(int i = 0; i < 3; i++) {
//         compare_and_pack(cursor_po, out.reg_file_2_en[i], 1, "out.reg_file_2_en[i]", pi, 9412);
//     }
//     for(int i = 0; i < 3; i++) {
//         compare_and_pack(cursor_po, out.reg_file_2_addr[i], 7, "out.reg_file_2_addr[i]", pi, 9412);
//     }
//     for(int i = 0; i < 3; i++) {
//         compare_and_pack(cursor_po, out.reg_file_2_data[i], 32, "out.reg_file_2_data[i]", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].valid, 1, "inst_r_1[i].valid", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.instruction, 32, "inst_r_1[i].uop.instruction", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.dest_areg, 6, "inst_r_1[i].uop.dest_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src1_areg, 6, "inst_r_1[i].uop.src1_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src2_areg, 6, "inst_r_1[i].uop.src2_areg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.dest_preg, 7, "inst_r_1[i].uop.dest_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src1_preg, 7, "inst_r_1[i].uop.src1_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src2_preg, 7, "inst_r_1[i].uop.src2_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.old_dest_preg, 7, "inst_r_1[i].uop.old_dest_preg", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src1_rdata, 32, "inst_r_1[i].uop.src1_rdata", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src2_rdata, 32, "inst_r_1[i].uop.src2_rdata", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.result, 32, "inst_r_1[i].uop.result", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.pred_br_taken, 1, "inst_r_1[i].uop.pred_br_taken", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.alt_pred, 1, "inst_r_1[i].uop.alt_pred", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.altpcpn, 8, "inst_r_1[i].uop.altpcpn", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.pcpn, 8, "inst_r_1[i].uop.pcpn", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.pred_br_pc, 32, "inst_r_1[i].uop.pred_br_pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.mispred, 1, "inst_r_1[i].uop.mispred", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.br_taken, 1, "inst_r_1[i].uop.br_taken", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.pc_next, 32, "inst_r_1[i].uop.pc_next", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.dest_en, 1, "inst_r_1[i].uop.dest_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src1_en, 1, "inst_r_1[i].uop.src1_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src2_en, 1, "inst_r_1[i].uop.src2_en", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src1_busy, 1, "inst_r_1[i].uop.src1_busy", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src2_busy, 1, "inst_r_1[i].uop.src2_busy", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src1_latency, 4, "inst_r_1[i].uop.src1_latency", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src2_latency, 4, "inst_r_1[i].uop.src2_latency", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src1_is_pc, 1, "inst_r_1[i].uop.src1_is_pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.src2_is_imm, 1, "inst_r_1[i].uop.src2_is_imm", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.func3, 3, "inst_r_1[i].uop.func3", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.func7_5, 1, "inst_r_1[i].uop.func7_5", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.imm, 32, "inst_r_1[i].uop.imm", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.pc, 32, "inst_r_1[i].uop.pc", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.tag, 4, "inst_r_1[i].uop.tag", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.csr_idx, 12, "inst_r_1[i].uop.csr_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.rob_idx, 7, "inst_r_1[i].uop.rob_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.stq_idx, 4, "inst_r_1[i].uop.stq_idx", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.pre_sta_mask, 16, "inst_r_1[i].uop.pre_sta_mask", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.pre_std_mask, 16, "inst_r_1[i].uop.pre_std_mask", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.uop_num, 2, "inst_r_1[i].uop.uop_num", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.cplt_num, 2, "inst_r_1[i].uop.cplt_num", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.rob_flag, 1, "inst_r_1[i].uop.rob_flag", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.page_fault_inst, 1, "inst_r_1[i].uop.page_fault_inst", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.page_fault_load, 1, "inst_r_1[i].uop.page_fault_load", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.page_fault_store, 1, "inst_r_1[i].uop.page_fault_store", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.illegal_inst, 1, "inst_r_1[i].uop.illegal_inst", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.type, 4, "inst_r_1[i].uop.type", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.op, 4, "inst_r_1[i].uop.op", pi, 9412);
//     }
//     for(int i = 0; i < 7; i++) {
//         compare_and_pack(cursor_po, inst_r_1[i].uop.amoop, 4, "inst_r_1[i].uop.amoop", pi, 9412);
//     }
// }

#endif