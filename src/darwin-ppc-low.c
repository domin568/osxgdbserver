/* Darwin/PowerPC specific low level interface, for the remote server for GDB.
   Copyright 2005 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include "darwin-low.h"
#include "server.h"

#include <mach/mach.h>
#include <mach/ppc/thread_status.h>
#include <mach/thread_act.h>
#include <string.h>

/*
  Standard GDB register layout for 32-bit PowerPC (71 registers, 412 bytes):

    Regs  0-31:  r0-r31    (32-bit each)
    Regs 32-63:  f0-f31    (64-bit each)
    Reg  64:     pc/srr0   (32-bit)
    Reg  65:     ps/srr1   (32-bit)
    Reg  66:     cr        (32-bit)
    Reg  67:     lr        (32-bit)
    Reg  68:     ctr       (32-bit)
    Reg  69:     xer       (32-bit)
    Reg  70:     fpscr     (32-bit)
*/

#define PPC_NUM_REGS 71

/* Fetch all registers from the Mach thread into the regcache.  */
static void darwin_ppc_fetch_registers(int thread_port)
{
    thread_act_t thread = (thread_act_t)thread_port;
    kern_return_t kr;
    int i;

    /* --- GPRs and SPRs via PPC_THREAD_STATE --- */
    {
        ppc_thread_state_t state;
        mach_msg_type_number_t count = PPC_THREAD_STATE_COUNT;
        unsigned int *gprs;

        kr = thread_get_state(thread, PPC_THREAD_STATE, (thread_state_t)&state, &count);
        if (kr != KERN_SUCCESS)
        {
            warning("thread_get_state(PPC_THREAD_STATE) failed: %s", mach_error_string(kr));
            return;
        }

        /* r0-r31 are contiguous unsigned ints in the struct. */
        gprs = &state.r0;
        for (i = 0; i < 32; i++)
        {
            char name[8];
            sprintf(name, "r%d", i);
            supply_register_by_name(name, &gprs[i]);
        }

        supply_register_by_name("pc", &state.srr0);
        supply_register_by_name("ps", &state.srr1);
        supply_register_by_name("cr", &state.cr);
        supply_register_by_name("lr", &state.lr);
        supply_register_by_name("ctr", &state.ctr);
        supply_register_by_name("xer", &state.xer);
    }

    /* --- FPRs via PPC_FLOAT_STATE --- */
    {
        ppc_float_state_t fstate;
        mach_msg_type_number_t count = PPC_FLOAT_STATE_COUNT;

        kr = thread_get_state(thread, PPC_FLOAT_STATE, (thread_state_t)&fstate, &count);
        if (kr != KERN_SUCCESS)
        {
            warning("thread_get_state(PPC_FLOAT_STATE) failed: %s", mach_error_string(kr));
            return;
        }

        for (i = 0; i < 32; i++)
        {
            char name[8];
            sprintf(name, "f%d", i);
            supply_register_by_name(name, &fstate.fpregs[i]);
        }

        supply_register_by_name("fpscr", &fstate.fpscr);
    }
}

/* Store all registers from the regcache into the Mach thread.  */
static void darwin_ppc_store_registers(int thread_port)
{
    thread_act_t thread = (thread_act_t)thread_port;
    kern_return_t kr;
    int i;

    /* --- GPRs and SPRs --- */
    {
        ppc_thread_state_t state;
        mach_msg_type_number_t count = PPC_THREAD_STATE_COUNT;
        unsigned int *gprs;

        kr = thread_get_state(thread, PPC_THREAD_STATE, (thread_state_t)&state, &count);
        if (kr != KERN_SUCCESS)
        {
            warning("thread_get_state failed in store: %s", mach_error_string(kr));
            return;
        }

        gprs = &state.r0;
        for (i = 0; i < 32; i++)
        {
            char name[8];
            sprintf(name, "r%d", i);
            collect_register_by_name(name, &gprs[i]);
        }

        collect_register_by_name("pc", &state.srr0);
        collect_register_by_name("ps", &state.srr1);
        collect_register_by_name("cr", &state.cr);
        collect_register_by_name("lr", &state.lr);
        collect_register_by_name("ctr", &state.ctr);
        collect_register_by_name("xer", &state.xer);

        kr = thread_set_state(thread, PPC_THREAD_STATE, (thread_state_t)&state,
                              PPC_THREAD_STATE_COUNT);
        if (kr != KERN_SUCCESS)
        {
            warning("thread_set_state(PPC_THREAD_STATE) failed: %s", mach_error_string(kr));
        }
    }

    /* --- FPRs --- */
    {
        ppc_float_state_t fstate;
        mach_msg_type_number_t count = PPC_FLOAT_STATE_COUNT;

        kr = thread_get_state(thread, PPC_FLOAT_STATE, (thread_state_t)&fstate, &count);
        if (kr != KERN_SUCCESS)
        {
            warning("thread_get_state(PPC_FLOAT_STATE) failed in store: %s", mach_error_string(kr));
            return;
        }

        for (i = 0; i < 32; i++)
        {
            char name[8];
            sprintf(name, "f%d", i);
            collect_register_by_name(name, &fstate.fpregs[i]);
        }

        collect_register_by_name("fpscr", &fstate.fpscr);

        kr = thread_set_state(thread, PPC_FLOAT_STATE, (thread_state_t)&fstate,
                              PPC_FLOAT_STATE_COUNT);
        if (kr != KERN_SUCCESS)
        {
            warning("thread_set_state(PPC_FLOAT_STATE) failed: %s", mach_error_string(kr));
        }
    }
}

static CORE_ADDR darwin_ppc_get_pc(void)
{
    unsigned int pc;
    collect_register_by_name("pc", &pc);
    return (CORE_ADDR)pc;
}

static void darwin_ppc_set_pc(CORE_ADDR pc)
{
    unsigned int pc32 = (unsigned int)pc;
    supply_register_by_name("pc", &pc32);
}

/* PPC trap instruction for software breakpoints. */
static const unsigned int ppc_breakpoint = 0x7FE00008;
#define PPC_BREAKPOINT_LEN 4

static int darwin_ppc_breakpoint_at(CORE_ADDR where)
{
    unsigned int insn;
    (*the_target->read_memory)(where, (char *)&insn, 4);
    if (insn == ppc_breakpoint)
    {
        return 1;
    }
    return 0;
}

struct darwin_target_ops the_low_target = {
    PPC_NUM_REGS,
    darwin_ppc_fetch_registers,
    darwin_ppc_store_registers,
    darwin_ppc_get_pc,
    darwin_ppc_set_pc,
    (const char *)&ppc_breakpoint,
    PPC_BREAKPOINT_LEN,
    0, /* decr_pc_after_break */
    darwin_ppc_breakpoint_at,
};
