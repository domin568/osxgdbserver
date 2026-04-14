/* Conditional breakpoint support for Darwin gdbserver.
   Copyright 2005 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.  */

#ifndef COND_BP_H
#define COND_BP_H

#include "server.h"

#define MAX_COND_BPS 32
#define MAX_COND_STR 256

/* Condition types. */
#define COND_TYPE_STR 0
#define COND_TYPE_REG 1
#define COND_TYPE_MEM 2

/* Comparison operators. */
#define COND_OP_EQ 0
#define COND_OP_NE 1
#define COND_OP_GT 2
#define COND_OP_LT 3
#define COND_OP_GE 4
#define COND_OP_LE 5

struct cond_bp
{
  int active;
  CORE_ADDR addr;
  int cond_type;            /* COND_TYPE_STR, COND_TYPE_REG, COND_TYPE_MEM */
  int op;                   /* COND_OP_EQ .. COND_OP_LE */

  /* For str type: */
  int reg_index;            /* GDB register number (0-31 for GPRs, 64=pc,
                               67=lr, 68=ctr), or -1 for absolute addr. */
  int offset;               /* signed offset added to register value */
  CORE_ADDR abs_addr;       /* absolute address when reg_index == -1 */
  char str_value[MAX_COND_STR];

  /* For reg type: */
  unsigned long value;      /* comparison value for reg/mem types */

  /* For mem type: */
  CORE_ADDR mem_addr;       /* memory address to read */
  int mem_size;             /* 1, 2, or 4 bytes */
};

extern struct cond_bp cond_bps[MAX_COND_BPS];

/* Parse a hex-encoded qRcmd command and write response to own_buf.
   own_buf is also hex-encoded in the response. */
void handle_rcmd (char *hex_cmd, char *own_buf);

/* Check if the given PC has a conditional breakpoint.
   Returns:
     0 = not a conditional breakpoint (let normal handling proceed)
     1 = condition met (should stop)
     2 = condition NOT met (should silently resume) */
int check_cond_bp (CORE_ADDR pc);

#endif /* COND_BP_H */
