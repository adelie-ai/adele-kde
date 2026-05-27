#!/usr/bin/env bash
# Run the Python unit-test suite for the plasmoid.
#
# Uses stdlib `unittest` because `pytest` is not part of the base KDE
# install and we want CI parity with developer machines.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE/.."
python3 -m unittest discover -s tests/python -p "test_*.py" -v
