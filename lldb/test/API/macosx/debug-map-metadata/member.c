struct ArchivedType { int value; };
int archived(int input) {
  struct ArchivedType local = {input};
  return local.value + 2; // archived source line
}
