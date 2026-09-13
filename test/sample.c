// Deliberately written to give SimplifyCFG/InstCombine/etc real CFG work to
// do: a redundant branch, a dead block, and a trivially foldable condition.
int helper(int a, int b) {
  int sum = a + b;
  return sum * 2;
}

int branchy(int x) {
  int r;
  if (x > 0) {
    r = helper(x, 1);
  } else {
    r = helper(x, -1);
  }
  if (1) {
    r = r + 1;
  } else {
    r = r - 1; // dead branch, should be folded away
  }
  return r;
}

int loopy(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    total += i * i;
  }
  return total;
}
