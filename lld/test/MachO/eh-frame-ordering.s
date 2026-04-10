# REQUIRES: x86, aarch64
## Test that __eh_frame CIE/FDE ordering is preserved even when section
## priorities (from order files, BP sorting, etc.) would otherwise reorder
## input sections. CIE records must precede the FDE records that reference
## them; reordering breaks CIE pointer resolution.

# RUN: rm -rf %t; split-file %s %t

## x86_64
# RUN: llvm-mc -filetype=obj -emit-compact-unwind-non-canonical=true -triple=x86_64-apple-macos10.15 %t/test.s -o %t/test-x86_64.o
# RUN: %lld -lSystem -lc++ %t/test-x86_64.o -o %t/test-x86_64 -order_file %t/order.txt
# RUN: llvm-objdump --dwarf=frames %t/test-x86_64 2>&1 | FileCheck %s

## arm64
# RUN: llvm-mc -filetype=obj -emit-compact-unwind-non-canonical=true -triple=arm64-apple-macos11.0 %t/test.s -o %t/test-arm64.o
# RUN: %lld -arch arm64 -lSystem -lc++ %t/test-arm64.o -o %t/test-arm64 -order_file %t/order.txt
# RUN: llvm-objdump --dwarf=frames %t/test-arm64 2>&1 | FileCheck %s

## Verify that CIE records precede their FDE records and that
## FDE records successfully reference their CIEs (no parse errors).
# CHECK: .eh_frame contents:
# CHECK: {{[0-9a-f]+}} {{.*}} CIE
# CHECK-NOT: error
# CHECK-NOT: warning
# CHECK: {{[0-9a-f]+}} {{.*}} FDE
# CHECK-NOT: error
# CHECK-NOT: warning
# CHECK: {{[0-9a-f]+}} {{.*}} FDE

#--- order.txt
## Order _h before _g to trigger reordering pressure on the eh_frame entries.
_h
_g
_main

#--- test.s
.globl _my_personality, _main

.text
## _f uses compact unwind (no cfi_escape), so no FDE in output.
.p2align 2
_f:
  .cfi_startproc
  .cfi_personality 155, ___gxx_personality_v0
  .cfi_lsda 16, Lexception0
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc

.p2align 2
_no_unwind:
  ret

## _g uses cfi_escape to force DWARF unwind (can't be compact-encoded).
.p2align 2
_g:
  .cfi_startproc
  .cfi_personality 155, ___gxx_personality_v0
  .cfi_lsda 16, Lexception1
  .cfi_def_cfa_offset 8
  .cfi_escape 0x2e, 0x10
  ret
  .cfi_endproc

## _h also uses cfi_escape with a different personality to get a second CIE.
.p2align 2
_h:
  .cfi_startproc
  .cfi_personality 155, _my_personality
  .cfi_lsda 16, Lexception2
  .cfi_def_cfa_offset 8
  .cfi_escape 0x2e, 0x10
  ret
  .cfi_endproc

.p2align 2
_my_personality:
  ret

.p2align 2
_main:
  ret

.section __TEXT,__gcc_except_tab
GCC_except_table0:
Lexception0:
  .byte 255

GCC_except_table1:
Lexception1:
  .byte 255

GCC_except_table2:
Lexception2:
  .byte 255

.subsections_via_symbols
