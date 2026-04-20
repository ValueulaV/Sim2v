#include <stdio.h>
#include <cstdint>
#include <iostream>
#include <ROB.h>
#include <IO.h>
#include <config.h>
// #define OUT_INITIAL_DETECT
#ifndef IO_GENERATOR_OUTER_H
#define IO_GENERATOR_OUTER_H
extern const int PI_WIDTH = 6801;
extern const int PO_WIDTH = 1826;
void io_generator_outer(bool* pi, bool* po) {
    // initial interface and class and struct
    Front_Dec     front2dec  = {};
    Dec_Front     dec2front  = {};
    Dec_Ren       dec2ren    = {};
    Dec_Broadcast dec_bcast  = {};
    Ren_Dec       ren2dec    = {};
    Ren_Dis       ren2dis    = {};
    Dis_Ren       dis2ren    = {};
    Dis_Iss       dis2iss    = {};
    Dis_Rob       dis2rob    = {};
    Dis_Stq       dis2stq    = {};
    Iss_Awake     iss_awake  = {};
    Iss_Prf       iss2prf    = {};
    Iss_Dis       iss2dis    = {};
    Prf_Exe       prf2exe    = {};
    Prf_Rob       prf2rob    = {};
    Prf_Awake     prf_awake  = {};
    Prf_Dec       prf2dec    = {};
    Exe_Prf       exe2prf    = {};
    Exe_Stq       exe2stq    = {};
    Exe_Iss       exe2iss    = {};
    Rob_Dis       rob2dis    = {};
    Rob_Csr       rob2csr    = {};
    Rob_Broadcast rob_bcast  = {};
    Rob_Commit    rob_commit = {};
    Stq_Dis       stq2dis    = {};     
    Csr_Exe       csr2exe    = {};
    Csr_Rob       csr2rob    = {};
    Csr_Front     csr2front  = {};
    Csr_Status    csr_status = {};
    Exe_Csr       exe2csr    = {};

    ROB rob = {};

    rob.in.dis2rob = &dis2rob;
    rob.in.dec_bcast = &dec_bcast;
    rob.in.prf2rob = &prf2rob;
    rob.in.dec_bcast = &dec_bcast;
    rob.in.csr2rob = &csr2rob;
  
    rob.out.rob_bcast = &rob_bcast;
    rob.out.rob_commit = &rob_commit;
    rob.out.rob2dis = &rob2dis;
    rob.out.rob2csr = &rob2csr;
    // end of initial interface and class and struct
    rob.pi_to_simulator(pi);
    #ifdef OUT_INITIAL_DETECT
    rob.out_initial_detect();
    #endif
    //please add code below
    rob.comb_complete();
    rob.comb_ready();
    rob.comb_commit();
    rob.comb_fire();
    rob.comb_flush();
    rob.comb_branch();
    //end of code add
    rob.simulator_to_po(po);
}
#endif

