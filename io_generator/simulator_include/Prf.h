#pragma once
#include "config.h"
// #include "Exu.h"
#include "IO.h"

class PrfOut {
public:
  PrfExeIO *prf2exe;
  PrfAwakeIO *prf_awake;
  wire<7>  reg_file_addr_0[ISSUE_WIDTH];
  wire<7>  reg_file_addr_1[ISSUE_WIDTH];
  wire<1>  reg_file_2_en  [ISSUE_WIDTH];
  wire<7>  reg_file_2_addr[ISSUE_WIDTH];
  wire<32> reg_file_2_data[ISSUE_WIDTH];
  wire<1>  writeb_equal_logic_out_1[ISSUE_WIDTH][ISSUE_WIDTH];
  wire<1>  bypass_equal_logic_out_1[ISSUE_WIDTH][TOTAL_FU_COUNT];
  wire<1>  writeb_equal_logic_out_2[ISSUE_WIDTH][ISSUE_WIDTH];
  wire<1>  bypass_equal_logic_out_2[ISSUE_WIDTH][TOTAL_FU_COUNT];
};

class PrfIn {
public:
  IssPrfIO *iss2prf;
  ExePrfIO *exe2prf;
  DecBroadcastIO *dec_bcast;
  RobBroadcastIO *rob_bcast;
  wire<32> reg_file_data_0[ISSUE_WIDTH];
  wire<32> reg_file_data_1[ISSUE_WIDTH];
  wire<1>  writeb_equal_give_in_1[ISSUE_WIDTH][ISSUE_WIDTH];
  wire<1>  bypass_equal_give_in_1[ISSUE_WIDTH][TOTAL_FU_COUNT];
  wire<1>  writeb_equal_give_in_2[ISSUE_WIDTH][ISSUE_WIDTH];
  wire<1>  bypass_equal_give_in_2[ISSUE_WIDTH][TOTAL_FU_COUNT];
  wire<1> is_killed_0[ISSUE_WIDTH];
  wire<1> is_killed_1[ISSUE_WIDTH];
};

class Prf {
public:
  // Prf(SimContext *ctx) { this->ctx = ctx; }
  // SimContext *ctx;
  PrfIn in;
  PrfOut out;

  void comb_complete();
  void comb_awake();
  void comb_read();
  void comb_write();
  void comb_pipeline();
  void init();
  void seq();
  void pi_to_simulator(bool* pi);
  void simulator_to_po(bool* po);
  void out_initial_detect();

  reg<32> reg_file[PRF_NUM];
  ExePrfIO::ExePrfEntry inst_r[ISSUE_WIDTH];

  wire<32> reg_file_1[PRF_NUM];
  ExePrfIO::ExePrfEntry inst_r_1[ISSUE_WIDTH];
};

#include <Prf_cpp.h>
