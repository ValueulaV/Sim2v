#pragma once
#include <IO.h>
#include <config.h>

class ROB_OUT {
public:
  Rob_Dis *rob2dis;
  Rob_Csr *rob2csr;
  Rob_Commit *rob_commit;
  Rob_Broadcast *rob_bcast;
  wire2_t entry_addr_bank_idx[4];
  wire5_t entry_addr_line_idx[4];

  wire2_t entry_single_idx_addr_bank_idx;
  wire5_t entry_single_idx_addr_line_idx;
};

class ROB_IN {
public:
  Dis_Rob *dis2rob;
  Prf_Rob *prf2rob;
  Csr_Rob *csr2rob;
  Dec_Broadcast *dec_bcast;
  Inst_entry entry_data[ROB_BANK_NUM];
  Inst_entry entry_single_idx_data;
};

class ROB {
public:

  void init();
  void seq();
  void comb_ready();
  void comb_commit();
  void comb_complete();
  void comb_fire();
  void comb_branch();
  void comb_flush();
  void pi_to_simulator(bool* pi);
  void simulator_to_po(bool* po);
  void out_initial_detect();
  ROB_IN in;
  ROB_OUT out;

  // 状态
  Inst_entry entry[ROB_BANK_NUM][ROB_NUM / 4];
  reg5_t enq_ptr;
  reg5_t deq_ptr;
  reg5_t count;
  reg1_t flag;

  Inst_entry entry_1[ROB_BANK_NUM][ROB_NUM / 4];
  wire5_t enq_ptr_1;
  wire5_t deq_ptr_1;
  wire5_t count_1;
  wire1_t flag_1;
};
#include <ROB_cpp.h>