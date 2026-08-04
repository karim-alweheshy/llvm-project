// RUN: touch %t.o
//
// Check that we're not passing -lto-pass-remarks-output if not requested
// RUN: %clang -target x86_64-apple-darwin12 %t.o -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=NO_PASS_REMARKS_OUTPUT %s < %t.log
// NO_PASS_REMARKS_OUTPUT-NOT: -lto-pass-remarks
// Check that we're passing -lto-pass-remarks-output for LTO
// RUN: %clang -target x86_64-apple-darwin12 %t.o -fsave-optimization-record -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_OUTPUT %s < %t.log
// PASS_REMARKS_OUTPUT: "-mllvm" "-lto-pass-remarks-output" "-mllvm" "foo/bar.out.opt.yaml" "-mllvm" "-lto-pass-remarks-format=yaml"
// PASS_REMARKS_OUTPUT-NOT: -lto-pass-remarks-with-hotness
// PASS_REMARKS_OUTPUT-NOT: --opt-remarks-

// Check native lld options for ordinary, Full LTO, and ThinLTO links.
// RUN: %clang -target x86_64-apple-darwin12 %t.o -fuse-ld=lld \
// RUN:   -B%S/Inputs/lld -fsave-optimization-record -### \
// RUN:   -o foo/bar.out 2>&1 | FileCheck \
// RUN:   --check-prefixes=LLD_PASS_REMARKS_OUTPUT,LLD_NO_LEGACY %s
// RUN: %clang -target x86_64-apple-darwin12 %t.o -fuse-ld=lld \
// RUN:   -B%S/Inputs/lld -flto=full -fsave-optimization-record -### \
// RUN:   -o foo/bar.out 2>&1 | FileCheck \
// RUN:   --check-prefixes=LLD_PASS_REMARKS_OUTPUT,LLD_NO_LEGACY %s
// RUN: %clang -target x86_64-apple-darwin12 %t.o -fuse-ld=lld \
// RUN:   -B%S/Inputs/lld -flto=thin -fsave-optimization-record -### \
// RUN:   -o foo/bar.out 2>&1 | FileCheck \
// RUN:   --check-prefixes=LLD_PASS_REMARKS_OUTPUT,LLD_NO_LEGACY %s
// LLD_PASS_REMARKS_OUTPUT: "{{.*}}ld64.lld"
// LLD_PASS_REMARKS_OUTPUT-SAME: "--opt-remarks-filename=foo/bar.out.opt.yaml"
// LLD_PASS_REMARKS_OUTPUT-SAME: "--opt-remarks-format=yaml"
// LLD_PASS_REMARKS_OUTPUT-NOT: --opt-remarks-passes
// LLD_PASS_REMARKS_OUTPUT-NOT: --opt-remarks-with-hotness
// LLD_NO_LEGACY-NOT: -lto-pass-remarks

// Check that every optimization-record option uses its native lld spelling.
// RUN: %clang -target x86_64-apple-darwin12 %t.o -fuse-ld=lld \
// RUN:   -B%S/Inputs/lld -flto=thin -fsave-optimization-record=some-format \
// RUN:   -foptimization-record-file=remarks-custom.opt \
// RUN:   -foptimization-record-passes=inline -fprofile-instr-use=blah \
// RUN:   -fdiagnostics-hotness-threshold=100 -### -o foo/bar.out 2>&1 | \
// RUN:   FileCheck --check-prefixes=LLD_ALL_REMARKS,LLD_NO_LEGACY %s
// LLD_ALL_REMARKS: "{{.*}}ld64.lld"
// LLD_ALL_REMARKS-SAME: "--opt-remarks-filename=remarks-custom.opt"
// LLD_ALL_REMARKS-SAME: "--opt-remarks-passes=inline"
// LLD_ALL_REMARKS-SAME: "--opt-remarks-format=some-format"
// LLD_ALL_REMARKS-SAME: "--opt-remarks-with-hotness"
// LLD_ALL_REMARKS-SAME: "--opt-remarks-hotness-threshold=100"

// RUN: %clang -target x86_64-apple-darwin12 %t.o -fsave-optimization-record -### 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_OUTPUT_NO_O %s < %t.log
// PASS_REMARKS_OUTPUT_NO_O: "-mllvm" "-lto-pass-remarks-output" "-mllvm" "a.out.opt.yaml"

// RUN: %clang -target x86_64-apple-darwin12 %t.o -fsave-optimization-record -fprofile-instr-use=blah -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_WITH_HOTNESS %s < %t.log
// PASS_REMARKS_WITH_HOTNESS: "-mllvm" "-lto-pass-remarks-output" "-mllvm" "foo/bar.out.opt.yaml" "-mllvm" "-lto-pass-remarks-format=yaml" "-mllvm" "-lto-pass-remarks-with-hotness"

// RUN: %clang -target x86_64-apple-darwin12 %t.o -fsave-optimization-record -fprofile-instr-use=blah -fdiagnostics-hotness-threshold=100 -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_WITH_HOTNESS_THRESHOLD %s < %t.log
// PASS_REMARKS_WITH_HOTNESS_THRESHOLD: "-mllvm" "-lto-pass-remarks-output" "-mllvm" "foo/bar.out.opt.yaml" "-mllvm" "-lto-pass-remarks-format=yaml" "-mllvm" "-lto-pass-remarks-with-hotness" "-mllvm" "-lto-pass-remarks-hotness-threshold=100"

// RUN: %clang -target x86_64-apple-darwin12 %t.o -fsave-optimization-record -foptimization-record-passes=inline -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_WITH_PASSES %s < %t.log
// PASS_REMARKS_WITH_PASSES: "-mllvm" "-lto-pass-remarks-output" "-mllvm" "foo/bar.out.opt.yaml" "-mllvm" "-lto-pass-remarks-filter=inline"
//
// RUN: %clang -target x86_64-apple-darwin12 %t.o -fsave-optimization-record=some-format -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_WITH_FORMAT %s < %t.log
// PASS_REMARKS_WITH_FORMAT: "-mllvm" "-lto-pass-remarks-output" "-mllvm" "foo/bar.out.opt.some-format" "-mllvm" "-lto-pass-remarks-format=some-format"

// RUN: %clang -target x86_64-apple-darwin12 %t.o -foptimization-record-file=remarks-custom.opt.yaml -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_WITH_FILE %s < %t.log
// PASS_REMARKS_WITH_FILE: "-mllvm" "-lto-pass-remarks-output" "-mllvm" "remarks-custom.opt.yaml"

// RUN: %clang -target x86_64-apple-darwin12 -arch x86_64 -arch x86_64h %t.o -fsave-optimization-record -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_WITH_FAT %s < %t.log
// PASS_REMARKS_WITH_FAT: "-arch" "x86_64"{{.*}}"-mllvm" "-lto-pass-remarks-output"
// PASS_REMARKS_WITH_FAT-NEXT: "-arch" "x86_64h"{{.*}}"-mllvm" "-lto-pass-remarks-output"
//
// RUN: not %clang -target x86_64-apple-darwin12 -arch x86_64 -arch x86_64h %t.o -foptimization-record-file=custom.opt.yaml -### -o foo/bar.out 2> %t.log
// RUN: FileCheck -check-prefix=PASS_REMARKS_WITH_FILE_FAT %s < %t.log
// PASS_REMARKS_WITH_FILE_FAT: error: cannot use '-foptimization-record-file' output with multiple -arch options
