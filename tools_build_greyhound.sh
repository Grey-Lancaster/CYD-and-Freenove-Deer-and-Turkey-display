#!/usr/bin/env bash
# Build Deer Hunt on greyhound (cached toolchains, much faster than this PC), bring the binary back, optionally OTA it.
#   ./tools_build_greyhound.sh [env] [ota-host]
#   ./tools_build_greyhound.sh freenove28 deerhunt28.local
# Needs the "GreyHound" entry in ~/.ssh/config. Output: dist/Deer-Hunt-<env>-update.bin
set -euo pipefail
ENV_NAME="${1:-freenove28}"
OTA_HOST="${2:-}"
SSH=(ssh -o BatchMode=yes -o ControlMaster=no -o ControlPath=none GreyHound)
REMOTE='~/cyd-build/Deer-Hunt'
cd "$(dirname "$0")"

tar -czf - platformio.ini boards include lib_freenove src | "${SSH[@]}" "mkdir -p $REMOTE && tar -xzf - -C $REMOTE"
"${SSH[@]}" "cd $REMOTE && ~/.platformio/penv/bin/pio run -e $ENV_NAME 2>&1 | grep -E 'error|SUCCESS|FAILED|Flash:'"
mkdir -p dist
scp -o BatchMode=yes -o ControlMaster=no -o ControlPath=none "GreyHound:cyd-build/Deer-Hunt/.pio/build/$ENV_NAME/firmware.bin" "dist/Deer-Hunt-$ENV_NAME-update.bin"
echo "dist/Deer-Hunt-$ENV_NAME-update.bin"

if [ -n "$OTA_HOST" ]; then
  F="dist/Deer-Hunt-$ENV_NAME-update.bin"
  H=$(md5sum "$F" | cut -d' ' -f1)
  curl -s -m 10 -w " [start %{http_code}]\n" "http://$OTA_HOST/ota/start?mode=fr&hash=$H"
  curl -s -m 180 -w " [upload %{http_code}]\n" -F "file=@$F;filename=firmware.bin" "http://$OTA_HOST/ota/upload"
fi
