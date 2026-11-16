#!/usr/bin/env bash
# Build an xllm wheel that ships the YuanRong KV cache store backend.
#
#   ./build_xllm_yuanrong_wheel.sh --device npu
#   ./build_xllm_yuanrong_wheel.sh --device cuda
#
# Environment:
#   YUANRONG_ROOT   Installed DS static PIC SDK prefix containing
#                   lib/cmake/DatasystemPrivate (or lib64/cmake/...). Required.
#   CMAKE_ARGS      extra cmake arguments, appended to the ones set here.
#
# The xllm binary cannot be built without a device toolchain: the CUDA path
# needs a CUDA-enabled torch (NCCL symbols), the NPU path needs CANN. This
# script fails fast with a clear message instead of letting cmake fail deep in
# a third-party target.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEVICE="npu"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

log() { printf '[build] %s\n' "$*"; }
die() { printf '[build] ERROR: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- SDK --------
[[ -n "${YUANRONG_ROOT:-}" ]] || die "Set YUANRONG_ROOT to the installed DS static PIC SDK prefix."
[[ -f "${YUANRONG_ROOT}/lib/cmake/DatasystemPrivate/DatasystemPrivateConfig.cmake" || \
   -f "${YUANRONG_ROOT}/lib64/cmake/DatasystemPrivate/DatasystemPrivateConfig.cmake" ]] \
  || die "${YUANRONG_ROOT} has no DatasystemPrivate CMake package; a DS Python wheel is insufficient."
log "DS static PIC SDK: ${YUANRONG_ROOT}"

# ----------------------------------------------------------- toolchain ------
case "${DEVICE}" in
  npu)
    CANN="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
    [[ -d "${CANN}" ]] || die "CANN not found at ${CANN}; export ASCEND_HOME_PATH or source set_env.sh"
    # Importing torch_npu also loads the device driver; compilation only needs
    # the installed package's headers and libraries, which CMake checks later.
    python3 -c 'from importlib.metadata import distribution; distribution("torch_npu")' \
      2>/dev/null || die "torch_npu is not installed; the NPU build needs it"
    ;;
  cuda)
    python3 -c 'import sys, torch; sys.exit(0 if torch.cuda.is_available() else 1)' \
      || die "torch has no CUDA (torch.version.cuda is empty); install a CUDA build of torch"
    ;;
  mlu|ilu|musa|dcu|maca)
    log "device ${DEVICE}: relying on its own SDK being on the include/link path"
    ;;
  *) die "unsupported device: ${DEVICE}" ;;
esac
log "device: ${DEVICE}"

# --------------------------------------------------------------- build ------
cd "${ROOT}"
log "cmake args: -DUSE_YUANRONG=ON -DYUANRONG_ROOT=${YUANRONG_ROOT} ${CMAKE_ARGS:-}"
CMAKE_ARGS="-DUSE_YUANRONG=ON -DYUANRONG_ROOT=${YUANRONG_ROOT} ${CMAKE_ARGS:-}" \
  python3 setup.py --device "${DEVICE}" bdist_wheel

WHEEL="$(ls -t "${ROOT}"/dist/*.whl 2>/dev/null | head -1 || true)"
[[ -n "${WHEEL}" ]] || die "no wheel produced under dist/"
log "wheel: ${WHEEL}"

# --------------------------------------------------------------- verify -----
# The bridge is a build dependency of both hosts and is staged by CMake.
# Reject incomplete packages instead of injecting a possibly stale library.
python3 - "${WHEEL}" <<'PY'
import sys
import zipfile
from pathlib import PurePosixPath

from scripts.logger import logger

wheel_path = sys.argv[1]
bridge_name = "libxllm_yuanrong_store_bridge.so"

with zipfile.ZipFile(wheel_path) as wheel:
    names = set(wheel.namelist())

hosts = [name for name in names if name == "xllm/xllm" or name == "xllm/libxllm.so"]
if len(hosts) != 1:
    logger.error("Wheel must contain exactly one xllm host artifact")
    sys.exit(1)
bridge_path = str(PurePosixPath(hosts[0]).with_name(bridge_name))
if bridge_path not in names:
    logger.error(f"Wheel is missing {bridge_path}; rebuild with USE_YUANRONG=ON")
    sys.exit(1)
logger.info(f"Wheel contains {bridge_path}")
PY

log "done"
log "install:  pip install ${WHEEL}"
log "check:    xllm --store_backend=yuanrong --store_yuanrong_host=<worker> --store_yuanrong_port=<port>"
