#!/usr/bin/env bash
set -euo pipefail
exec "${TUNTOM_DEPLOY_PYTHON:-python3}" "$(dirname -- "${BASH_SOURCE[0]}")/helpers/controller.py" "$@"
