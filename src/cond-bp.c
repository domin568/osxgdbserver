/* Conditional breakpoint support for Darwin gdbserver.
   Copyright 2005 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.  */

#include "server.h"
#include "cond-bp.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct cond_bp cond_bps[MAX_COND_BPS];

/* ── Helpers ───────────────────────────────────────────────────── */

/* Hex-encode an ASCII string into dst.  dst must be 2*len+1 bytes. */
static void
hex_encode (char *dst, const char *src, int len)
{
  int i;
  for (i = 0; i < len; i++)
    sprintf (dst + i * 2, "%02x", (unsigned char) src[i]);
  dst[len * 2] = '\0';
}

/* Decode hex string (2 hex chars per byte) into dst.
   Returns number of bytes decoded. */
static int
hex_decode (char *dst, const char *src, int max)
{
  int i = 0;
  while (src[0] && src[1] && i < max - 1)
    {
      int hi, lo;
      hi = src[0];
      lo = src[1];
      if (hi >= '0' && hi <= '9') hi -= '0';
      else if (hi >= 'a' && hi <= 'f') hi = hi - 'a' + 10;
      else if (hi >= 'A' && hi <= 'F') hi = hi - 'A' + 10;
      else break;
      if (lo >= '0' && lo <= '9') lo -= '0';
      else if (lo >= 'a' && lo <= 'f') lo = lo - 'a' + 10;
      else if (lo >= 'A' && lo <= 'F') lo = lo - 'A' + 10;
      else break;
      dst[i++] = (char)((hi << 4) | lo);
      src += 2;
    }
  dst[i] = '\0';
  return i;
}

/* Write a hex-encoded text response into own_buf. */
static void
make_response (char *own_buf, const char *msg)
{
  hex_encode (own_buf, msg, strlen (msg));
}

/* Parse a PPC register name, return GDB register number or -1. */
static int
parse_reg_name (const char *name)
{
  char upper[8];
  int i, rn;

  if (strlen (name) > 7)
    return -1;

  for (i = 0; name[i]; i++)
    upper[i] = toupper ((unsigned char) name[i]);
  upper[i] = '\0';

  if (strcmp (upper, "PC") == 0) return 64;
  if (strcmp (upper, "PS") == 0) return 65;
  if (strcmp (upper, "CR") == 0) return 66;
  if (strcmp (upper, "LR") == 0) return 67;
  if (strcmp (upper, "CTR") == 0) return 68;
  if (strcmp (upper, "XER") == 0) return 69;

  if (upper[0] == 'R' && upper[1] >= '0' && upper[1] <= '9')
    {
      rn = atoi (upper + 1);
      if (rn >= 0 && rn <= 31)
        return rn;
    }

  return -1;
}

/* ── Operator parsing ─────────────────────────────────────────── */

static const char *
op_to_str (int op)
{
  switch (op)
    {
    case COND_OP_EQ: return "==";
    case COND_OP_NE: return "!=";
    case COND_OP_GT: return ">";
    case COND_OP_LT: return "<";
    case COND_OP_GE: return ">=";
    case COND_OP_LE: return "<=";
    }
  return "??";
}

/* Parse operator string.  Returns op code or -1 on error. */
static int
parse_op (const char *s)
{
  if (strcmp (s, "==") == 0) return COND_OP_EQ;
  if (strcmp (s, "!=") == 0) return COND_OP_NE;
  if (strcmp (s, ">") == 0) return COND_OP_GT;
  if (strcmp (s, "<") == 0) return COND_OP_LT;
  if (strcmp (s, ">=") == 0) return COND_OP_GE;
  if (strcmp (s, "<=") == 0) return COND_OP_LE;
  return -1;
}

/* Compare two unsigned 32-bit values using the given operator. */
static int
compare_values (unsigned long current, int op, unsigned long expected)
{
  switch (op)
    {
    case COND_OP_EQ: return current == expected;
    case COND_OP_NE: return current != expected;
    case COND_OP_GT: return current > expected;
    case COND_OP_LT: return current < expected;
    case COND_OP_GE: return current >= expected;
    case COND_OP_LE: return current <= expected;
    }
  return 0;
}

/* ── Expression parser (for str: reg, reg+off, reg-off, abs) ── */

/* Parse an expression like "r9+0xa", "r3", "0x1234" and fill out
   reg_index, offset, abs_addr.  Returns 1 on success. */
static int
parse_expr (const char *expr, int *reg_index, int *offset,
            unsigned long *abs_addr, char *own_buf)
{
  char *sep = NULL;
  char msg[256];
  int i;

  *reg_index = -1;
  *offset = 0;
  *abs_addr = 0;

  for (i = 1; expr[i]; i++)
    {
      if (expr[i] == '+' || expr[i] == '-')
        {
          sep = (char *) &expr[i];
          break;
        }
    }

  if (sep != NULL)
    {
      char reg_part[32];
      int n = sep - expr;
      if (n > 31) n = 31;
      strncpy (reg_part, expr, n);
      reg_part[n] = '\0';

      *reg_index = parse_reg_name (reg_part);
      if (*reg_index == -1)
        {
          sprintf (msg, "Error: unknown register '%s'\n", reg_part);
          make_response (own_buf, msg);
          return 0;
        }
      *offset = (int) strtol (sep, NULL, 0);
    }
  else
    {
      *reg_index = parse_reg_name (expr);
      if (*reg_index == -1)
        {
          char *endp;
          *abs_addr = strtoul (expr, &endp, 16);
          if (endp == expr || *endp != '\0')
            {
              sprintf (msg, "Error: cannot parse '%s' as register or address\n", expr);
              make_response (own_buf, msg);
              return 0;
            }
        }
    }
  return 1;
}

/* Find a free cond_bp slot.  Returns slot index or -1. */
static int
find_free_slot (void)
{
  int i;
  for (i = 0; i < MAX_COND_BPS; i++)
    if (!cond_bps[i].active)
      return i;
  return -1;
}

/* ── cond_bp command parser ───────────────────────────────────── */

/* Parse:
   cond_bp <addr> str <reg+off> ==|!= <string>
   cond_bp <addr> reg <reg_name> <op> <hex_value>
   cond_bp <addr> mem <hex_addr> <size> <op> <hex_value> */
static void
cmd_cond_bp (const char *args, char *own_buf)
{
  char addr_str[32], type_str[16];
  unsigned long bp_addr;
  int slot;
  char msg[512];

  if (sscanf (args, "%31s %15s", addr_str, type_str) < 2)
    {
      make_response (own_buf,
        "Error: usage:\n"
        "  cond_bp <addr> str <reg+off> ==|!= <string>\n"
        "  cond_bp <addr> reg <reg_name> <op> <hex_value>\n"
        "  cond_bp <addr> mem <hex_addr> <size> <op> <hex_value>\n");
      return;
    }

  bp_addr = strtoul (addr_str, NULL, 16);

  slot = find_free_slot ();
  if (slot == -1)
    {
      make_response (own_buf, "Error: max conditional breakpoints reached\n");
      return;
    }

  memset (&cond_bps[slot], 0, sizeof (cond_bps[slot]));

  if (strcmp (type_str, "str") == 0)
    {
      /* ── str type: cond_bp <addr> str <expr> <op> <string> ── */
      char expr[64], op_str[4], target[MAX_COND_STR];
      int reg_index, offset, op, i;
      unsigned long abs_addr;

      if (sscanf (args, "%*s %*s %63s %3s", expr, op_str) < 2)
        {
          make_response (own_buf,
            "Error: usage: cond_bp <addr> str <expr> ==|!= <string>\n");
          return;
        }

      op = parse_op (op_str);
      if (op == -1 || (op != COND_OP_EQ && op != COND_OP_NE))
        {
          make_response (own_buf,
            "Error: string conditions only support == and !=\n");
          return;
        }

      if (!parse_expr (expr, &reg_index, &offset, &abs_addr, own_buf))
        return;

      /* The target string is everything after the operator.
         Skip 4 tokens: addr, type, expr, op. */
      {
        const char *p = args;
        int tok = 0;
        while (tok < 4 && *p)
          {
            while (*p && *p == ' ') p++;
            while (*p && *p != ' ') p++;
            tok++;
          }
        while (*p && *p == ' ') p++;
        if (*p == '\0')
          {
            make_response (own_buf, "Error: missing target string\n");
            return;
          }
        strncpy (target, p, MAX_COND_STR - 1);
        target[MAX_COND_STR - 1] = '\0';
        i = strlen (target) - 1;
        while (i >= 0 && (target[i] == ' ' || target[i] == '\t'
                          || target[i] == '\r' || target[i] == '\n'))
          target[i--] = '\0';
      }

      set_breakpoint_at ((CORE_ADDR) bp_addr, NULL);

      cond_bps[slot].active = 1;
      cond_bps[slot].addr = (CORE_ADDR) bp_addr;
      cond_bps[slot].cond_type = COND_TYPE_STR;
      cond_bps[slot].op = op;
      cond_bps[slot].reg_index = reg_index;
      cond_bps[slot].offset = offset;
      cond_bps[slot].abs_addr = (CORE_ADDR) abs_addr;
      strncpy (cond_bps[slot].str_value, target, MAX_COND_STR - 1);
      cond_bps[slot].str_value[MAX_COND_STR - 1] = '\0';

      sprintf (msg, "OK: cond bp at 0x%lx if str(%s) %s \"%s\"\n",
               bp_addr, expr, op_str, target);
      make_response (own_buf, msg);
      fprintf (stderr, "cond_bp[%d]: 0x%lx str reg=%d off=%d op=%s target=\"%s\"\n",
               slot, bp_addr, reg_index, offset, op_str, target);
    }
  else if (strcmp (type_str, "reg") == 0)
    {
      /* ── reg type: cond_bp <addr> reg <reg_name> <op> <value> ── */
      char reg_name[16], op_str[4], val_str[32];
      int reg_id, op;
      unsigned long value;

      if (sscanf (args, "%*s %*s %15s %3s %31s", reg_name, op_str, val_str) < 3)
        {
          make_response (own_buf,
            "Error: usage: cond_bp <addr> reg <reg_name> <op> <hex_value>\n");
          return;
        }

      reg_id = parse_reg_name (reg_name);
      if (reg_id == -1)
        {
          sprintf (msg, "Error: unknown register '%s'\n", reg_name);
          make_response (own_buf, msg);
          return;
        }

      op = parse_op (op_str);
      if (op == -1)
        {
          sprintf (msg, "Error: unknown operator '%s'\n", op_str);
          make_response (own_buf, msg);
          return;
        }

      value = strtoul (val_str, NULL, 16);

      set_breakpoint_at ((CORE_ADDR) bp_addr, NULL);

      cond_bps[slot].active = 1;
      cond_bps[slot].addr = (CORE_ADDR) bp_addr;
      cond_bps[slot].cond_type = COND_TYPE_REG;
      cond_bps[slot].op = op;
      cond_bps[slot].reg_index = reg_id;
      cond_bps[slot].value = value;

      sprintf (msg, "OK: cond bp at 0x%lx if %s %s 0x%lx\n",
               bp_addr, reg_name, op_str, value);
      make_response (own_buf, msg);
      fprintf (stderr, "cond_bp[%d]: 0x%lx reg %s(%d) %s 0x%lx\n",
               slot, bp_addr, reg_name, reg_id, op_str, value);
    }
  else if (strcmp (type_str, "mem") == 0)
    {
      /* ── mem type: cond_bp <addr> mem <hex_addr> <size> <op> <value> ── */
      char maddr_str[32], op_str[4], val_str[32];
      unsigned long mem_addr, value;
      int mem_size, op;

      if (sscanf (args, "%*s %*s %31s %d %3s %31s",
                  maddr_str, &mem_size, op_str, val_str) < 4)
        {
          make_response (own_buf,
            "Error: usage: cond_bp <addr> mem <hex_addr> <size> <op> <hex_value>\n");
          return;
        }

      mem_addr = strtoul (maddr_str, NULL, 16);

      if (mem_size != 1 && mem_size != 2 && mem_size != 4)
        {
          make_response (own_buf, "Error: size must be 1, 2, or 4\n");
          return;
        }

      op = parse_op (op_str);
      if (op == -1)
        {
          sprintf (msg, "Error: unknown operator '%s'\n", op_str);
          make_response (own_buf, msg);
          return;
        }

      value = strtoul (val_str, NULL, 16);

      set_breakpoint_at ((CORE_ADDR) bp_addr, NULL);

      cond_bps[slot].active = 1;
      cond_bps[slot].addr = (CORE_ADDR) bp_addr;
      cond_bps[slot].cond_type = COND_TYPE_MEM;
      cond_bps[slot].op = op;
      cond_bps[slot].mem_addr = (CORE_ADDR) mem_addr;
      cond_bps[slot].mem_size = mem_size;
      cond_bps[slot].value = value;

      sprintf (msg, "OK: cond bp at 0x%lx if mem[0x%lx:%d] %s 0x%lx\n",
               bp_addr, mem_addr, mem_size, op_str, value);
      make_response (own_buf, msg);
      fprintf (stderr, "cond_bp[%d]: 0x%lx mem 0x%lx size=%d %s 0x%lx\n",
               slot, bp_addr, mem_addr, mem_size, op_str, value);
    }
  else
    {
      sprintf (msg, "Error: unknown type '%s' (use 'str', 'reg', or 'mem')\n",
               type_str);
      make_response (own_buf, msg);
    }
}

/* ── remove_cond_bp command ───────────────────────────────────── */

static void
cmd_remove_cond_bp (const char *args, char *own_buf)
{
  unsigned long addr;
  int i, found = 0;
  char msg[128];

  addr = strtoul (args, NULL, 16);

  for (i = 0; i < MAX_COND_BPS; i++)
    {
      if (cond_bps[i].active && cond_bps[i].addr == (CORE_ADDR) addr)
        {
          cond_bps[i].active = 0;
          found = 1;
        }
    }

  if (found)
    {
      sprintf (msg, "OK: removed conditional breakpoint at 0x%lx\n", addr);
      fprintf (stderr, "cond_bp: removed at 0x%lx\n", addr);
    }
  else
    sprintf (msg, "Error: no conditional breakpoint at 0x%lx\n", addr);

  make_response (own_buf, msg);
}

/* ── help command ─────────────────────────────────────────────── */

static void
cmd_help (char *own_buf)
{
  make_response (own_buf,
    "Monitor commands:\n"
    "  cond_bp <addr> reg <reg> <op> <hex_value>\n"
    "  cond_bp <addr> mem <hex_addr> <size> <op> <hex_value>\n"
    "  cond_bp <addr> str <reg+off> ==|!= <string>\n"
    "  remove_cond_bp <addr>\n"
    "  help\n"
    "Operators: == != > < >= <=\n"
    "Examples:\n"
    "  cond_bp 4c4dc reg r3 == 0\n"
    "  cond_bp 4c4dc mem bffeff40 4 != 0\n"
    "  cond_bp 803e0 str r9+0xa == GetMenuItemHierarchicalID\n");
}

/* ── Public: handle qRcmd ─────────────────────────────────────── */

void
handle_rcmd (char *hex_cmd, char *own_buf)
{
  char cmd[1024];
  int len;

  len = hex_decode (cmd, hex_cmd, sizeof (cmd));
  if (len <= 0)
    {
      make_response (own_buf, "Error: empty command\n");
      return;
    }

  fprintf (stderr, "monitor: \"%s\"\n", cmd);

  if (strncmp (cmd, "cond_bp ", 8) == 0)
    cmd_cond_bp (cmd + 8, own_buf);
  else if (strncmp (cmd, "remove_cond_bp ", 15) == 0)
    cmd_remove_cond_bp (cmd + 15, own_buf);
  else if (strcmp (cmd, "help") == 0 || cmd[0] == '\0')
    cmd_help (own_buf);
  else
    {
      char msg[256];
      sprintf (msg, "Error: unknown command '%s'. Type 'help' for usage.\n", cmd);
      make_response (own_buf, msg);
    }
}

/* ── Public: check conditional breakpoint at PC ───────────────── */

/* Read a null-terminated string from the inferior at address ADDR.
   Returns length read.  BUF is filled up to MAX bytes. */
static int
read_inferior_string (CORE_ADDR addr, char *buf, int max)
{
  int i;
  for (i = 0; i < max - 1; i++)
    {
      buf[i] = 0;
      read_inferior_memory (addr + i, buf + i, 1);
      if (buf[i] == '\0')
        return i;
    }
  buf[i] = '\0';
  return i;
}

int
check_cond_bp (CORE_ADDR pc)
{
  int i;
  struct cond_bp *cb;

  for (i = 0; i < MAX_COND_BPS; i++)
    {
      cb = &cond_bps[i];
      if (!cb->active)
        continue;
      if (cb->addr != pc)
        continue;

      /* Found a conditional breakpoint at this PC. */

      if (cb->cond_type == COND_TYPE_STR)
        {
          /* String comparison. */
          CORE_ADDR str_addr;
          char str_buf[MAX_COND_STR];
          int match;

          if (cb->reg_index >= 0)
            {
              unsigned int reg_val = 0;
              collect_register (cb->reg_index, &reg_val);
              str_addr = (CORE_ADDR)((long) reg_val + cb->offset);
            }
          else
            str_addr = cb->abs_addr;

          read_inferior_string (str_addr, str_buf, MAX_COND_STR);

          match = (strcmp (str_buf, cb->str_value) == 0);

          if (cb->op == COND_OP_EQ)
            {
              fprintf (stderr, "cond_bp: 0x%lx str(\"%s\") == \"%s\" -> %s\n",
                       (unsigned long) pc, str_buf, cb->str_value,
                       match ? "STOP" : "continue");
              return match ? 1 : 2;
            }
          else
            {
              fprintf (stderr, "cond_bp: 0x%lx str(\"%s\") != \"%s\" -> %s\n",
                       (unsigned long) pc, str_buf, cb->str_value,
                       !match ? "STOP" : "continue");
              return !match ? 1 : 2;
            }
        }
      else if (cb->cond_type == COND_TYPE_REG)
        {
          /* Register comparison. */
          unsigned int reg_val = 0;
          int result;

          collect_register (cb->reg_index, &reg_val);
          result = compare_values ((unsigned long) reg_val, cb->op, cb->value);

          fprintf (stderr, "cond_bp: 0x%lx reg(%d)=0x%x %s 0x%lx -> %s\n",
                   (unsigned long) pc, cb->reg_index, reg_val,
                   op_to_str (cb->op), cb->value,
                   result ? "STOP" : "continue");
          return result ? 1 : 2;
        }
      else if (cb->cond_type == COND_TYPE_MEM)
        {
          /* Memory comparison (big-endian PPC). */
          unsigned char mem_buf[4];
          unsigned long current = 0;
          int result;

          memset (mem_buf, 0, sizeof (mem_buf));
          read_inferior_memory (cb->mem_addr, mem_buf, cb->mem_size);

          /* Big-endian byte order. */
          switch (cb->mem_size)
            {
            case 1:
              current = mem_buf[0];
              break;
            case 2:
              current = ((unsigned long) mem_buf[0] << 8) | mem_buf[1];
              break;
            case 4:
              current = ((unsigned long) mem_buf[0] << 24)
                        | ((unsigned long) mem_buf[1] << 16)
                        | ((unsigned long) mem_buf[2] << 8)
                        | mem_buf[3];
              break;
            }

          result = compare_values (current, cb->op, cb->value);

          fprintf (stderr,
                   "cond_bp: 0x%lx mem[0x%lx:%d]=0x%lx %s 0x%lx -> %s\n",
                   (unsigned long) pc, (unsigned long) cb->mem_addr,
                   cb->mem_size, current, op_to_str (cb->op), cb->value,
                   result ? "STOP" : "continue");
          return result ? 1 : 2;
        }
    }

  return 0;  /* Not a conditional breakpoint. */
}
