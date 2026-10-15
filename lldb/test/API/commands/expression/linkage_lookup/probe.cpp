extern "C" {
int linkage_library_anchor() { return 0; }
int linkage_target() { return 42; }
int linkage_global = 73;
int present_weak() { return 29; }

// The executable defines data with this linkage spelling. A JIT data
// relocation must use that definition before this other image's function.
int data_probe_decoy() asm("__ZN9DataProbe5valueE");
int data_probe_decoy() { return 11; }

// A C++ expression declaration intentionally omits extern "C". The existing
// alternate-mangling and basename fallback must still find this C definition.
int cpp_fallback_target() { return 17; }
}
