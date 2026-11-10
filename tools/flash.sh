#!/bin/sh
# 烧写本工程固件（自动 source 本机 ESP-IDF 环境）。
#   ./tools/flash.sh [串口]        不传串口时自动找 /dev/cu.usbmodem*
# 烧写完成后可自行跑 monitor：
#   . ~/esp/esp-idf/export.sh && idf.py -p <串口> monitor
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJ="$DIR/.."

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export IDF_GITHUB_ASSETS="${IDF_GITHUB_ASSETS:-dl.espressif.cn/github_assets}"

PORT=${1:-}
if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
fi
if [ -z "$PORT" ]; then
    echo "没找到串口，请显式传入，例如 ./tools/flash.sh /dev/cu.usbmodem14101" >&2
    exit 1
fi

. "$IDF_PATH/export.sh" >/dev/null

cd "$PROJ"
echo "串口: $PORT"
exec idf.py -p "$PORT" flash
