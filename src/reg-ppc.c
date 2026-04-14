/* Standard GDB register layout for PowerPC.

   This file defines the register layout matching IDA Pro and standard
   GDB remote protocol for 32-bit PowerPC:

     Regs  0-31:  r0-r31   (GPR,  32-bit each)
     Regs 32-63:  f0-f31   (FPR,  64-bit each)
     Reg  64:     pc       (32-bit, srr0)
     Reg  65:     ps       (32-bit, srr1/MSR)
     Reg  66:     cr       (32-bit)
     Reg  67:     lr       (32-bit)
     Reg  68:     ctr      (32-bit)
     Reg  69:     xer      (32-bit)
     Reg  70:     fpscr    (32-bit)

   Total: 71 registers, 412 bytes.
*/

#include "regdef.h"
#include "regcache.h"

struct reg regs_ppc[] = {
  /* GPRs: 32-bit */
  { "r0",    0, 32 },
  { "r1",   32, 32 },
  { "r2",   64, 32 },
  { "r3",   96, 32 },
  { "r4",  128, 32 },
  { "r5",  160, 32 },
  { "r6",  192, 32 },
  { "r7",  224, 32 },
  { "r8",  256, 32 },
  { "r9",  288, 32 },
  { "r10", 320, 32 },
  { "r11", 352, 32 },
  { "r12", 384, 32 },
  { "r13", 416, 32 },
  { "r14", 448, 32 },
  { "r15", 480, 32 },
  { "r16", 512, 32 },
  { "r17", 544, 32 },
  { "r18", 576, 32 },
  { "r19", 608, 32 },
  { "r20", 640, 32 },
  { "r21", 672, 32 },
  { "r22", 704, 32 },
  { "r23", 736, 32 },
  { "r24", 768, 32 },
  { "r25", 800, 32 },
  { "r26", 832, 32 },
  { "r27", 864, 32 },
  { "r28", 896, 32 },
  { "r29", 928, 32 },
  { "r30", 960, 32 },
  { "r31", 992, 32 },
  /* FPRs: 64-bit */
  { "f0",  1024, 64 },
  { "f1",  1088, 64 },
  { "f2",  1152, 64 },
  { "f3",  1216, 64 },
  { "f4",  1280, 64 },
  { "f5",  1344, 64 },
  { "f6",  1408, 64 },
  { "f7",  1472, 64 },
  { "f8",  1536, 64 },
  { "f9",  1600, 64 },
  { "f10", 1664, 64 },
  { "f11", 1728, 64 },
  { "f12", 1792, 64 },
  { "f13", 1856, 64 },
  { "f14", 1920, 64 },
  { "f15", 1984, 64 },
  { "f16", 2048, 64 },
  { "f17", 2112, 64 },
  { "f18", 2176, 64 },
  { "f19", 2240, 64 },
  { "f20", 2304, 64 },
  { "f21", 2368, 64 },
  { "f22", 2432, 64 },
  { "f23", 2496, 64 },
  { "f24", 2560, 64 },
  { "f25", 2624, 64 },
  { "f26", 2688, 64 },
  { "f27", 2752, 64 },
  { "f28", 2816, 64 },
  { "f29", 2880, 64 },
  { "f30", 2944, 64 },
  { "f31", 3008, 64 },
  /* SPRs: 32-bit */
  { "pc",    3072, 32 },   /* reg 64: srr0 */
  { "ps",    3104, 32 },   /* reg 65: srr1/MSR */
  { "cr",    3136, 32 },   /* reg 66 */
  { "lr",    3168, 32 },   /* reg 67 */
  { "ctr",   3200, 32 },   /* reg 68 */
  { "xer",   3232, 32 },   /* reg 69 */
  { "fpscr", 3264, 32 },   /* reg 70 */
};

const char *expedite_regs_ppc[] = { "r1", "pc", 0 };

void
init_registers (void)
{
  set_register_cache (regs_ppc,
                      sizeof (regs_ppc) / sizeof (regs_ppc[0]));
  gdbserver_expedite_regs = expedite_regs_ppc;
}
