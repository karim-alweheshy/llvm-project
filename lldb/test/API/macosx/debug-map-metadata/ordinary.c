struct OrdinaryType { int value; };
int ordinary(int input) {
  struct OrdinaryType local = {input};
  return local.value + 1; // ordinary source line
}
