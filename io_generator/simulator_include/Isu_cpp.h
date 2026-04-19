#ifndef ISU_CPP_H
#define ISU_CPP_H
#include "Isu.h"
#include "config.h"
#include <cstdint>
#include <cstring>
#include <vector>

Isu::Isu(SimContext *context) : ctx(context) {
    latency_pipe.resize(18);
    latency_pipe_1.resize(18);
}

void Isu::add_iq(const IssueQueueConfig &cfg) {
  iqs.emplace_back(cfg);
  configs.push_back(cfg);
}

void Isu::apply_wakeup_to_uop(IqStoredUop &uop) const {
  for (int k = 0; k < MAX_WAKEUP_PORTS; k++) {
    if (!out.iss_awake->wake[k].valid) {
      continue;
    }
    uint32_t preg = out.iss_awake->wake[k].preg;
    if (uop.src1_en && uop.src1_preg == preg) {
      uop.src1_busy = false;
    }
    if (uop.src2_en && uop.src2_preg == preg) {
      uop.src2_busy = false;
    }
  }
}

void Isu::init() {

  iqs.clear();
  configs.clear();
  latency_pipe.clear();
  latency_pipe_1.clear();
  for (int i = 0; i < IQ_NUM; i++) {
    committed_indices_buf[i].clear();
    committed_indices_buf[i].reserve(GLOBAL_IQ_CONFIG[i].size);
  }

  // 遍历每一个 IQ 配置
  for (int i = 0; i < IQ_NUM; i++) {
    const auto &iq_cfg = GLOBAL_IQ_CONFIG[i];

    // 1. 设置 IQ 基本参数
    IssueQueueConfig dynamic_cfg;
    dynamic_cfg.id = iq_cfg.id;
    dynamic_cfg.size = iq_cfg.size;
    dynamic_cfg.dispatch_width = iq_cfg.dispatch_width;
    dynamic_cfg.supported_ops =
        iq_cfg.supported_ops; // 这是给 Dispatch 路由用的粗粒度 Mask

    // 2. ✨ 自动认领物理端口 ✨
    // 根据 range (start, num) 去查 GLOBAL_ISSUE_PORT_CONFIG
    for (int p = 0; p < iq_cfg.port_num; p++) {
      // 计算在全剧表中的下标
      int global_idx = iq_cfg.port_start_idx + p;

      // 拿到详细端口信息
      const auto &port_info = GLOBAL_ISSUE_PORT_CONFIG[global_idx];

      // 将这个端口绑定到当前 IQ (发射队列)
      // 注意：PortBinding 结构体里需要 port_idx 和 capability_mask
      // Use GLOBAL_ISSUE_PORT_CONFIG array index as the canonical physical
      // issue port id. This avoids TU-local __COUNTER__ differences in
      // config.h causing Isu/Exu port-id mismatch.
      dynamic_cfg.ports.push_back({
          global_idx,            // Canonical physical port id
          port_info.support_mask // Capability mask
      });
    }

    // 3. 创建 IQ
    add_iq(dynamic_cfg);
  }
}


int Isu::get_latency(UopType uop) {
  if (uop == UOP_MUL)
    return MUL_MAX_LATENCY; // 乘法指令延迟
  if (uop == UOP_DIV)
    return DIV_MAX_LATENCY; // 除法指令延迟
  return 1;                 // 其他指令认为是单周期，走 Fast Wakeup
}

// =================================================================
// 1. comb_ready: 告诉 Dispatch 每个 IQ 有多少空位
// =================================================================
void Isu::comb_ready() {
  for (int i = 0; i < IQ_NUM; i++) {
    // 直接用 i 索引，因为我们保证了 iqs[i].id == i
    out.iss2dis->ready_num[i] = iqs[i].size - iqs[i].count;
  }
}

// =================================================================
// 2. comb_enq: 批量入队
// =================================================================
void Isu::comb_enq() {
  for (int i = 0; i < IQ_NUM; i++) {
    auto &q = iqs[i];
    int max_w = configs[i].dispatch_width; // 获取该 IQ 配置的最大入队宽

    // 遍历该 IQ 的所有输入通道
    for (int w = 0; w < max_w; w++) {
      // 使用新接口结构 req[i][w]
      if (in.dis2iss->req[i][w].valid) {
        IqStoredUop uop = IqStoredUop::from_dis_iss_uop(in.dis2iss->req[i][w].uop);
        // 本拍入队前叠加唤醒总线，避免把“可读源”误标成 busy。
        apply_wakeup_to_uop(uop);

        IqStoredEntry new_entry;
        new_entry.uop = uop;
        new_entry.valid = true;
        // 记录入队时间 (已移除)
        // 调试已移除

        // 入队
        int success = q.enqueue(new_entry);
        Assert(success && "发射队列溢出！Dispatch 逻辑故障！");
      }
    }
  }
}

// =================================================================
// 3. comb_issue: 调度 + 延迟唤醒管理
// =================================================================
void Isu::comb_issue() {

  for (int i = 0; i < ISSUE_WIDTH; i++) {
    out.iss2prf->iss_entry[i].valid = false;
  }

  for (size_t i = 0; i < iqs.size(); i++) {
    IssueQueue &q = iqs[i];

    // 调用新的 schedule
    auto scheduled_pairs = q.schedule();
    auto &committed_indices = committed_indices_buf[i];
    committed_indices.clear();

    for (auto &pair : scheduled_pairs) {
      int entry_idx = pair.first;
      int phys_port = pair.second; // 物理端口号

      // 检查下游反压
      uint64_t req_bit = (1ULL << static_cast<uint32_t>(q.entry[entry_idx].uop.op));
      if (in.exe2iss->ready[phys_port] &&
          (in.exe2iss->fu_ready_mask[phys_port] & req_bit) &&
          !in.rob_bcast->flush && !in.dec_bcast->mispred) {

        // 发射到指定的物理端口
        out.iss2prf->iss_entry[phys_port].valid = true;
        out.iss2prf->iss_entry[phys_port].uop = q.entry[entry_idx].uop.to_iss_prf_uop();

        // 记录成功发射的索引
        committed_indices.push_back(entry_idx);
      }
    }

    // 提交
    q.commit_issue(committed_indices);
  }
}

void Isu::comb_calc_latency_next() {
  // 清空 Next State (重新计算)
  latency_pipe_1.clear();

  // === Part 1: 处理旧指令 (Countdown) ===
  // 逻辑：读取 Current State，倒计时 > 0 的保留并减 1
  for (const auto &entry : latency_pipe) {
    // 如果倒计时为 0，说明本周期已经在 comb_awake 里唤醒了，
    // 下一周期它就消失了，所以这里只处理 > 0 的
    if (entry.valid && entry.countdown > 0) {
      LatencyEntry next_entry = entry;
      next_entry.countdown--;
      latency_pipe_1.push_back(next_entry);
    }
  }

  // === Part 2: 处理新指令 (New Issue) ===
  // 逻辑：直接从 Output Port 读取刚刚发射的指令
  for (int i = 0; i < ISSUE_WIDTH; i++) {
    const auto &inst = out.iss2prf->iss_entry[i];

    if (inst.valid && inst.uop.dest_en) {
      UopType op = decode_uop_type(inst.uop.op);
      int lat = get_latency(op);

      if (lat > 1) {
        LatencyEntry new_entry;
        new_entry.valid = true;
        new_entry.countdown = lat - 1;
        new_entry.dest_preg = inst.uop.dest_preg;
        new_entry.br_mask = inst.uop.br_mask;
        new_entry.rob_idx = inst.uop.rob_idx;
        new_entry.rob_flag = inst.uop.rob_flag;

        latency_pipe_1.push_back(new_entry);
      }
    }
  }
}

// =================================================================
// 4. comb_awake: 统一唤醒逻辑
// =================================================================
void Isu::comb_awake() {
  std::vector<uint32_t> pregs;
  pregs.reserve(MAX_WAKEUP_PORTS); // 预分配避免重复分配

  // 来源 A: 慢速唤醒 (来自写回阶段：Load / 缓存缺失)
  for (int i = 0; i < LSU_LOAD_WB_WIDTH; i++) {
    if (in.prf_awake->wake[i].valid) {
      pregs.push_back(in.prf_awake->wake[i].preg);
    }
  }

  // 来源 B: 延迟唤醒 (乘法/除法完成)
  for (const auto &le : latency_pipe) {
    if (le.valid && le.countdown == 0) {
      pregs.push_back(le.dest_preg);
    }
  }

  // 来源 C: 快速唤醒 (本周期发射的单周期 ALU 指令)
  for (int i = 0; i < ISSUE_WIDTH; i++) {
    const auto &entry = out.iss2prf->iss_entry[i];
    if (entry.valid && entry.uop.dest_en) {
      UopType op = decode_uop_type(entry.uop.op);
      int lat = get_latency(op);
      if (lat <= 1 && op != UOP_LOAD && op != UOP_STA) {
        pregs.push_back(entry.uop.dest_preg);
      }
    }
  }

  Assert(pregs.size() <= MAX_WAKEUP_PORTS);
  
  // === 统一广播 ===
  // 1. 唤醒所有 IQ
  for (auto &q : iqs) {
    q.wakeup(pregs);
  }



  // 3. 输出给外部 (iss_awake) - 用于通知 rename table 等
  for (size_t i = 0; i < MAX_WAKEUP_PORTS; i++) {
    if (i < pregs.size()) {
      out.iss_awake->wake[i].valid = true;
      out.iss_awake->wake[i].preg = pregs[i];
    } else {
      out.iss_awake->wake[i].valid = false;
    }
  }
}

void Isu::comb_flush() {
  if (in.rob_bcast->flush) {
    for (auto &q : iqs)
      q.flush_all();
    latency_pipe.clear();
    latency_pipe_1.clear();
  } else if (in.dec_bcast->mispred) {
    for (auto &q : iqs)
      q.flush_br(in.dec_bcast->br_mask);

    // 清空延迟流水线管道条目
    auto it = latency_pipe_1.begin();
    while (it != latency_pipe_1.end()) {
      bool match_mask = (it->br_mask & in.dec_bcast->br_mask) != 0;
      if (match_mask) {
        it = latency_pipe_1.erase(it);
        continue;
      }
      ++it;
    }
  }

  // 清除已解析分支的 br_mask bit（在 flush 之后，只影响存活条目）
  wire<BR_MASK_WIDTH> clear = in.dec_bcast->clear_mask;
  if (clear) {
    for (auto &q : iqs)
      q.clear_br(clear);
    for (auto &entry : latency_pipe_1) {
      entry.br_mask &= ~clear;
    }
  }
}

// =================================================================
// 5. 时序逻辑
// =================================================================

void Isu::seq() {
  for (auto &q : iqs) {
    q.tick();
  }

  latency_pipe = latency_pipe_1;
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
void Isu::pi_to_simulator(bool* pi) {
    const bool* cursor = pi;
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].valid = pack_bits<bool>(cursor, 1); //0
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //10
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //70
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //130
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //190
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //230
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //260
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_en = pack_bits<bool>(cursor, 1); //270
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_en = pack_bits<bool>(cursor, 1); //280
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_en = pack_bits<bool>(cursor, 1); //290
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //300
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //310
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //320
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //330
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //340
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //370
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //440
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //760
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //800
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //960
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //1080
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //1130
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //1170
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //1180
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //1220
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.op = pack_bits<uint8_t>(cursor, 5); //1230
            cursor += 5;
        }
    }
    in.prf_awake->wake[1].valid = pack_bits<bool>(cursor, 1); //1280
    cursor += 1;
    in.prf_awake->wake[1].preg = pack_bits<uint8_t>(cursor, 6); //1281
    cursor += 6;
    for(int i0 = 0; i0 < 6; i0++) {
        in.exe2iss->ready[i0] = pack_bits<bool>(cursor, 1); //1287
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        in.exe2iss->fu_ready_mask[i0] = pack_bits<uint32_t>(cursor, 17); //1293
        cursor += 17;
    }
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); //1395
    cursor += 1;
    in.rob_bcast->mret = pack_bits<bool>(cursor, 1); //1396
    cursor += 1;
    in.rob_bcast->sret = pack_bits<bool>(cursor, 1); //1397
    cursor += 1;
    in.rob_bcast->ecall = pack_bits<bool>(cursor, 1); //1398
    cursor += 1;
    in.rob_bcast->exception = pack_bits<bool>(cursor, 1); //1399
    cursor += 1;
    in.rob_bcast->fence = pack_bits<bool>(cursor, 1); //1400
    cursor += 1;
    in.rob_bcast->fence_i = pack_bits<bool>(cursor, 1); //1401
    cursor += 1;
    in.rob_bcast->page_fault_inst = pack_bits<bool>(cursor, 1); //1402
    cursor += 1;
    in.rob_bcast->page_fault_load = pack_bits<bool>(cursor, 1); //1403
    cursor += 1;
    in.rob_bcast->page_fault_store = pack_bits<bool>(cursor, 1); //1404
    cursor += 1;
    in.rob_bcast->illegal_inst = pack_bits<bool>(cursor, 1); //1405
    cursor += 1;
    in.rob_bcast->interrupt = pack_bits<bool>(cursor, 1); //1406
    cursor += 1;
    in.rob_bcast->trap_val = pack_bits<uint32_t>(cursor, 32); //1407
    cursor += 32;
    in.rob_bcast->pc = pack_bits<uint32_t>(cursor, 32); //1439
    cursor += 32;
    in.rob_bcast->head_rob_idx = pack_bits<uint8_t>(cursor, 5); //1471
    cursor += 5;
    in.rob_bcast->head_valid = pack_bits<bool>(cursor, 1); //1476
    cursor += 1;
    in.rob_bcast->head_incomplete_rob_idx = pack_bits<uint8_t>(cursor, 5); //1477
    cursor += 5;
    in.rob_bcast->head_incomplete_valid = pack_bits<bool>(cursor, 1); //1482
    cursor += 1;
    in.dec_bcast->mispred = pack_bits<bool>(cursor, 1); //1483
    cursor += 1;
    in.dec_bcast->br_mask = pack_bits<uint16_t>(cursor, 16); //1484
    cursor += 16;
    in.dec_bcast->br_id = pack_bits<uint8_t>(cursor, 4); //1500
    cursor += 4;
    in.dec_bcast->redirect_rob_idx = pack_bits<uint8_t>(cursor, 5); //1504
    cursor += 5;
    in.dec_bcast->clear_mask = pack_bits<uint16_t>(cursor, 16); //1509
    cursor += 16;
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].valid = pack_bits<bool>(cursor, 1); //1525
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //1605
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //2085
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //2565
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //3045
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //3365
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //3605
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.dest_en = pack_bits<bool>(cursor, 1); //3685
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_en = pack_bits<bool>(cursor, 1); //3765
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_en = pack_bits<bool>(cursor, 1); //3845
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //新增！
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //新增！
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //3925
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //4005
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //4085
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //4325
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //4885
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //7445
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //7765
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //9045
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //10005
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //10405
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //10725
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //10805
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //11125
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.op = pack_bits<uint8_t>(cursor, 5); //11205
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        iqs[i0].count = pack_bits<uint32_t>(cursor, 32); //11605
        cursor += 32;
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 64; i1++) {
            iqs[i0].wake_matrix_src1[i1] = pack_bits<uint64_t>(cursor, 64); //11765
            cursor += 64;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 64; i1++) {
            iqs[i0].wake_matrix_src2[i1] = pack_bits<uint64_t>(cursor, 64); //32245
            cursor += 64;
        }
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].valid = pack_bits<bool>(cursor, 1); //52725
        cursor += 1;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].countdown = pack_bits<uint8_t>(cursor, 8); //52743
        cursor += 8;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].dest_preg = pack_bits<uint32_t>(cursor, 32); //52887
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].br_mask = pack_bits<uint16_t>(cursor, 16); //53463
        cursor += 16;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].rob_idx = pack_bits<uint32_t>(cursor, 32); //53751
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].rob_flag = pack_bits<uint32_t>(cursor, 32); //54327
        cursor += 32;
    }
}
void Isu::out_initial_detect() {
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].valid != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.dest_preg != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.src1_preg != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.src1_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.src2_preg != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.src2_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.ftq_idx != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.ftq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.ftq_offset != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.ftq_offset error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.is_atomic != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.is_atomic error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.dest_en != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.dest_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.src1_en != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.src1_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.src2_en != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.src2_en error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.src2_en != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.src1_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.src2_en != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.src2_busy error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.src1_is_pc != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.src1_is_pc error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.src2_is_imm != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.src2_is_imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.func3 != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.func3 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.func7 != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.func7 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.imm != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.imm error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.br_id != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.br_id error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.br_mask != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.br_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.csr_idx != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.csr_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.rob_idx != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.rob_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.stq_idx != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.stq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.stq_flag != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.stq_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.ldq_idx != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.ldq_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.rob_flag != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.rob_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 6; i0++) {
        if(out.iss2prf->iss_entry[i0].uop.op != 0) {
            std::cout << "out.iss2prf->iss_entry[i0].uop.op error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        if(out.iss2dis->ready_num[i0] != 0) {
            std::cout << "out.iss2dis->ready_num[i0] error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        if(out.iss_awake->wake[i0].valid != 0) {
            std::cout << "out.iss_awake->wake[i0].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        if(out.iss_awake->wake[i0].preg != 0) {
            std::cout << "out.iss_awake->wake[i0].preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].valid != 0) {
                std::cout << "iqs[i0].entry_1[i1].valid error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.dest_preg != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.dest_preg error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.src1_preg != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.src1_preg error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.src2_preg != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.src2_preg error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.ftq_idx != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.ftq_idx error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.ftq_offset != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.ftq_offset error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.is_atomic != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.is_atomic error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.dest_en != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.dest_en error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.src1_en != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.src1_en error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.src2_en != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.src2_en error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.src1_is_pc != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.src1_is_pc error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.src2_is_imm != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.src2_is_imm error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.func3 != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.func3 error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.func7 != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.func7 error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.imm != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.imm error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.br_id != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.br_id error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.br_mask != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.br_mask error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.csr_idx != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.csr_idx error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.rob_idx != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.rob_idx error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.stq_idx != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.stq_idx error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.stq_flag != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.stq_flag error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.ldq_idx != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.ldq_idx error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.rob_flag != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.rob_flag error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            if(iqs[i0].entry_1[i1].uop.op != 0) {
                std::cout << "iqs[i0].entry_1[i1].uop.op error, not 0!" << std::endl;
                exit(1);
            }
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        if(iqs[i0].count_1 != 0) {
            std::cout << "iqs[i0].count_1 error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 18; i0++) {
        if(latency_pipe_1[i0].valid != 0) {
            std::cout << "latency_pipe_1[i0].valid error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 18; i0++) {
        if(latency_pipe_1[i0].countdown != 0) {
            std::cout << "latency_pipe_1[i0].countdown error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 18; i0++) {
        if(latency_pipe_1[i0].dest_preg != 0) {
            std::cout << "latency_pipe_1[i0].dest_preg error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 18; i0++) {
        if(latency_pipe_1[i0].br_mask != 0) {
            std::cout << "latency_pipe_1[i0].br_mask error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 18; i0++) {
        if(latency_pipe_1[i0].rob_idx != 0) {
            std::cout << "latency_pipe_1[i0].rob_idx error, not 0!" << std::endl;
            exit(1);
        }
    }
    for(int i0 = 0; i0 < 18; i0++) {
        if(latency_pipe_1[i0].rob_flag != 0) {
            std::cout << "latency_pipe_1[i0].rob_flag error, not 0!" << std::endl;
            exit(1);
        }
    }
}
void Isu::simulator_to_po(bool* po) {
    bool* cursor = po;
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].valid, 1); //0
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.dest_preg, 6); //6
        cursor += 6;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src1_preg, 6); //42
        cursor += 6;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src2_preg, 6); //78
        cursor += 6;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.ftq_idx, 4); //114
        cursor += 4;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.ftq_offset, 3); //138
        cursor += 3;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.is_atomic, 1); //156
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.dest_en, 1); //162
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src1_en, 1); //168
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src2_en, 1); //174
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src1_is_pc, 1); //180
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src2_is_imm, 1); //186
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.func3, 3); //192
        cursor += 3;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.func7, 7); //210
        cursor += 7;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.imm, 32); //252
        cursor += 32;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.br_id, 4); //444
        cursor += 4;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.br_mask, 16); //468
        cursor += 16;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.csr_idx, 12); //564
        cursor += 12;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.rob_idx, 5); //636
        cursor += 5;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.stq_idx, 4); //666
        cursor += 4;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.stq_flag, 1); //690
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.ldq_idx, 4); //696
        cursor += 4;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.rob_flag, 1); //720
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.op, 5); //726
        cursor += 5;
    }
    for(int i0 = 0; i0 < 5; i0++) {
        unpack_bits(cursor, out.iss2dis->ready_num[i0], 5); //756
        cursor += 5;
    }
    for(int i0 = 0; i0 < 5; i0++) {
        unpack_bits(cursor, out.iss_awake->wake[i0].valid, 1); //781
        cursor += 1;
    }
    for(int i0 = 0; i0 < 5; i0++) {
        unpack_bits(cursor, out.iss_awake->wake[i0].preg, 6); //786
        cursor += 6;
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].valid, 1); //816
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.dest_preg, 6); //896
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.src1_preg, 6); //1376
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.src2_preg, 6); //1856
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.ftq_idx, 4); //2336
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.ftq_offset, 3); //2656
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.is_atomic, 1); //2896
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.dest_en, 1); //2976
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.src1_en, 1); //3056
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.src2_en, 1); //3136
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.src1_busy, 1); //3136
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.src2_busy, 1); //3136
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.src1_is_pc, 1); //3216
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.src2_is_imm, 1); //3296
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.func3, 3); //3376
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.func7, 7); //3616
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.imm, 32); //4176
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.br_id, 4); //6736
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.br_mask, 16); //7056
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.csr_idx, 12); //8336
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.rob_idx, 5); //9296
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.stq_idx, 4); //9696
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.stq_flag, 1); //10016
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.ldq_idx, 4); //10096
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.rob_flag, 1); //10416
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            unpack_bits(cursor, iqs[i0].entry_1[i1].uop.op, 5); //10496
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        unpack_bits(cursor, iqs[i0].count_1, 32); //10896
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].valid, 1); //11056
        cursor += 1;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].countdown, 8); //11074
        cursor += 8;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].dest_preg, 32); //11218
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].br_mask, 16); //11794
        cursor += 16;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].rob_idx, 32); //12082
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].rob_flag, 32); //12658
        cursor += 32;
    }
}
// void Isu::simulator_with_bsd() {
//     bool pi [54903];
//     bool po [13234];
//     bool* cursor_pi = pi;
//     bool* cursor_po = po;
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].valid, 1); //0
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.dest_preg, 6); //10
//             cursor_pi += 6;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.src1_preg, 6); //70
//             cursor_pi += 6;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.src2_preg, 6); //130
//             cursor_pi += 6;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.ftq_idx, 4); //190
//             cursor_pi += 4;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.ftq_offset, 3); //230
//             cursor_pi += 3;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.is_atomic, 1); //260
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.dest_en, 1); //270
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.src1_en, 1); //280
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.src2_en, 1); //290
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.src1_busy, 1); //300
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.src2_busy, 1); //310
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.src1_is_pc, 1); //320
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.src2_is_imm, 1); //330
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.func3, 3); //340
//             cursor_pi += 3;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.func7, 7); //370
//             cursor_pi += 7;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.imm, 32); //440
//             cursor_pi += 32;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.br_id, 4); //760
//             cursor_pi += 4;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.br_mask, 16); //800
//             cursor_pi += 16;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.csr_idx, 12); //960
//             cursor_pi += 12;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.rob_idx, 5); //1080
//             cursor_pi += 5;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.stq_idx, 4); //1130
//             cursor_pi += 4;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.stq_flag, 1); //1170
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.ldq_idx, 4); //1180
//             cursor_pi += 4;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.rob_flag, 1); //1220
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 2; i1++) {
//             unpack_bits(cursor_pi, in.dis2iss->req[i0][i1].uop.op, 5); //1230
//             cursor_pi += 5;
//         }
//     }
//     unpack_bits(cursor_pi, in.prf_awake->wake[1].valid, 1); //1280
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.prf_awake->wake[1].preg, 6); //1281
//     cursor_pi += 6;
//     for(int i0 = 0; i0 < 6; i0++) {
//         unpack_bits(cursor_pi, in.exe2iss->ready[i0], 1); //1287
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         unpack_bits(cursor_pi, in.exe2iss->fu_ready_mask[i0], 17); //1293
//         cursor_pi += 17;
//     }
//     unpack_bits(cursor_pi, in.rob_bcast->flush, 1); //1395
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->mret, 1); //1396
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->sret, 1); //1397
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->ecall, 1); //1398
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->exception, 1); //1399
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->fence, 1); //1400
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->fence_i, 1); //1401
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_inst, 1); //1402
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_load, 1); //1403
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->page_fault_store, 1); //1404
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->illegal_inst, 1); //1405
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->interrupt, 1); //1406
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->trap_val, 32); //1407
//     cursor_pi += 32;
//     unpack_bits(cursor_pi, in.rob_bcast->pc, 32); //1439
//     cursor_pi += 32;
//     unpack_bits(cursor_pi, in.rob_bcast->head_rob_idx, 5); //1471
//     cursor_pi += 5;
//     unpack_bits(cursor_pi, in.rob_bcast->head_valid, 1); //1476
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.rob_bcast->head_incomplete_rob_idx, 5); //1477
//     cursor_pi += 5;
//     unpack_bits(cursor_pi, in.rob_bcast->head_incomplete_valid, 1); //1482
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.dec_bcast->mispred, 1); //1483
//     cursor_pi += 1;
//     unpack_bits(cursor_pi, in.dec_bcast->br_mask, 16); //1484
//     cursor_pi += 16;
//     unpack_bits(cursor_pi, in.dec_bcast->br_id, 4); //1500
//     cursor_pi += 4;
//     unpack_bits(cursor_pi, in.dec_bcast->redirect_rob_idx, 5); //1504
//     cursor_pi += 5;
//     unpack_bits(cursor_pi, in.dec_bcast->clear_mask, 16); //1509
//     cursor_pi += 16;
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].valid, 1); //1525
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.dest_preg, 6); //1605
//             cursor_pi += 6;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.src1_preg, 6); //2085
//             cursor_pi += 6;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.src2_preg, 6); //2565
//             cursor_pi += 6;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.ftq_idx, 4); //3045
//             cursor_pi += 4;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.ftq_offset, 3); //3365
//             cursor_pi += 3;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.is_atomic, 1); //3605
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.dest_en, 1); //3685
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.src1_en, 1); //3765
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.src2_en, 1); //3845
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.src1_is_pc, 1); //3925
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.src2_is_imm, 1); //4005
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.func3, 3); //4085
//             cursor_pi += 3;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.func7, 7); //4325
//             cursor_pi += 7;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.imm, 32); //4885
//             cursor_pi += 32;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.br_id, 4); //7445
//             cursor_pi += 4;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.br_mask, 16); //7765
//             cursor_pi += 16;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.csr_idx, 12); //9045
//             cursor_pi += 12;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.rob_idx, 5); //10005
//             cursor_pi += 5;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.stq_idx, 4); //10405
//             cursor_pi += 4;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.stq_flag, 1); //10725
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.ldq_idx, 4); //10805
//             cursor_pi += 4;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.rob_flag, 1); //11125
//             cursor_pi += 1;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].entry[i1].uop.op, 5); //11205
//             cursor_pi += 5;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         unpack_bits(cursor_pi, iqs[i0].count, 32); //11605
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 64; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].wake_matrix_src1[i1], 64); //11765
//             cursor_pi += 64;
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 64; i1++) {
//             unpack_bits(cursor_pi, iqs[i0].wake_matrix_src2[i1], 64); //32245
//             cursor_pi += 64;
//         }
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         unpack_bits(cursor_pi, latency_pipe[i0].valid, 1); //52725
//         cursor_pi += 1;
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         unpack_bits(cursor_pi, latency_pipe[i0].countdown, 8); //52743
//         cursor_pi += 8;
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         unpack_bits(cursor_pi, latency_pipe[i0].dest_preg, 32); //52887
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         unpack_bits(cursor_pi, latency_pipe[i0].br_mask, 16); //53463
//         cursor_pi += 16;
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         unpack_bits(cursor_pi, latency_pipe[i0].rob_idx, 32); //53751
//         cursor_pi += 32;
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         unpack_bits(cursor_pi, latency_pipe[i0].rob_flag, 32); //54327
//         cursor_pi += 32;
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
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].valid, 1, "out.iss2prf->iss_entry[i0].valid", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.dest_preg, 6, "out.iss2prf->iss_entry[i0].uop.dest_preg", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.src1_preg, 6, "out.iss2prf->iss_entry[i0].uop.src1_preg", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.src2_preg, 6, "out.iss2prf->iss_entry[i0].uop.src2_preg", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.ftq_idx, 4, "out.iss2prf->iss_entry[i0].uop.ftq_idx", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.ftq_offset, 3, "out.iss2prf->iss_entry[i0].uop.ftq_offset", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.is_atomic, 1, "out.iss2prf->iss_entry[i0].uop.is_atomic", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.dest_en, 1, "out.iss2prf->iss_entry[i0].uop.dest_en", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.src1_en, 1, "out.iss2prf->iss_entry[i0].uop.src1_en", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.src2_en, 1, "out.iss2prf->iss_entry[i0].uop.src2_en", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.src1_is_pc, 1, "out.iss2prf->iss_entry[i0].uop.src1_is_pc", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.src2_is_imm, 1, "out.iss2prf->iss_entry[i0].uop.src2_is_imm", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.func3, 3, "out.iss2prf->iss_entry[i0].uop.func3", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.func7, 7, "out.iss2prf->iss_entry[i0].uop.func7", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.imm, 32, "out.iss2prf->iss_entry[i0].uop.imm", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.br_id, 4, "out.iss2prf->iss_entry[i0].uop.br_id", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.br_mask, 16, "out.iss2prf->iss_entry[i0].uop.br_mask", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.csr_idx, 12, "out.iss2prf->iss_entry[i0].uop.csr_idx", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.rob_idx, 5, "out.iss2prf->iss_entry[i0].uop.rob_idx", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.stq_idx, 4, "out.iss2prf->iss_entry[i0].uop.stq_idx", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.stq_flag, 1, "out.iss2prf->iss_entry[i0].uop.stq_flag", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.ldq_idx, 4, "out.iss2prf->iss_entry[i0].uop.ldq_idx", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.rob_flag, 1, "out.iss2prf->iss_entry[i0].uop.rob_flag", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 6; i0++) {
//         compare_and_pack(cursor_po, out.iss2prf->iss_entry[i0].uop.op, 5, "out.iss2prf->iss_entry[i0].uop.op", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         compare_and_pack(cursor_po, out.iss2dis->ready_num[i0], 5, "out.iss2dis->ready_num[i0]", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         compare_and_pack(cursor_po, out.iss_awake->wake[i0].valid, 1, "out.iss_awake->wake[i0].valid", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         compare_and_pack(cursor_po, out.iss_awake->wake[i0].preg, 6, "out.iss_awake->wake[i0].preg", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].valid, 1, "iqs[i0].entry_1[i1].valid", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.dest_preg, 6, "iqs[i0].entry_1[i1].uop.dest_preg", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.src1_preg, 6, "iqs[i0].entry_1[i1].uop.src1_preg", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.src2_preg, 6, "iqs[i0].entry_1[i1].uop.src2_preg", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.ftq_idx, 4, "iqs[i0].entry_1[i1].uop.ftq_idx", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.ftq_offset, 3, "iqs[i0].entry_1[i1].uop.ftq_offset", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.is_atomic, 1, "iqs[i0].entry_1[i1].uop.is_atomic", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.dest_en, 1, "iqs[i0].entry_1[i1].uop.dest_en", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.src1_en, 1, "iqs[i0].entry_1[i1].uop.src1_en", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.src2_en, 1, "iqs[i0].entry_1[i1].uop.src2_en", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.src1_is_pc, 1, "iqs[i0].entry_1[i1].uop.src1_is_pc", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.src2_is_imm, 1, "iqs[i0].entry_1[i1].uop.src2_is_imm", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.func3, 3, "iqs[i0].entry_1[i1].uop.func3", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.func7, 7, "iqs[i0].entry_1[i1].uop.func7", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.imm, 32, "iqs[i0].entry_1[i1].uop.imm", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.br_id, 4, "iqs[i0].entry_1[i1].uop.br_id", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.br_mask, 16, "iqs[i0].entry_1[i1].uop.br_mask", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.csr_idx, 12, "iqs[i0].entry_1[i1].uop.csr_idx", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.rob_idx, 5, "iqs[i0].entry_1[i1].uop.rob_idx", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.stq_idx, 4, "iqs[i0].entry_1[i1].uop.stq_idx", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.stq_flag, 1, "iqs[i0].entry_1[i1].uop.stq_flag", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.ldq_idx, 4, "iqs[i0].entry_1[i1].uop.ldq_idx", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.rob_flag, 1, "iqs[i0].entry_1[i1].uop.rob_flag", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         for(int i1 = 0; i1 < 16; i1++) {
//             compare_and_pack(cursor_po, iqs[i0].entry_1[i1].uop.op, 5, "iqs[i0].entry_1[i1].uop.op", pi, 54903);
//         }
//     }
//     for(int i0 = 0; i0 < 5; i0++) {
//         compare_and_pack(cursor_po, iqs[i0].count_1, 32, "iqs[i0].count_1", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         compare_and_pack(cursor_po, latency_pipe_1[i0].valid, 1, "latency_pipe_1[i0].valid", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         compare_and_pack(cursor_po, latency_pipe_1[i0].countdown, 8, "latency_pipe_1[i0].countdown", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         compare_and_pack(cursor_po, latency_pipe_1[i0].dest_preg, 32, "latency_pipe_1[i0].dest_preg", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         compare_and_pack(cursor_po, latency_pipe_1[i0].br_mask, 16, "latency_pipe_1[i0].br_mask", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         compare_and_pack(cursor_po, latency_pipe_1[i0].rob_idx, 32, "latency_pipe_1[i0].rob_idx", pi, 54903);
//     }
//     for(int i0 = 0; i0 < 18; i0++) {
//         compare_and_pack(cursor_po, latency_pipe_1[i0].rob_flag, 32, "latency_pipe_1[i0].rob_flag", pi, 54903);
//     }
// }

// =================================================================
// 模块 1：Ready + Issue (PI_WIDTH = 10478+160)
// 认领：exe2iss(1287), bcast(1395), iqs[].entry(1525), iqs[].count(11605)
// =================================================================
void Isu::pi_to_simulator_mod1(bool* pi) {
    bool* cursor = pi;
    for(int i0=0; i0<6; i0++) { in.exe2iss->ready[i0] = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0=0; i0<6; i0++) { in.exe2iss->fu_ready_mask[i0] = pack_bits<uint32_t>(cursor, 17); cursor += 17; }
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); cursor += 1;
    in.rob_bcast->mret = pack_bits<bool>(cursor, 1); //1396
    cursor += 1;
    in.rob_bcast->sret = pack_bits<bool>(cursor, 1); //1397
    cursor += 1;
    in.rob_bcast->ecall = pack_bits<bool>(cursor, 1); //1398
    cursor += 1;
    in.rob_bcast->exception = pack_bits<bool>(cursor, 1); //1399
    cursor += 1;
    in.rob_bcast->fence = pack_bits<bool>(cursor, 1); //1400
    cursor += 1;
    in.rob_bcast->fence_i = pack_bits<bool>(cursor, 1); //1401
    cursor += 1;
    in.rob_bcast->page_fault_inst = pack_bits<bool>(cursor, 1); //1402
    cursor += 1;
    in.rob_bcast->page_fault_load = pack_bits<bool>(cursor, 1); //1403
    cursor += 1;
    in.rob_bcast->page_fault_store = pack_bits<bool>(cursor, 1); //1404
    cursor += 1;
    in.rob_bcast->illegal_inst = pack_bits<bool>(cursor, 1); //1405
    cursor += 1;
    in.rob_bcast->interrupt = pack_bits<bool>(cursor, 1); //1406
    cursor += 1;
    in.rob_bcast->trap_val = pack_bits<uint32_t>(cursor, 32); //1407
    cursor += 32;
    in.rob_bcast->pc = pack_bits<uint32_t>(cursor, 32); //1439
    cursor += 32;
    in.rob_bcast->head_rob_idx = pack_bits<uint8_t>(cursor, 5); //1471
    cursor += 5;
    in.rob_bcast->head_valid = pack_bits<bool>(cursor, 1); //1476
    cursor += 1;
    in.rob_bcast->head_incomplete_rob_idx = pack_bits<uint8_t>(cursor, 5); //1477
    cursor += 5;
    in.rob_bcast->head_incomplete_valid = pack_bits<bool>(cursor, 1); //1482
    cursor += 1;
    in.dec_bcast->mispred = pack_bits<bool>(cursor, 1); //1483
    cursor += 1;
    in.dec_bcast->br_mask = pack_bits<uint16_t>(cursor, 16); //1484
    cursor += 16;
    in.dec_bcast->br_id = pack_bits<uint8_t>(cursor, 4); //1500
    cursor += 4;
    in.dec_bcast->redirect_rob_idx = pack_bits<uint8_t>(cursor, 5); //1504
    cursor += 5;
    in.dec_bcast->clear_mask = pack_bits<uint16_t>(cursor, 16); //1509
    cursor += 16;
    // ... 此处请拷贝原版 rob_bcast 全部字段 (从 mret 到 head_incomplete_valid) ...
    // ... 此处请拷贝原版 dec_bcast 全部字段 (从 mispred 到 clear_mask) ...
    for(int i0=0; i0<5; i0++) for(int i1=0; i1<16; i1++) { iqs[i0].entry[i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //1605
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //2085
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //2565
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //3045
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //3365
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //3605
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.dest_en = pack_bits<bool>(cursor, 1); //3685
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_en = pack_bits<bool>(cursor, 1); //3765
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_en = pack_bits<bool>(cursor, 1); //3845
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //3925
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //4005
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //4085
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //4325
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //4885
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //7445
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //7765
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //9045
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //10005
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //10405
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //10725
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //10805
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //11125
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.op = pack_bits<uint8_t>(cursor, 5); //11205
            cursor += 5;
        }
    }

    // ... 此处请拷贝原版 iqs[].entry 全部字段 (dest_preg, src1_preg ... 直到 op) ...
    for(int i0=0; i0<5; i0++) { iqs[i0].count = pack_bits<uint32_t>(cursor, 32); cursor += 32; }
}

// =================================================================
// 模块 2：Combined Aux (PI_WIDTH = 12343+160)
// 认领：prf_awake(1280), iqs[].entry(1525), latency_pipe(52725), 注入的 M1 结果
// =================================================================
void Isu::pi_to_simulator_mod2(bool* pi) {
    bool* cursor = pi;
    in.prf_awake->wake[1].valid = pack_bits<bool>(cursor, 1); cursor += 1;
    in.prf_awake->wake[1].preg = pack_bits<uint8_t>(cursor, 6); cursor += 6;
    // iqs[].entry (全量搬运，用于判断唤醒谁)
    for(int i0=0; i0<5; i0++) for(int i1=0; i1<16; i1++) { iqs[i0].entry[i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //1605
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //2085
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //2565
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //3045
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //3365
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //3605
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.dest_en = pack_bits<bool>(cursor, 1); //3685
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_en = pack_bits<bool>(cursor, 1); //3765
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_en = pack_bits<bool>(cursor, 1); //3845
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //3925
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //4005
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //4085
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //4325
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //4885
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //7445
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //7765
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //9045
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //10005
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //10405
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //10725
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //10805
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //11125
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 16; i1++) {
            iqs[i0].entry[i1].uop.op = pack_bits<uint8_t>(cursor, 5); //11205
            cursor += 5;
        }
    }

    // ... (粘贴 entry 全部字段映射) ...
    // latency_pipe (全量搬运)
    for(int i0=0; i0<18; i0++) { latency_pipe[i0].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].countdown = pack_bits<uint8_t>(cursor, 8); //52743
        cursor += 8;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].dest_preg = pack_bits<uint32_t>(cursor, 32); //52887
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].br_mask = pack_bits<uint16_t>(cursor, 16); //53463
        cursor += 16;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].rob_idx = pack_bits<uint32_t>(cursor, 32); //53751
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        latency_pipe[i0].rob_flag = pack_bits<uint32_t>(cursor, 32); //54327
        cursor += 32;
    }

    // ... (粘贴 latency_pipe 全部字段映射) ...
    // 注入 M1 的 iss_entry 结果
    for(int i0=0; i0<6; i0++) { 
        out.iss2prf->iss_entry[i0].valid = pack_bits<bool>(cursor, 1); cursor += 1; 
        out.iss2prf->iss_entry[i0].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6;
        out.iss2prf->iss_entry[i0].uop.dest_en = pack_bits<bool>(cursor, 1); cursor += 1;
        out.iss2prf->iss_entry[i0].uop.op = pack_bits<uint8_t>(cursor, 5); cursor += 5;
    }
}

// =================================================================
// 模块 3_x：IQ x 状态更新 (PI_WIDTH = 11489+32)
// 认领：dis2iss(0), iqs[x].entry(1525+x*...), iqs[x].matrix(11765+x*...), flush
// =================================================================
// 以 IQ0 为例，IQ1-4 逻辑相同，仅数组下标 [0] 变 [1..4]
void Isu::pi_to_simulator_mod3_0(bool* pi) {
    bool* cursor = pi;
    // dis2iss->req (全局 0-1279)
    for(int i0=0; i0<5; i0++) for(int i1=0; i1<2; i1++) { in.dis2iss->req[i0][i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //10
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //70
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //130
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //190
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //230
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //260
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_en = pack_bits<bool>(cursor, 1); //270
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_en = pack_bits<bool>(cursor, 1); //280
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_en = pack_bits<bool>(cursor, 1); //290
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //300
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //310
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //320
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //330
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //340
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //370
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //440
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //760
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //800
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //960
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //1080
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //1130
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //1170
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //1180
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //1220
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.op = pack_bits<uint8_t>(cursor, 5); //1230
            cursor += 5;
        }
    }

    // ... (粘贴 dis2iss 全部字段映射) ...
    // iqs[0].entry 专属映射
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.is_atomic = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.dest_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.src1_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.src2_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.src1_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.src2_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); cursor += 7; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.imm = pack_bits<uint32_t>(cursor, 32); cursor += 32; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); cursor += 16; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); cursor += 12; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.stq_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.rob_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[0].entry[i1].uop.op = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
    
    // ... (粘贴 iqs[0].entry 全部字段映射) ...
    // iqs[0].wake_matrix 专属映射
    for(int i1=0; i1<64; i1++) { iqs[0].wake_matrix_src1[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    for(int i1=0; i1<64; i1++) { iqs[0].wake_matrix_src2[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); cursor += 1;
}

void Isu::pi_to_simulator_mod3_1(bool* pi) {
    bool* cursor = pi;
    // dis2iss->req (全局 0-1279)
    for(int i0=0; i0<5; i0++) for(int i1=0; i1<2; i1++) { in.dis2iss->req[i0][i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //10
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //70
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //130
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //190
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //230
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //260
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_en = pack_bits<bool>(cursor, 1); //270
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_en = pack_bits<bool>(cursor, 1); //280
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_en = pack_bits<bool>(cursor, 1); //290
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //300
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //310
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //320
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //330
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //340
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //370
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //440
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //760
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //800
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //960
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //1080
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //1130
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //1170
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //1180
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //1220
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.op = pack_bits<uint8_t>(cursor, 5); //1230
            cursor += 5;
        }
    }

    // ... (粘贴 dis2iss 全部字段映射) ...
    // iqs[0].entry 专属映射
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.is_atomic = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.dest_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.src1_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.src2_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.src1_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.src2_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); cursor += 7; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.imm = pack_bits<uint32_t>(cursor, 32); cursor += 32; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); cursor += 16; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); cursor += 12; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.stq_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.rob_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[1].entry[i1].uop.op = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
    
    // ... (粘贴 iqs[0].entry 全部字段映射) ...
    // iqs[0].wake_matrix 专属映射
    for(int i1=0; i1<64; i1++) { iqs[1].wake_matrix_src1[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    for(int i1=0; i1<64; i1++) { iqs[1].wake_matrix_src2[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); cursor += 1;
}

void Isu::pi_to_simulator_mod3_2(bool* pi) {
    bool* cursor = pi;
    // dis2iss->req (全局 0-1279)
    for(int i0=0; i0<5; i0++) for(int i1=0; i1<2; i1++) { in.dis2iss->req[i0][i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //10
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //70
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //130
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //190
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //230
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //260
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_en = pack_bits<bool>(cursor, 1); //270
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_en = pack_bits<bool>(cursor, 1); //280
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_en = pack_bits<bool>(cursor, 1); //290
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //300
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //310
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //320
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //330
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //340
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //370
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //440
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //760
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //800
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //960
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //1080
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //1130
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //1170
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //1180
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //1220
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.op = pack_bits<uint8_t>(cursor, 5); //1230
            cursor += 5;
        }
    }

    // ... (粘贴 dis2iss 全部字段映射) ...
    // iqs[0].entry 专属映射
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.is_atomic = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.dest_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.src1_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.src2_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.src1_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.src2_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); cursor += 7; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.imm = pack_bits<uint32_t>(cursor, 32); cursor += 32; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); cursor += 16; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); cursor += 12; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.stq_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.rob_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[2].entry[i1].uop.op = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
        
    // ... (粘贴 iqs[0].entry 全部字段映射) ...
    // iqs[0].wake_matrix 专属映射
    for(int i1=0; i1<64; i1++) { iqs[2].wake_matrix_src1[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    for(int i1=0; i1<64; i1++) { iqs[2].wake_matrix_src2[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); cursor += 1;
}

void Isu::pi_to_simulator_mod3_3(bool* pi) {
    bool* cursor = pi;
    // dis2iss->req (全局 0-1279)
    for(int i0=0; i0<5; i0++) for(int i1=0; i1<2; i1++) { in.dis2iss->req[i0][i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //10
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //70
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //130
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //190
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //230
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //260
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_en = pack_bits<bool>(cursor, 1); //270
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_en = pack_bits<bool>(cursor, 1); //280
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_en = pack_bits<bool>(cursor, 1); //290
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //300
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //310
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //320
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //330
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //340
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //370
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //440
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //760
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //800
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //960
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //1080
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //1130
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //1170
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //1180
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //1220
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.op = pack_bits<uint8_t>(cursor, 5); //1230
            cursor += 5;
        }
    }

    // ... (粘贴 dis2iss 全部字段映射) ...
    // iqs[0].entry 专属映射
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.is_atomic = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.dest_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.src1_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.src2_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.src1_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.src2_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); cursor += 7; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.imm = pack_bits<uint32_t>(cursor, 32); cursor += 32; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); cursor += 16; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); cursor += 12; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.stq_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.rob_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[3].entry[i1].uop.op = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
        
    // ... (粘贴 iqs[0].entry 全部字段映射) ...
    // iqs[0].wake_matrix 专属映射
    for(int i1=0; i1<64; i1++) { iqs[3].wake_matrix_src1[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    for(int i1=0; i1<64; i1++) { iqs[3].wake_matrix_src2[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); cursor += 1;
}

void Isu::pi_to_simulator_mod3_4(bool* pi) {
    bool* cursor = pi;
    // dis2iss->req (全局 0-1279)
    for(int i0=0; i0<5; i0++) for(int i1=0; i1<2; i1++) { in.dis2iss->req[i0][i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); //10
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); //70
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); //130
            cursor += 6;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); //190
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); //230
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.is_atomic = pack_bits<bool>(cursor, 1); //260
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.dest_en = pack_bits<bool>(cursor, 1); //270
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_en = pack_bits<bool>(cursor, 1); //280
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_en = pack_bits<bool>(cursor, 1); //290
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_busy = pack_bits<bool>(cursor, 1); //300
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_busy = pack_bits<bool>(cursor, 1); //310
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); //320
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); //330
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); //340
            cursor += 3;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); //370
            cursor += 7;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.imm = pack_bits<uint32_t>(cursor, 32); //440
            cursor += 32;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); //760
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); //800
            cursor += 16;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); //960
            cursor += 12;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); //1080
            cursor += 5;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); //1130
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.stq_flag = pack_bits<bool>(cursor, 1); //1170
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); //1180
            cursor += 4;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.rob_flag = pack_bits<bool>(cursor, 1); //1220
            cursor += 1;
        }
    }
    for(int i0 = 0; i0 < 5; i0++) {
        for(int i1 = 0; i1 < 2; i1++) {
            in.dis2iss->req[i0][i1].uop.op = pack_bits<uint8_t>(cursor, 5); //1230
            cursor += 5;
        }
    }

    // ... (粘贴 dis2iss 全部字段映射) ...
    // iqs[0].entry 专属映射
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].valid = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.dest_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.src1_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.src2_preg = pack_bits<uint8_t>(cursor, 6); cursor += 6; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.ftq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.ftq_offset = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.is_atomic = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.dest_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.src1_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.src2_en = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.src1_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.src2_busy = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.src1_is_pc = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.src2_is_imm = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.func3 = pack_bits<uint8_t>(cursor, 3); cursor += 3; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.func7 = pack_bits<uint8_t>(cursor, 7); cursor += 7; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.imm = pack_bits<uint32_t>(cursor, 32); cursor += 32; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.br_id = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.br_mask = pack_bits<uint16_t>(cursor, 16); cursor += 16; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.csr_idx = pack_bits<uint16_t>(cursor, 12); cursor += 12; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.rob_idx = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.stq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.stq_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.ldq_idx = pack_bits<uint8_t>(cursor, 4); cursor += 4; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.rob_flag = pack_bits<bool>(cursor, 1); cursor += 1; }
    for(int i1 = 0; i1 < 16; i1++) { iqs[4].entry[i1].uop.op = pack_bits<uint8_t>(cursor, 5); cursor += 5; }
        
    // ... (粘贴 iqs[0].entry 全部字段映射) ...
    // iqs[0].wake_matrix 专属映射
    for(int i1=0; i1<64; i1++) { iqs[4].wake_matrix_src1[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    for(int i1=0; i1<64; i1++) { iqs[4].wake_matrix_src2[i1] = pack_bits<uint64_t>(cursor, 64); cursor += 64; }
    in.rob_bcast->flush = pack_bits<bool>(cursor, 1); cursor += 1;
}

// 模块 1 输出：Iss_entry + Ready_num (PO_WIDTH = 781)
void Isu::simulator_to_po_mod1(bool* po) {
    bool* cursor = po;
    for(int i0=0; i0<6; i0++) { unpack_bits(cursor, out.iss2prf->iss_entry[i0].valid, 1); cursor += 1; }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.dest_preg, 6); //6
        cursor += 6;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src1_preg, 6); //42
        cursor += 6;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src2_preg, 6); //78
        cursor += 6;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.ftq_idx, 4); //114
        cursor += 4;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.ftq_offset, 3); //138
        cursor += 3;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.is_atomic, 1); //156
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.dest_en, 1); //162
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src1_en, 1); //168
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src2_en, 1); //174
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src1_is_pc, 1); //180
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.src2_is_imm, 1); //186
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.func3, 3); //192
        cursor += 3;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.func7, 7); //210
        cursor += 7;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.imm, 32); //252
        cursor += 32;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.br_id, 4); //444
        cursor += 4;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.br_mask, 16); //468
        cursor += 16;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.csr_idx, 12); //564
        cursor += 12;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.rob_idx, 5); //636
        cursor += 5;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.stq_idx, 4); //666
        cursor += 4;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.stq_flag, 1); //690
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.ldq_idx, 4); //696
        cursor += 4;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.rob_flag, 1); //720
        cursor += 1;
    }
    for(int i0 = 0; i0 < 6; i0++) {
        unpack_bits(cursor, out.iss2prf->iss_entry[i0].uop.op, 5); //726
        cursor += 5;
    }

    // ... (粘贴原 simulator_to_po 中 iss_entry 全部 756 位导出代码) ...
    for(int i=0; i<5; i++) { unpack_bits(cursor, out.iss2dis->ready_num[i], 5); cursor += 5; }
}

// 模块 2 输出：Wake_bcast + Latency_pipe_1 (PO_WIDTH = 2213)
void Isu::simulator_to_po_mod2(bool* po) {
    bool* cursor = po;
    for(int i0=0; i0<5; i0++) { unpack_bits(cursor, out.iss_awake->wake[i0].valid, 1); cursor += 1; }//781
    for(int i0=0; i0<5; i0++) { unpack_bits(cursor, out.iss_awake->wake[i0].preg, 6); cursor += 6; }//786
    for(int i0=0; i0<18; i0++) { unpack_bits(cursor, latency_pipe_1[i0].valid, 1); cursor += 1; }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].countdown, 8); //11074
        cursor += 8;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].dest_preg, 32); //11218
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].br_mask, 16); //11794
        cursor += 16;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].rob_idx, 32); //12082
        cursor += 32;
    }
    for(int i0 = 0; i0 < 18; i0++) {
        unpack_bits(cursor, latency_pipe_1[i0].rob_flag, 32); //12658
        cursor += 32;
    }

    // ... (粘贴原 simulator_to_po 中 latency_pipe_1 全部字段导出代码) ...
}

// 模块 3_0 输出：IQ0 的下一拍状态 (PO_WIDTH = 2048+32)
void Isu::simulator_to_po_mod3_0(bool* po) {
    bool* cursor = po;
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].valid, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.dest_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.src1_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.src2_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.ftq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.ftq_offset, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.is_atomic, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.dest_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.src1_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.src2_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.src1_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.src2_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.src1_is_pc, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.src2_is_imm, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.func3, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.func7, 7); cursor += 7; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.imm, 32); cursor += 32; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.br_id, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.br_mask, 16); cursor += 16; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.csr_idx, 12); cursor += 12; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.rob_idx, 5); cursor += 5; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.stq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.stq_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.ldq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.rob_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[0].entry_1[i1].uop.op, 5); cursor += 5; }

    // ... (粘贴原 simulator_to_po 中仅针对 iqs[0].entry_1 的全部字段导出代码) ...
    unpack_bits(cursor, iqs[0].count_1, 32); cursor += 32;
}

void Isu::simulator_to_po_mod3_1(bool* po) {
    bool* cursor = po;
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].valid, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.dest_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.src1_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.src2_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.ftq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.ftq_offset, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.is_atomic, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.dest_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.src1_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.src2_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.src1_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.src2_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.src1_is_pc, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.src2_is_imm, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.func3, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.func7, 7); cursor += 7; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.imm, 32); cursor += 32; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.br_id, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.br_mask, 16); cursor += 16; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.csr_idx, 12); cursor += 12; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.rob_idx, 5); cursor += 5; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.stq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.stq_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.ldq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.rob_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[1].entry_1[i1].uop.op, 5); cursor += 5; }

    // ... (粘贴原 simulator_to_po 中仅针对 iqs[0].entry_1 的全部字段导出代码) ...
    unpack_bits(cursor, iqs[1].count_1, 32); cursor += 32;
}

void Isu::simulator_to_po_mod3_2(bool* po) {
    bool* cursor = po;
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].valid, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.dest_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.src1_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.src2_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.ftq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.ftq_offset, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.is_atomic, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.dest_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.src1_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.src2_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.src1_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.src2_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.src1_is_pc, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.src2_is_imm, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.func3, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.func7, 7); cursor += 7; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.imm, 32); cursor += 32; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.br_id, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.br_mask, 16); cursor += 16; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.csr_idx, 12); cursor += 12; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.rob_idx, 5); cursor += 5; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.stq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.stq_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.ldq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.rob_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[2].entry_1[i1].uop.op, 5); cursor += 5; }

    // ... (粘贴原 simulator_to_po 中仅针对 iqs[0].entry_1 的全部字段导出代码) ...
    unpack_bits(cursor, iqs[2].count_1, 32); cursor += 32;
}

void Isu::simulator_to_po_mod3_3(bool* po) {
    bool* cursor = po;
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].valid, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.dest_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.src1_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.src2_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.ftq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.ftq_offset, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.is_atomic, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.dest_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.src1_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.src2_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.src1_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.src2_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.src1_is_pc, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.src2_is_imm, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.func3, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.func7, 7); cursor += 7; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.imm, 32); cursor += 32; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.br_id, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.br_mask, 16); cursor += 16; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.csr_idx, 12); cursor += 12; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.rob_idx, 5); cursor += 5; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.stq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.stq_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.ldq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.rob_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[3].entry_1[i1].uop.op, 5); cursor += 5; }

    // ... (粘贴原 simulator_to_po 中仅针对 iqs[0].entry_1 的全部字段导出代码) ...
    unpack_bits(cursor, iqs[3].count_1, 32); cursor += 32;
}

void Isu::simulator_to_po_mod3_4(bool* po) {
    bool* cursor = po;
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].valid, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.dest_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.src1_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.src2_preg, 6); cursor += 6; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.ftq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.ftq_offset, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.is_atomic, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.dest_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.src1_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.src2_en, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.src1_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.src2_busy, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.src1_is_pc, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.src2_is_imm, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.func3, 3); cursor += 3; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.func7, 7); cursor += 7; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.imm, 32); cursor += 32; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.br_id, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.br_mask, 16); cursor += 16; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.csr_idx, 12); cursor += 12; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.rob_idx, 5); cursor += 5; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.stq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.stq_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.ldq_idx, 4); cursor += 4; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.rob_flag, 1); cursor += 1; }
    for(int i1=0; i1<16; i1++) { unpack_bits(cursor, iqs[4].entry_1[i1].uop.op, 5); cursor += 5; }

    // ... (粘贴原 simulator_to_po 中仅针对 iqs[0].entry_1 的全部字段导出代码) ...
    unpack_bits(cursor, iqs[4].count_1, 32); cursor += 32;
}

// (注：simulator_to_po_mod3_1 到 3_4 同理，仅改 iqs[] 下标)

#endif
