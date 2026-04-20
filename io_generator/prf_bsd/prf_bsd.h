#include <stdio.h>
#include <cstdint>
#include <iostream>
#include <PRF.h>
#include <IO.h>
#include <config.h>
// #define OUT_INITIAL_DETECT
#ifndef IO_GENERATOR_OUTER_H
#define IO_GENERATOR_OUTER_H
extern const int PI_WIDTH = 9412;
extern const int PO_WIDTH = 9141;
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

    PRF prf = {};
    prf.in.iss2prf = &iss2prf;
    prf.in.exe2prf = &exe2prf;
    prf.in.dec_bcast = &dec_bcast;
    prf.in.rob_bcast = &rob_bcast;


    prf.out.prf2exe = &prf2exe;
    prf.out.prf2rob = &prf2rob; 
    prf.out.prf2dec = &prf2dec;
    prf.out.prf_awake = &prf_awake;

    // end of initial interface and class and struct
    prf.pi_to_simulator(pi);
    #ifdef OUT_INITIAL_DETECT
    prf.out_initial_detect();
    #endif
    //please add code below
    prf.init();
    prf.comb_br_check();
    prf.comb_complete();
    prf.comb_awake();
    prf.comb_write();
    prf.comb_read();
    prf.comb_flush();
    prf.comb_branch();
    prf.comb_pipeline();
    //end of code add
    prf.simulator_to_po(po);
}
#endif

