#!/usr/bin/env bash
# Unit tests and the package guard, run locally (the GitHub ci workflow was removed; release.yml runs the guard)
set -e
cd "$(dirname "$0")/.."
python -X utf8 -W error::ResourceWarning -m unittest discover -s tests/unit -v
python -X utf8 tools/package_guard.py --git .
