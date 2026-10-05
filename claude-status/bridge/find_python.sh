# Sourced by the install scripts. Sets PY to a usable Python 3.8+, preferring
# $PYTHON, then Homebrew's, then Apple's (only if the Command Line Tools are
# installed - otherwise /usr/bin/python3 is a stub that pops up an install dialog).
find_python() {
  for p in "$PYTHON" /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
    [ -n "$p" ] && [ -x "$p" ] || continue
    if [ "$p" = /usr/bin/python3 ] && ! xcode-select -p >/dev/null 2>&1; then continue; fi
    if "$p" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
      PY="$p"
      return 0
    fi
  done
  return 1
}

if ! find_python; then
  echo "Python 3 is needed. macOS will offer to install Apple's Command Line Tools -"
  echo "click Install, wait for it to finish, then run this again."
  xcode-select --install 2>/dev/null || true
  exit 1
fi
