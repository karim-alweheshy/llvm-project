namespace Unrelated {
int value() { return 42; }
}

extern "C" int unrelated(void) { return Unrelated::value(); }
