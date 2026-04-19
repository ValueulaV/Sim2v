#include <stdio.h>
#include <cstdint>
#include <iostream>
#include <Isu.h>
#include <IO.h>
#include <config.h>

// #define OUT_INITIAL_DETECT
#ifndef IO_GENERATOR_OUTER_H
#define IO_GENERATOR_OUTER_H
extern const int PI_WIDTH = 54903;
extern const int PO_WIDTH = 13234;
void io_generator_outer(bool* pi, bool* po) {
    // initial interface and class and struct
    FrontDecIO     front2dec  = {};
    DecFrontIO     dec2front  = {};
    DecRenIO       dec2ren    = {};
    DecBroadcastIO dec_bcast  = {};
    RenDecIO       ren2dec    = {};
    RenDisIO       ren2dis    = {};
    DisRenIO       dis2ren    = {};
    DisIssIO       dis2iss    = {};   
    DisRobIO       dis2rob    = {};
    IssAwakeIO     iss_awake  = {};
    IssPrfIO       iss2prf    = {};
    IssDisIO       iss2dis    = {};
    PrfExeIO       prf2exe    = {};
    PrfAwakeIO     prf_awake  = {};   
    ExePrfIO       exe2prf    = {};
    ExeIssIO       exe2iss    = {};
    RobDisIO       rob2dis    = {};
    RobCsrIO       rob2csr    = {};
    RobBroadcastIO rob_bcast  = {};
    RobCommitIO    rob_commit = {};
    CsrExeIO       csr2exe    = {};
    CsrRobIO       csr2rob    = {};
    CsrFrontIO     csr2front  = {};
    CsrStatusIO    csr_status = {};
    ExeCsrIO       exe2csr    = {};   

    SimContext ctx;      // 先声明一个上下文对象
    Isu isu(&ctx);       // 把这个对象的地址传给 Isu

    // Isu isu = {};

    isu.in.dis2iss = &dis2iss;
    isu.in.prf_awake = &prf_awake;
    isu.in.exe2iss = &exe2iss;
    isu.in.rob_bcast = &rob_bcast;
    isu.in.dec_bcast = &dec_bcast;

    isu.out.iss2prf = &iss2prf;
    isu.out.iss2dis = &iss2dis;
    isu.out.iss_awake = &iss_awake;

    isu.init();
    // end of initial interface and class and struct
    isu.pi_to_simulator(pi);
    #ifdef OUT_INITIAL_DETECT
    isu.out_initial_detect();
    #endif
    //please add code below
    // isu.init();
    isu.comb_ready();
    isu.comb_issue();
    isu.comb_awake();
    isu.comb_calc_latency_next();
    isu.comb_enq();
    isu.comb_flush();
    //end of code add
    isu.simulator_to_po(po);
}
#endif
