#!/usr/bin/env bash
# Run the QML autotests via qmltestrunner.
#
# These tests import the production QML modules from
# `shared/chat-module/ui/`, so they need the QML import path to include
# both that directory and our test stubs.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

# Prefer the Qt6 qmltestrunner — KDE Plasma 6 is the target runtime, and
# the legacy `/usr/bin/qmltestrunner` symlink points at the Qt5 build on
# many distros (Arch / CachyOS in particular).
RUNNER="qmltestrunner"
for candidate in \
    /usr/lib/qt6/bin/qmltestrunner \
    /usr/lib/qt6/libexec/qmltestrunner \
    qmltestrunner-qt6 \
    qmltestrunner; do
    if command -v "$candidate" >/dev/null 2>&1 || [ -x "$candidate" ]; then
        RUNNER="$candidate"
        break
    fi
done

# Each TestCase QML file is a complete test program.
EXIT=0
for f in "$HERE"/qml/tst_*.qml; do
    echo "=== $(basename "$f") ==="
    if ! "$RUNNER" -input "$f" -import "$REPO_ROOT/shared/chat-module/ui"; then
        EXIT=1
    fi
done
exit "$EXIT"
