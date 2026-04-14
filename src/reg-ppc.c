/* Register protocol for Apple GDB on PowerPC Darwin.

   This file defines the register layout that matches Apple's GDB (gdb-437)
   on PowerPC Mac OS X.  Apple uses a different register numbering and
   sizes than standard GDB:

     Regs  0-31:  r0-r31   (GPR,  64-bit each, zero-extended from 32-bit HW)
     Regs 32-63:  f0-f31   (FPR,  64-bit each)
     Regs 64-95:  v0-v31   (VR,  128-bit each, AltiVec)
     Reg  96:     pc       (64-bit, zero-extended from srr0)
     Reg  97:     ps       (64-bit, zero-extended from srr1)
     Reg  98:     cr       (32-bit)
     Reg  99:     lr       (64-bit, zero-extended)
     Reg 100:     ctr      (64-bit, zero-extended)
     Reg 101:     xer      (64-bit, zero-extended)
     Reg 102:     mq       (32-bit, 601 only, zero for G4)
     Reg 103:     fpscr    (32-bit)
     Reg 104:     vscr     (32-bit)
     Reg 105:     vrsave   (32-bit)

   Total: 106 registers, 1084 bytes.
*/

#include "regdef.h"
#include "regcache.h"

struct reg regs_ppc_apple[] = {
  /* GPRs: 64-bit (zero-extended from 32-bit hardware registers) */
  { "r0",    0, 64 },
  { "r1",   64, 64 },
  { "r2",  128, 64 },
  { "r3",  192, 64 },
  { "r4",  256, 64 },
  { "r5",  320, 64 },
  { "r6",  384, 64 },
  { "r7",  448, 64 },
  { "r8",  512, 64 },
  { "r9",  576, 64 },
  { "r10", 640, 64 },
  { "r11", 704, 64 },
  { "r12", 768, 64 },
  { "r13", 832, 64 },
  { "r14", 896, 64 },
  { "r15", 960, 64 },
  { "r16", 1024, 64 },
  { "r17", 1088, 64 },
  { "r18", 1152, 64 },
  { "r19", 1216, 64 },
  { "r20", 1280, 64 },
  { "r21", 1344, 64 },
  { "r22", 1408, 64 },
  { "r23", 1472, 64 },
  { "r24", 1536, 64 },
  { "r25", 1600, 64 },
  { "r26", 1664, 64 },
  { "r27", 1728, 64 },
  { "r28", 1792, 64 },
  { "r29", 1856, 64 },
  { "r30", 1920, 64 },
  { "r31", 1984, 64 },
  /* FPRs: 64-bit */
  { "f0",  2048, 64 },
  { "f1",  2112, 64 },
  { "f2",  2176, 64 },
  { "f3",  2240, 64 },
  { "f4",  2304, 64 },
  { "f5",  2368, 64 },
  { "f6",  2432, 64 },
  { "f7",  2496, 64 },
  { "f8",  2560, 64 },
  { "f9",  2624, 64 },
  { "f10", 2688, 64 },
  { "f11", 2752, 64 },
  { "f12", 2816, 64 },
  { "f13", 2880, 64 },
  { "f14", 2944, 64 },
  { "f15", 3008, 64 },
  { "f16", 3072, 64 },
  { "f17", 3136, 64 },
  { "f18", 3200, 64 },
  { "f19", 3264, 64 },
  { "f20", 3328, 64 },
  { "f21", 3392, 64 },
  { "f22", 3456, 64 },
  { "f23", 3520, 64 },
  { "f24", 3584, 64 },
  { "f25", 3648, 64 },
  { "f26", 3712, 64 },
  { "f27", 3776, 64 },
  { "f28", 3840, 64 },
  { "f29", 3904, 64 },
  { "f30", 3968, 64 },
  { "f31", 4032, 64 },
  /* AltiVec vector registers: 128-bit */
  { "v0",  4096, 128 },
  { "v1",  4224, 128 },
  { "v2",  4352, 128 },
  { "v3",  4480, 128 },
  { "v4",  4608, 128 },
  { "v5",  4736, 128 },
  { "v6",  4864, 128 },
  { "v7",  4992, 128 },
  { "v8",  5120, 128 },
  { "v9",  5248, 128 },
  { "v10", 5376, 128 },
  { "v11", 5504, 128 },
  { "v12", 5632, 128 },
  { "v13", 5760, 128 },
  { "v14", 5888, 128 },
  { "v15", 6016, 128 },
  { "v16", 6144, 128 },
  { "v17", 6272, 128 },
  { "v18", 6400, 128 },
  { "v19", 6528, 128 },
  { "v20", 6656, 128 },
  { "v21", 6784, 128 },
  { "v22", 6912, 128 },
  { "v23", 7040, 128 },
  { "v24", 7168, 128 },
  { "v25", 7296, 128 },
  { "v26", 7424, 128 },
  { "v27", 7552, 128 },
  { "v28", 7680, 128 },
  { "v29", 7808, 128 },
  { "v30", 7936, 128 },
  { "v31", 8064, 128 },
  /* SPRs */
  { "pc",     8192, 64 },   /* reg 96: srr0 */
  { "ps",     8256, 64 },   /* reg 97: srr1 */
  { "cr",     8320, 32 },   /* reg 98 */
  { "lr",     8352, 64 },   /* reg 99 */
  { "ctr",    8416, 64 },   /* reg 100 */
  { "xer",    8480, 64 },   /* reg 101 */
  { "mq",     8544, 32 },   /* reg 102: 601 only */
  { "fpscr",  8576, 32 },   /* reg 103 */
  { "vscr",   8608, 32 },   /* reg 104 */
  { "vrsave", 8640, 32 },   /* reg 105 */
};

const char *expedite_regs_ppc_apple[] = { "r1", "pc", 0 };

void
init_registers (void)
{
  set_register_cache (regs_ppc_apple,
                      sizeof (regs_ppc_apple) / sizeof (regs_ppc_apple[0]));
  gdbserver_expedite_regs = expedite_regs_ppc_apple;
}
