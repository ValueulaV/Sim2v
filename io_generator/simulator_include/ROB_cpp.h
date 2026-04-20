#ifndef ROB_CPP_H
#define ROB_CPP_H
#include "IO.h"
// #include <RISCV.h>
#include <ROB.h>
// #include <TOP.h>
#include <cmath>
#include <config.h>
#include <cstdlib>
#include <iostream>
#include <util.h>
extern uint64_t vcd_time;
// extern Back_Top back;
void ROB::init() {
  deq_ptr = deq_ptr_1 = 0;
  enq_ptr = enq_ptr_1 = 0;
  count = count_1 = 0;
  flag = flag_1 = false;

  for (int i = 0; i < ROB_LINE_NUM; i++) {
    for (int j = 0; j < ROB_BANK_NUM; j++) {
      entry[j][i].valid = false;
    }
  }
}

void ROB::comb_ready() {
  out.rob2dis->stall = false;
  out.rob2csr->commit = false;
  out.rob2csr->interrupt_resp = false;

  for (int i = 0; i < ROB_BANK_NUM; i++) {
    if (entry[i][deq_ptr].valid && is_flush_inst(entry[i][deq_ptr].uop)) {
      out.rob2dis->stall = true;
      break;
    }
  }

  if (count != 0 && in.csr2rob->interrupt_req && !in.dec_bcast->mispred) {
    for (int i = 0; i < ROB_BANK_NUM; i++) {
      if (entry[i][deq_ptr].valid) {
        out.rob2csr->interrupt_resp = true;
        break;
      }
    }
  }
  out.rob2dis->empty = (count == 0);
  out.rob2dis->ready = !(enq_ptr == deq_ptr && count != 0);
}

void ROB::comb_commit() {

//   static int stall_cycle = 0; // 检查是否卡死
  out.rob_bcast->flush = out.rob_bcast->exception = out.rob_bcast->mret =
      out.rob_bcast->sret = out.rob_bcast->ecall = false;
      
  out.rob_bcast->pc = 0;
  out.rob_bcast->trap_val = 0;
  out.rob_bcast->interrupt = out.rob2csr->interrupt_resp;

  out.rob_bcast->page_fault_inst = out.rob_bcast->page_fault_load =
      out.rob_bcast->page_fault_store = out.rob_bcast->illegal_inst = false;

  wire1_t commit =
      (!(enq_ptr == deq_ptr && count == 0) && !in.dec_bcast->mispred);

  // bank的同一行是否都完成
  for (int i = 0; i < ROB_BANK_NUM; i++) {
    commit = commit &&
             (!entry[i][deq_ptr].valid || (entry[i][deq_ptr].uop.cplt_num ==
                                           entry[i][deq_ptr].uop.uop_num));
  }

  // 出队一行存在特殊指令则single commit
  wire1_t single_commit = false;
  wire2_t single_idx;

  if (!in.dec_bcast->mispred) {
    for (int i = 0; i < ROB_BANK_NUM; i++) {
      if (entry[i][deq_ptr].valid && is_flush_inst(entry[i][deq_ptr].uop) ||
          out.rob2csr->interrupt_resp) {
        single_commit = true;
        break;
      }
    }

    // 看第一个valid的inst是否完成 或者是interrupt，如果完成则single_commit
    for (int i = 0; i < ROB_BANK_NUM; i++) {
      if (entry[i][deq_ptr].valid) {
        single_idx = i;
        if (!out.rob_bcast->interrupt &&
            entry[i][deq_ptr].uop.cplt_num != entry[i][deq_ptr].uop.uop_num) {
          single_commit = false;
        }
        break;
      }
    }
  }

  for (int i = 0; i < ROB_BANK_NUM; i++) {
    out.rob_commit->commit_entry[i].uop = entry[i][deq_ptr].uop;
  }

  // 一组提交
  if (commit && !single_commit) {
    for (int i = 0; i < ROB_BANK_NUM; i++) {
      out.rob_commit->commit_entry[i].valid = entry[i][deq_ptr].valid;
    }

    // stall_cycle = 0;
    LOOP_INC(deq_ptr_1, ROB_LINE_NUM);
    count_1--;

  } else if (single_commit) {
    // stall_cycle = 0;
    for (int i = 0; i < ROB_BANK_NUM; i++) {
      if (i == single_idx)
        out.rob_commit->commit_entry[i].valid = true;
      else
        out.rob_commit->commit_entry[i].valid = false;
    }

    entry_1[single_idx][deq_ptr].valid = false;
    if (is_flush_inst(entry[single_idx][deq_ptr].uop) ||
        out.rob2csr->interrupt_resp) {
      out.rob_bcast->flush = true;
      out.rob_bcast->exception = is_exception(entry[single_idx][deq_ptr].uop) ||
                                 out.rob2csr->interrupt_resp;
      out.rob_bcast->pc = out.rob_commit->commit_entry[single_idx].uop.pc;

      if (out.rob2csr->interrupt_resp) {
        // interrupt拥有最高优先级
      } else if (entry[single_idx][deq_ptr].uop.type == ECALL) {
        out.rob_bcast->ecall = true;
        out.rob_bcast->pc = out.rob_commit->commit_entry[single_idx].uop.pc;
      } else if (entry[single_idx][deq_ptr].uop.type == MRET) {
        out.rob_bcast->mret = true;
      } else if (entry[single_idx][deq_ptr].uop.type == SRET) {
        out.rob_bcast->sret = true;
      } else if (entry[single_idx][deq_ptr].uop.page_fault_store) {
        out.rob_bcast->page_fault_store = true;
        out.rob_bcast->trap_val = entry[single_idx][deq_ptr].uop.result;
      } else if (entry[single_idx][deq_ptr].uop.page_fault_load) {
        out.rob_bcast->page_fault_load = true;
        out.rob_bcast->trap_val = entry[single_idx][deq_ptr].uop.result;
      } else if (entry[single_idx][deq_ptr].uop.page_fault_inst) {
        out.rob_bcast->page_fault_inst = true;
        out.rob_bcast->trap_val = entry[single_idx][deq_ptr].uop.pc;
      } else if (entry[single_idx][deq_ptr].uop.illegal_inst) {
        out.rob_bcast->illegal_inst = true;
        out.rob_bcast->trap_val = entry[single_idx][deq_ptr].uop.instruction;
      } else if (entry[single_idx][deq_ptr].uop.type == EBREAK) {
        // extern bool sim_end;
        // sim_end = true;
      } else if (entry[single_idx][deq_ptr].uop.type == CSR) {
        out.rob2csr->commit = true;
      } else {
        if (entry[single_idx][deq_ptr].uop.type != CSR &&
            entry[single_idx][deq_ptr].uop.type != SFENCE_VMA) {
          cout << hex << entry[single_idx][deq_ptr].uop.instruction << endl;
          exit(1);
        }
      }
    }
  } else {
    for (int i = 0; i < ROB_BANK_NUM; i++) {
      out.rob_commit->commit_entry[i].valid = false;
    }
  }

  out.rob2dis->enq_idx = enq_ptr;
  out.rob2dis->rob_flag = flag;

//   stall_cycle++;
//   if (stall_cycle > 1000) {
//     cout << dec << sim_time << endl;
//     cout << "卡死了" << endl;

//     // 打印ROB出队行指令 看是哪条指令卡死
//     cout << "ROB deq inst:" << endl;
//     for (int i = 0; i < ROB_BANK_NUM; i++) {
//       if (entry[i][deq_ptr].valid) {
//         cout << hex << entry[i][deq_ptr].uop.instruction
//              << " cplt_num: " << entry[i][deq_ptr].uop.cplt_num
//              << "is_page_fault: " << is_page_fault(entry[i][deq_ptr].uop)
//              << endl;
//       }
//     }
//     exit(1);
//   }
}

void ROB::comb_complete() {
  //  执行完毕的标记
  for (int i = 0; i < ISSUE_WAY; i++) {
    if (in.prf2rob->entry[i].valid) {
      int bank_idx = in.prf2rob->entry[i].uop.rob_idx & 0b11;
      int line_idx = in.prf2rob->entry[i].uop.rob_idx >> 2;
      entry_1[bank_idx][line_idx].uop.cplt_num++;

      if (i == IQ_LD) {
        if (is_page_fault(in.prf2rob->entry[i].uop)) {
          entry_1[bank_idx][line_idx].uop.result =
              in.prf2rob->entry[i].uop.result;
          entry_1[bank_idx][line_idx].uop.page_fault_load = true;
        }
      }

      if (i == IQ_STA) {
        if (is_page_fault(in.prf2rob->entry[i].uop)) {
          entry_1[bank_idx][line_idx].uop.result =
              in.prf2rob->entry[i].uop.result;
          entry_1[bank_idx][line_idx].uop.page_fault_store = true;
        }
      }

      // for debug
    //   entry_1[bank_idx][line_idx].uop.difftest_skip =
    //       in.prf2rob->entry[i].uop.difftest_skip;
      if (i == IQ_BR0 || i == IQ_BR1) {
        entry_1[bank_idx][line_idx].uop.pc_next =
            in.prf2rob->entry[i].uop.pc_next;
        entry_1[bank_idx][line_idx].uop.mispred =
            in.prf2rob->entry[i].uop.mispred;
        entry_1[bank_idx][line_idx].uop.br_taken =
            in.prf2rob->entry[i].uop.br_taken;
      }
    }
  }
}

void ROB::comb_branch() {
  // 分支预测失败
  if (in.dec_bcast->mispred && !out.rob_bcast->flush) {
    enq_ptr_1 = ((in.dec_bcast->redirect_rob_idx >> 2) + 1) % (ROB_LINE_NUM);
    count_1 = count - (enq_ptr + ROB_LINE_NUM - enq_ptr_1) % ROB_LINE_NUM;

    if (enq_ptr_1 > enq_ptr) {
      flag_1 = !flag;
    }

    for (int i = (in.dec_bcast->redirect_rob_idx & 0b11) + 1; i < ROB_BANK_NUM;
         i++) {
      entry_1[i][in.dec_bcast->redirect_rob_idx >> 2].valid = false;
    }
  }
}

void ROB::comb_fire() {
  // 入队
  wire1_t enq = false;
  if (out.rob2dis->ready) {
    for (int i = 0; i < FETCH_WIDTH; i++) {
      if (in.dis2rob->dis_fire[i]) {
        entry_1[i][enq_ptr].valid = true;
        entry_1[i][enq_ptr].uop = in.dis2rob->uop[i];
        entry_1[i][enq_ptr].uop.cplt_num = 0;
        enq = true;
      } else {
        entry_1[i][enq_ptr].valid = false;
      }
    }
  }

  if (enq) {
    LOOP_INC(enq_ptr_1, ROB_LINE_NUM);
    count_1++;
    if (enq_ptr_1 == 0) {
      flag_1 = !flag;
    }
  }
}

void ROB::comb_flush() {
  if (out.rob_bcast->flush) {
    for (int i = 0; i < ROB_BANK_NUM; i++) {
      for (int j = 0; j < ROB_LINE_NUM; j++) {
        entry_1[i][j].valid = false;
      }
    }

    enq_ptr_1 = 0;
    deq_ptr_1 = 0;
    count_1 = 0;
    flag_1 = false;
  }
}

void ROB::seq() {
  for (int i = 0; i < ROB_BANK_NUM; i++) {
    for (int j = 0; j < ROB_LINE_NUM; j++) {
      entry[i][j] = entry_1[i][j];
    }
  }

  deq_ptr = deq_ptr_1;
  enq_ptr = enq_ptr_1;
  count = count_1;
  flag = flag_1;
}
template <typename T>
inline T pack_bits(const bool* cursor, int width) {
    T val = 0;
    // 编译器会自动展开这个循环，对于 width=1, 4, 32 等常数非常快
    for (int i = 0; i < width; i++) {
        // 使用 | 而不是 +=，且不需要 if 判断，利用 bool 为 0/1 的特性
        val |= (static_cast<T>(cursor[i]) << i);
    }
    cursor += width; // 游标自动前进
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
    cursor += width; // 游标前进
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
inline void compare_and_pack(const bool*& cursor, T& target_var, int width, const char* var_name, const bool* pi_array, int pi_size) {
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
void ROB::pi_to_simulator(bool* pi) {
    const bool* cursor = pi;
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].instruction = pack_bits<uint32_t>(cursor, 32); //0
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].dest_areg = pack_bits<uint8_t>(cursor, 6); //128
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src1_areg = pack_bits<uint8_t>(cursor, 6); //152
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src2_areg = pack_bits<uint8_t>(cursor, 6); //176
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].dest_preg = pack_bits<uint8_t>(cursor, 7); //200
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src1_preg = pack_bits<uint8_t>(cursor, 7); //228
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src2_preg = pack_bits<uint8_t>(cursor, 7); //256
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].old_dest_preg = pack_bits<uint8_t>(cursor, 7); //284
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src1_rdata = pack_bits<uint32_t>(cursor, 32); //312
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src2_rdata = pack_bits<uint32_t>(cursor, 32); //440
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].result = pack_bits<uint32_t>(cursor, 32); //568
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].pred_br_taken = pack_bits<bool>(cursor, 1); //696
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].alt_pred = pack_bits<bool>(cursor, 1); //700
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].altpcpn = pack_bits<uint8_t>(cursor, 8); //704
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].pcpn = pack_bits<uint8_t>(cursor, 8); //736
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].pred_br_pc = pack_bits<uint32_t>(cursor, 32); //768
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].mispred = pack_bits<bool>(cursor, 1); //896
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].br_taken = pack_bits<bool>(cursor, 1); //900
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].pc_next = pack_bits<uint32_t>(cursor, 32); //904
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].dest_en = pack_bits<bool>(cursor, 1); //1032
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src1_en = pack_bits<bool>(cursor, 1); //1036
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src2_en = pack_bits<bool>(cursor, 1); //1040
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src1_busy = pack_bits<bool>(cursor, 1); //1044
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src2_busy = pack_bits<bool>(cursor, 1); //1048
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src1_latency = pack_bits<uint8_t>(cursor, 4); //1052
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src2_latency = pack_bits<uint8_t>(cursor, 4); //1068
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src1_is_pc = pack_bits<bool>(cursor, 1); //1084
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].src2_is_imm = pack_bits<bool>(cursor, 1); //1088
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].func3 = pack_bits<uint8_t>(cursor, 3); //1092
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].func7_5 = pack_bits<bool>(cursor, 1); //1104
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].imm = pack_bits<uint32_t>(cursor, 32); //1108
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].pc = pack_bits<uint32_t>(cursor, 32); //1236
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].tag = pack_bits<uint8_t>(cursor, 4); //1364
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].csr_idx = pack_bits<uint16_t>(cursor, 12); //1380
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].rob_idx = pack_bits<uint8_t>(cursor, 7); //1428
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].stq_idx = pack_bits<uint8_t>(cursor, 4); //1456
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].pre_sta_mask = pack_bits<uint16_t>(cursor, 16); //1472
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].pre_std_mask = pack_bits<uint16_t>(cursor, 16); //1536
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].uop_num = pack_bits<uint8_t>(cursor, 2); //1600
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].cplt_num = pack_bits<uint8_t>(cursor, 2); //1608
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].rob_flag = pack_bits<bool>(cursor, 1); //1616
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].page_fault_inst = pack_bits<bool>(cursor, 1); //1620
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].page_fault_load = pack_bits<bool>(cursor, 1); //1624
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].page_fault_store = pack_bits<bool>(cursor, 1); //1628
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].illegal_inst = pack_bits<bool>(cursor, 1); //1632
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].type = pack_bits<uint8_t>(cursor, 4); //1636
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].op = decode_uop_type(pack_bits<uint8_t>(cursor, 4)); //1652
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->uop[i].amoop = pack_bits<uint8_t>(cursor, 4); //1668
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->valid[i] = pack_bits<bool>(cursor, 1); //1684
    }
    for(int i = 0; i < 4; i++) {
        in.dis2rob->dis_fire[i] = pack_bits<bool>(cursor, 1); //1688
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].valid = pack_bits<bool>(cursor, 1); //1692
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.instruction = pack_bits<uint32_t>(cursor, 32); //1699
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.dest_areg = pack_bits<uint8_t>(cursor, 6); //1923
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src1_areg = pack_bits<uint8_t>(cursor, 6); //1965
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src2_areg = pack_bits<uint8_t>(cursor, 6); //2007
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.dest_preg = pack_bits<uint8_t>(cursor, 7); //2049
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src1_preg = pack_bits<uint8_t>(cursor, 7); //2098
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src2_preg = pack_bits<uint8_t>(cursor, 7); //2147
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.old_dest_preg = pack_bits<uint8_t>(cursor, 7); //2196
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src1_rdata = pack_bits<uint32_t>(cursor, 32); //2245
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src2_rdata = pack_bits<uint32_t>(cursor, 32); //2469
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.result = pack_bits<uint32_t>(cursor, 32); //2693
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.pred_br_taken = pack_bits<bool>(cursor, 1); //2917
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.alt_pred = pack_bits<bool>(cursor, 1); //2924
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.altpcpn = pack_bits<uint8_t>(cursor, 8); //2931
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.pcpn = pack_bits<uint8_t>(cursor, 8); //2987
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.pred_br_pc = pack_bits<uint32_t>(cursor, 32); //3043
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.mispred = pack_bits<bool>(cursor, 1); //3267
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.br_taken = pack_bits<bool>(cursor, 1); //3274
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.pc_next = pack_bits<uint32_t>(cursor, 32); //3281
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.dest_en = pack_bits<bool>(cursor, 1); //3505
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src1_en = pack_bits<bool>(cursor, 1); //3512
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src2_en = pack_bits<bool>(cursor, 1); //3519
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src1_busy = pack_bits<bool>(cursor, 1); //3526
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src2_busy = pack_bits<bool>(cursor, 1); //3533
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src1_latency = pack_bits<uint8_t>(cursor, 4); //3540
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src2_latency = pack_bits<uint8_t>(cursor, 4); //3568
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //3596
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //3603
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.func3 = pack_bits<uint8_t>(cursor, 3); //3610
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.func7_5 = pack_bits<bool>(cursor, 1); //3631
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.imm = pack_bits<uint32_t>(cursor, 32); //3638
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.pc = pack_bits<uint32_t>(cursor, 32); //3862
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.tag = pack_bits<uint8_t>(cursor, 4); //4086
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //4114
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.rob_idx = pack_bits<uint8_t>(cursor, 7); //4198
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //4247
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.pre_sta_mask = pack_bits<uint16_t>(cursor, 16); //4275
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.pre_std_mask = pack_bits<uint16_t>(cursor, 16); //4387
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.uop_num = pack_bits<uint8_t>(cursor, 2); //4499
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.cplt_num = pack_bits<uint8_t>(cursor, 2); //4513
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.rob_flag = pack_bits<bool>(cursor, 1); //4527
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.page_fault_inst = pack_bits<bool>(cursor, 1); //4534
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.page_fault_load = pack_bits<bool>(cursor, 1); //4541
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.page_fault_store = pack_bits<bool>(cursor, 1); //4548
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.illegal_inst = pack_bits<bool>(cursor, 1); //4555
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.type = pack_bits<uint8_t>(cursor, 4); //4562
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.op = decode_uop_type(pack_bits<uint8_t>(cursor, 4)); //4590
    }
    for(int i = 0; i < 7; i++) {
        in.prf2rob->entry[i].uop.amoop = pack_bits<uint8_t>(cursor, 4); //4618
    }
    in.csr2rob->interrupt_req = pack_bits<bool>(cursor, 1); //4646
    in.dec_bcast->mispred = pack_bits<bool>(cursor, 1); //4647
    in.dec_bcast->br_mask = pack_bits<uint16_t>(cursor, 16); //4648
    in.dec_bcast->br_tag = pack_bits<uint8_t>(cursor, 4); //4664
    in.dec_bcast->redirect_rob_idx = pack_bits<uint8_t>(cursor, 7); //4668
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].valid = pack_bits<bool>(cursor, 1); //4675
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.instruction = pack_bits<uint32_t>(cursor, 32); //4679
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.dest_areg = pack_bits<uint8_t>(cursor, 6); //4807
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src1_areg = pack_bits<uint8_t>(cursor, 6); //4831
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src2_areg = pack_bits<uint8_t>(cursor, 6); //4855
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.dest_preg = pack_bits<uint8_t>(cursor, 7); //4879
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src1_preg = pack_bits<uint8_t>(cursor, 7); //4907
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src2_preg = pack_bits<uint8_t>(cursor, 7); //4935
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.old_dest_preg = pack_bits<uint8_t>(cursor, 7); //4963
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src1_rdata = pack_bits<uint32_t>(cursor, 32); //4991
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src2_rdata = pack_bits<uint32_t>(cursor, 32); //5119
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.result = pack_bits<uint32_t>(cursor, 32); //5247
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.pred_br_taken = pack_bits<bool>(cursor, 1); //5375
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.alt_pred = pack_bits<bool>(cursor, 1); //5379
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.altpcpn = pack_bits<uint8_t>(cursor, 8); //5383
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.pcpn = pack_bits<uint8_t>(cursor, 8); //5415
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.pred_br_pc = pack_bits<uint32_t>(cursor, 32); //5447
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.mispred = pack_bits<bool>(cursor, 1); //5575
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.br_taken = pack_bits<bool>(cursor, 1); //5579
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.pc_next = pack_bits<uint32_t>(cursor, 32); //5583
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.dest_en = pack_bits<bool>(cursor, 1); //5711
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src1_en = pack_bits<bool>(cursor, 1); //5715
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src2_en = pack_bits<bool>(cursor, 1); //5719
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src1_busy = pack_bits<bool>(cursor, 1); //5723
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src2_busy = pack_bits<bool>(cursor, 1); //5727
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src1_latency = pack_bits<uint8_t>(cursor, 4); //5731
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src2_latency = pack_bits<uint8_t>(cursor, 4); //5747
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //5763
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //5767
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.func3 = pack_bits<uint8_t>(cursor, 3); //5771
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.func7_5 = pack_bits<bool>(cursor, 1); //5783
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.imm = pack_bits<uint32_t>(cursor, 32); //5787
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.pc = pack_bits<uint32_t>(cursor, 32); //5915
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.tag = pack_bits<uint8_t>(cursor, 4); //6043
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //6059
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.rob_idx = pack_bits<uint8_t>(cursor, 7); //6107
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //6135
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.pre_sta_mask = pack_bits<uint16_t>(cursor, 16); //6151
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.pre_std_mask = pack_bits<uint16_t>(cursor, 16); //6215
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.uop_num = pack_bits<uint8_t>(cursor, 2); //6279
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.cplt_num = pack_bits<uint8_t>(cursor, 2); //6287
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.rob_flag = pack_bits<bool>(cursor, 1); //6295
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.page_fault_inst = pack_bits<bool>(cursor, 1); //6299
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.page_fault_load = pack_bits<bool>(cursor, 1); //6303
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.page_fault_store = pack_bits<bool>(cursor, 1); //6307
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.illegal_inst = pack_bits<bool>(cursor, 1); //6311
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.type = pack_bits<uint8_t>(cursor, 4); //6315
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.op = decode_uop_type(pack_bits<uint8_t>(cursor, 4)); //6331
    }
    for(int i = 0; i < 4; i++) {
        in.entry_data[i].uop.amoop = pack_bits<uint8_t>(cursor, 4); //6347
    }
    in.entry_single_idx_data.valid = pack_bits<bool>(cursor, 1); //6363
    in.entry_single_idx_data.uop.instruction = pack_bits<uint32_t>(cursor, 32); //6364
    in.entry_single_idx_data.uop.dest_areg = pack_bits<uint8_t>(cursor, 6); //6396
    in.entry_single_idx_data.uop.src1_areg = pack_bits<uint8_t>(cursor, 6); //6402
    in.entry_single_idx_data.uop.src2_areg = pack_bits<uint8_t>(cursor, 6); //6408
    in.entry_single_idx_data.uop.dest_preg = pack_bits<uint8_t>(cursor, 7); //6414
    in.entry_single_idx_data.uop.src1_preg = pack_bits<uint8_t>(cursor, 7); //6421
    in.entry_single_idx_data.uop.src2_preg = pack_bits<uint8_t>(cursor, 7); //6428
    in.entry_single_idx_data.uop.old_dest_preg = pack_bits<uint8_t>(cursor, 7); //6435
    in.entry_single_idx_data.uop.src1_rdata = pack_bits<uint32_t>(cursor, 32); //6442
    in.entry_single_idx_data.uop.src2_rdata = pack_bits<uint32_t>(cursor, 32); //6474
    in.entry_single_idx_data.uop.result = pack_bits<uint32_t>(cursor, 32); //6506
    in.entry_single_idx_data.uop.pred_br_taken = pack_bits<bool>(cursor, 1); //6538
    in.entry_single_idx_data.uop.alt_pred = pack_bits<bool>(cursor, 1); //6539
    in.entry_single_idx_data.uop.altpcpn = pack_bits<uint8_t>(cursor, 8); //6540
    in.entry_single_idx_data.uop.pcpn = pack_bits<uint8_t>(cursor, 8); //6548
    in.entry_single_idx_data.uop.pred_br_pc = pack_bits<uint32_t>(cursor, 32); //6556
    in.entry_single_idx_data.uop.mispred = pack_bits<bool>(cursor, 1); //6588
    in.entry_single_idx_data.uop.br_taken = pack_bits<bool>(cursor, 1); //6589
    in.entry_single_idx_data.uop.pc_next = pack_bits<uint32_t>(cursor, 32); //6590
    in.entry_single_idx_data.uop.dest_en = pack_bits<bool>(cursor, 1); //6622
    in.entry_single_idx_data.uop.src1_en = pack_bits<bool>(cursor, 1); //6623
    in.entry_single_idx_data.uop.src2_en = pack_bits<bool>(cursor, 1); //6624
    in.entry_single_idx_data.uop.src1_busy = pack_bits<bool>(cursor, 1); //6625
    in.entry_single_idx_data.uop.src2_busy = pack_bits<bool>(cursor, 1); //6626
    in.entry_single_idx_data.uop.src1_latency = pack_bits<uint8_t>(cursor, 4); //6627
    in.entry_single_idx_data.uop.src2_latency = pack_bits<uint8_t>(cursor, 4); //6631
    in.entry_single_idx_data.uop.src1_is_pc = pack_bits<bool>(cursor, 1); //6635
    in.entry_single_idx_data.uop.src2_is_imm = pack_bits<bool>(cursor, 1); //6636
    in.entry_single_idx_data.uop.func3 = pack_bits<uint8_t>(cursor, 3); //6637
    in.entry_single_idx_data.uop.func7_5 = pack_bits<bool>(cursor, 1); //6640
    in.entry_single_idx_data.uop.imm = pack_bits<uint32_t>(cursor, 32); //6641
    in.entry_single_idx_data.uop.pc = pack_bits<uint32_t>(cursor, 32); //6673
    in.entry_single_idx_data.uop.tag = pack_bits<uint8_t>(cursor, 4); //6705
    in.entry_single_idx_data.uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //6709
    in.entry_single_idx_data.uop.rob_idx = pack_bits<uint8_t>(cursor, 7); //6721
    in.entry_single_idx_data.uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //6728
    in.entry_single_idx_data.uop.pre_sta_mask = pack_bits<uint16_t>(cursor, 16); //6732
    in.entry_single_idx_data.uop.pre_std_mask = pack_bits<uint16_t>(cursor, 16); //6748
    in.entry_single_idx_data.uop.uop_num = pack_bits<uint8_t>(cursor, 2); //6764
    in.entry_single_idx_data.uop.cplt_num = pack_bits<uint8_t>(cursor, 2); //6766
    in.entry_single_idx_data.uop.rob_flag = pack_bits<bool>(cursor, 1); //6768
    in.entry_single_idx_data.uop.page_fault_inst = pack_bits<bool>(cursor, 1); //6769
    in.entry_single_idx_data.uop.page_fault_load = pack_bits<bool>(cursor, 1); //6770
    in.entry_single_idx_data.uop.page_fault_store = pack_bits<bool>(cursor, 1); //6771
    in.entry_single_idx_data.uop.illegal_inst = pack_bits<bool>(cursor, 1); //6772
    in.entry_single_idx_data.uop.type = pack_bits<uint8_t>(cursor, 4); //6773
    in.entry_single_idx_data.uop.op = decode_uop_type(pack_bits<uint8_t>(cursor, 4)); //6777
    in.entry_single_idx_data.uop.amoop = pack_bits<uint8_t>(cursor, 4); //6781
    enq_ptr = pack_bits<uint8_t>(cursor, 5); //6785
    deq_ptr = pack_bits<uint8_t>(cursor, 5); //6790
    count = pack_bits<uint8_t>(cursor, 5); //6795
    flag = pack_bits<bool>(cursor, 1); //6800
}
void ROB::out_initial_detect() {
    if(out.rob2dis->ready != 0) {
        std::cout << "out.rob2dis->ready error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob2dis->empty != 0) {
        std::cout << "out.rob2dis->empty error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob2dis->stall != 0) {
        std::cout << "out.rob2dis->stall error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob2dis->enq_idx != 0) {
        std::cout << "out.rob2dis->enq_idx error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob2dis->rob_flag != 0) {
        std::cout << "out.rob2dis->rob_flag error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob2csr->interrupt_resp != 0) {
        std::cout << "out.rob2csr->interrupt_resp error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob2csr->commit != 0) {
        std::cout << "out.rob2csr->commit error, not 0!" << std::endl;
        exit(1);
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].valid != 0) {
            std::cout << "out.rob_commit->commit_entry[i].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.instruction != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.instruction error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.dest_areg != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.dest_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src1_areg != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src1_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src2_areg != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src2_areg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.dest_preg != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src1_preg != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src1_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src2_preg != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src2_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.old_dest_preg != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.old_dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src1_rdata != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src1_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src2_rdata != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src2_rdata error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.result != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.result error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.pred_br_taken != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.pred_br_taken error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.alt_pred != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.alt_pred error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.altpcpn != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.altpcpn error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.pcpn != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.pcpn error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.pred_br_pc != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.pred_br_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.mispred != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.mispred error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.br_taken != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.br_taken error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.pc_next != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.pc_next error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.dest_en != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.dest_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src1_en != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src1_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src2_en != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src2_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src1_busy != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src1_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src2_busy != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src2_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src1_latency != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src1_latency error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src2_latency != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src2_latency error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src1_is_pc != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src1_is_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.src2_is_imm != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.src2_is_imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.func3 != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.func3 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.func7_5 != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.func7_5 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.imm != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.pc != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.tag != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.tag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.csr_idx != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.csr_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.rob_idx != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.rob_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.stq_idx != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.stq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.pre_sta_mask != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.pre_sta_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.pre_std_mask != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.pre_std_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.uop_num != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.uop_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.cplt_num != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.cplt_num error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.rob_flag != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.rob_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.page_fault_inst != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.page_fault_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.page_fault_load != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.page_fault_load error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.page_fault_store != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.page_fault_store error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.illegal_inst != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.illegal_inst error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.type != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.type error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.op != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.op error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.rob_commit->commit_entry[i].uop.amoop != 0) {
            std::cout << "out.rob_commit->commit_entry[i].uop.amoop error, not 0!" << std::endl;
            exit(1);
        }
    }
    if(out.rob_bcast->flush != 0) {
        std::cout << "out.rob_bcast->flush error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->mret != 0) {
        std::cout << "out.rob_bcast->mret error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->sret != 0) {
        std::cout << "out.rob_bcast->sret error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->ecall != 0) {
        std::cout << "out.rob_bcast->ecall error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->exception != 0) {
        std::cout << "out.rob_bcast->exception error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->page_fault_inst != 0) {
        std::cout << "out.rob_bcast->page_fault_inst error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->page_fault_load != 0) {
        std::cout << "out.rob_bcast->page_fault_load error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->page_fault_store != 0) {
        std::cout << "out.rob_bcast->page_fault_store error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->illegal_inst != 0) {
        std::cout << "out.rob_bcast->illegal_inst error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->interrupt != 0) {
        std::cout << "out.rob_bcast->interrupt error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->trap_val != 0) {
        std::cout << "out.rob_bcast->trap_val error, not 0!" << std::endl;
        exit(1);
    }
    if(out.rob_bcast->pc != 0) {
        std::cout << "out.rob_bcast->pc error, not 0!" << std::endl;
        exit(1);
    }
    for(int i = 0; i < 4; i++) {
        if(out.entry_addr_bank_idx[i] != 0) {
            std::cout << "out.entry_addr_bank_idx[i] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i = 0; i < 4; i++) {
        if(out.entry_addr_line_idx[i] != 0) {
            std::cout << "out.entry_addr_line_idx[i] error, not 0!" << std::endl;
            exit(1);
        }
    }
    if(out.entry_single_idx_addr_bank_idx != 0) {
        std::cout << "out.entry_single_idx_addr_bank_idx error, not 0!" << std::endl;
        exit(1);
    }
    if(out.entry_single_idx_addr_line_idx != 0) {
        std::cout << "out.entry_single_idx_addr_line_idx error, not 0!" << std::endl;
        exit(1);
    }
    if(enq_ptr_1 != 0) {
        std::cout << "enq_ptr_1 error, not 0!" << std::endl;
        exit(1);
    }
    if(deq_ptr_1 != 0) {
        std::cout << "deq_ptr_1 error, not 0!" << std::endl;
        exit(1);
    }
    if(count_1 != 0) {
        std::cout << "count_1 error, not 0!" << std::endl;
        exit(1);
    }
    if(flag_1 != 0) {
        std::cout << "flag_1 error, not 0!" << std::endl;
        exit(1);
    }
}
void ROB::simulator_to_po(bool* po) {
    bool* cursor = po;
    unpack_bits(cursor, out.rob2dis->ready, 1); //0
    unpack_bits(cursor, out.rob2dis->empty, 1); //1
    unpack_bits(cursor, out.rob2dis->stall, 1); //2
    unpack_bits(cursor, out.rob2dis->enq_idx, 7); //3
    unpack_bits(cursor, out.rob2dis->rob_flag, 1); //10
    unpack_bits(cursor, out.rob2csr->interrupt_resp, 1); //11
    unpack_bits(cursor, out.rob2csr->commit, 1); //12
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].valid, 1); //13
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.instruction, 32); //17
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.dest_areg, 6); //145
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src1_areg, 6); //169
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src2_areg, 6); //193
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.dest_preg, 7); //217
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src1_preg, 7); //245
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src2_preg, 7); //273
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.old_dest_preg, 7); //301
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src1_rdata, 32); //329
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src2_rdata, 32); //457
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.result, 32); //585
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.pred_br_taken, 1); //713
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.alt_pred, 1); //717
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.altpcpn, 8); //721
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.pcpn, 8); //753
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.pred_br_pc, 32); //785
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.mispred, 1); //913
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.br_taken, 1); //917
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.pc_next, 32); //921
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.dest_en, 1); //1049
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src1_en, 1); //1053
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src2_en, 1); //1057
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src1_busy, 1); //1061
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src2_busy, 1); //1065
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src1_latency, 4); //1069
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src2_latency, 4); //1085
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src1_is_pc, 1); //1101
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.src2_is_imm, 1); //1105
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.func3, 3); //1109
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.func7_5, 1); //1121
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.imm, 32); //1125
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.pc, 32); //1253
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.tag, 4); //1381
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.csr_idx, 12); //1397
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.rob_idx, 7); //1445
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.stq_idx, 4); //1473
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.pre_sta_mask, 16); //1489
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.pre_std_mask, 16); //1553
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.uop_num, 2); //1617
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.cplt_num, 2); //1625
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.rob_flag, 1); //1633
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.page_fault_inst, 1); //1637
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.page_fault_load, 1); //1641
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.page_fault_store, 1); //1645
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.illegal_inst, 1); //1649
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.type, 4); //1653
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.op, 4); //1669
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.rob_commit->commit_entry[i].uop.amoop, 4); //1685
    }
    unpack_bits(cursor, out.rob_bcast->flush, 1); //1701
    unpack_bits(cursor, out.rob_bcast->mret, 1); //1702
    unpack_bits(cursor, out.rob_bcast->sret, 1); //1703
    unpack_bits(cursor, out.rob_bcast->ecall, 1); //1704
    unpack_bits(cursor, out.rob_bcast->exception, 1); //1705
    unpack_bits(cursor, out.rob_bcast->page_fault_inst, 1); //1706
    unpack_bits(cursor, out.rob_bcast->page_fault_load, 1); //1707
    unpack_bits(cursor, out.rob_bcast->page_fault_store, 1); //1708
    unpack_bits(cursor, out.rob_bcast->illegal_inst, 1); //1709
    unpack_bits(cursor, out.rob_bcast->interrupt, 1); //1710
    unpack_bits(cursor, out.rob_bcast->trap_val, 32); //1711
    unpack_bits(cursor, out.rob_bcast->pc, 32); //1743
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.entry_addr_bank_idx[i], 2); //1775
    }
    for(int i = 0; i < 4; i++) {
        unpack_bits(cursor, out.entry_addr_line_idx[i], 5); //1783
    }
    unpack_bits(cursor, out.entry_single_idx_addr_bank_idx, 2); //1803
    unpack_bits(cursor, out.entry_single_idx_addr_line_idx, 5); //1805
    unpack_bits(cursor, enq_ptr_1, 5); //1810
    unpack_bits(cursor, deq_ptr_1, 5); //1815
    unpack_bits(cursor, count_1, 5); //1820
    unpack_bits(cursor, flag_1, 1); //1825
}
#endif
