#!/bin/bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
bash "$HERE/clients.sh" stop
bash "$HERE/dashboard.sh" stop
bash "$HERE/backend.sh" stop
