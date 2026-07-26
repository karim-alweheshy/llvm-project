; REQUIRES: aarch64-registered-target

;; The prevailing raw version may come from a frontend built from an older LLVM
;; release. Ensure the CS instrumentation backend updates it to describe the
;; profile records that the backend actually emits.
; RUN: opt -module-summary %s -o %t.bc
; RUN: llvm-lto2 run -lto-cspgo-profile-file=alloc -lto-cspgo-gen \
; RUN:   -pgo-temporal-instrumentation -save-temps -o %t %t.bc \
; RUN:   -r=%t.bc,main,plx \
; RUN:   -r=%t.bc,__llvm_profile_raw_version,plx
; RUN: llvm-dis %t.1.4.opt.bc -o - | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-linux-gnu"

;; Raw version 10 with the IR and CSIR variant masks.
@__llvm_profile_raw_version = hidden constant i64 216172782113783818
@llvm.compiler.used = appending global [1 x ptr] [ptr @__llvm_profile_raw_version], section "llvm.metadata"

; CHECK: @__llvm_profile_raw_version = hidden constant i64 -9007199254740991989

define i32 @main() {
entry:
  ret i32 0
}
