#!/bin/sh
# 编译本工程（自动 source 本机 ESP-IDF 环境）。
#   ./tools/build.sh [idf.py 附加参数]
# IDF 位置默认 ~/esp/esp-idf，可用环境变量 IDF_PATH 覆盖。
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJ="$DIR/.."

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "找不到 ESP-IDF：$IDF_PATH（见 lvgl_st77922_demo/README.md 的安装说明）" >&2
    exit 1
fi

# 让 idf_tools 走国内镜像（首次安装时有用，日常编译无害）
export IDF_GITHUB_ASSETS="${IDF_GITHUB_ASSETS:-dl.espressif.cn/github_assets}"

# export.sh 需要被 source 到当前 shell
. "$IDF_PATH/export.sh" >/dev/null

cd "$PROJ"
exec idf.py build "$@"
