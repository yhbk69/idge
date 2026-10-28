#!/usr/bin/env bash
# ============================================================================
# pt2rknn.sh —— 从 .pt 权重一步做到 model/library/<库id>/model.rknn
#
# 用法: ./python/pt2rknn.sh <model.pt> <库id> [platform=rk3588] [dtype=i8|u8|fp] [校准清单]
#   校准清单默认为 model/train/calib/dataset_test.txt（仓库里那份是占位图，
#   精度敏感的任务务必换成含目标物体的现场帧清单）。
#
# 只能跑在 x86_64 Linux/WSL 上：rknn-toolkit2 只提供 x86_64 wheel，
# 板子（aarch64）装不上。转换产物与目标平台绑定，rk3588 转的不能跑在 rk3568。
#
# 本脚本刻意只做转换，不写 labels.txt、不改 config.json —— 类别表是运行时的
# 事实源（行序 = cls_id），必须由入库的人核对后钉死，不能让脚本猜。
# ============================================================================
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "用法: $0 <model.pt> <库id> [platform=rk3588] [dtype=i8|u8|fp] [校准清单]" >&2
    exit 1
fi

PT="$1"
LIB_ID="$2"
PLATFORM="${3:-rk3588}"
DTYPE="${4:-i8}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CALIB="${5:-$REPO/model/train/calib/dataset_test.txt}"

# ---- 前置检查 -------------------------------------------------------------
# 库目录名即模型 id（config 里就按它引用），字符集与 ModelRegistry 一致
if ! printf '%s' "$LIB_ID" | grep -qE '^[a-z0-9][a-z0-9_-]*$'; then
    echo "错误: 库id \"$LIB_ID\" 不合法，只允许小写字母/数字/-/_，且以字母或数字开头。" >&2
    exit 1
fi

case "$DTYPE" in
    i8|u8|fp) ;;
    *) echo "错误: dtype 只能是 i8 / u8 / fp，传入的是 \"$DTYPE\"。" >&2; exit 1 ;;
esac

if [ "$(uname -m)" != "x86_64" ]; then
    echo "错误: rknn-toolkit2 只有 x86_64 wheel，当前架构 $(uname -m) 装不上。" >&2
    echo "      请在一台 x86_64 Linux/WSL 机器上 clone 本仓库后执行本脚本，" >&2
    echo "      产物 model/library/$LIB_ID/model.rknn 再拷回板子。" >&2
    exit 1
fi

[ -f "$PT" ] || { echo "错误: 找不到权重文件 $PT" >&2; exit 1; }
PT="$(cd "$(dirname "$PT")" && pwd)/$(basename "$PT")"
ONNX="${PT%.pt}.onnx"
OUT_DIR="$REPO/model/library/$LIB_ID"
OUT="$OUT_DIR/model.rknn"

if [ ! -d "$OUT_DIR" ]; then
    echo "提示: $OUT_DIR 不存在，先建目录（labels.txt 需要你自己核对后写入）。"
    mkdir -p "$OUT_DIR"
fi

echo "==> 源权重   : $PT"
echo "==> 目标平台 : $PLATFORM"
echo "==> 量化     : $DTYPE"
echo "==> 校准清单 : $CALIB"
echo "==> 产物     : $OUT"
echo

# ---- 1) .pt → onnx --------------------------------------------------------
echo "==> [1/2] ultralytics 导出 ONNX (imgsz=640, simplify, opset=12)"
python3 - "$PT" <<'PY'
import sys
from ultralytics import YOLO
# imgsz 必须 640：C++ 端 letterbox 与 config 的 detect 段都是 640×640 口径
YOLO(sys.argv[1]).export(format="onnx", imgsz=640, simplify=True, opset=12, dynamic=False)
PY

[ -f "$ONNX" ] || { echo "错误: 导出后没找到 $ONNX（权重不是 Ultralytics 格式？）" >&2; exit 1; }

# ---- 2) onnx → rknn -------------------------------------------------------
# convert.py 按相对路径读校准清单，所以 cd 进 python/ 再跑，清单给绝对路径
echo "==> [2/2] convert.py 转 RKNN"
( cd "$REPO/python" && DATASET="$CALIB" python3 convert.py "$ONNX" "$PLATFORM" "$DTYPE" "$OUT" )

# ---- 收尾 -----------------------------------------------------------------
echo
echo "已生成 $OUT"
echo
echo "入库还差三步（本脚本不代劳，因为类别表必须人工核对）："
echo "  1) 确认 $OUT_DIR/labels.txt 存在，每行一个类名、行序 = 模型输出的 cls_id。"
echo "     与训练时的 data.yaml names 对齐；写错会让画框和报警串类别。"
echo "  2) config.json 里加模型参数（报警类别是**模型的参数**，不是槽位/通道的）："
echo "       \"video\": { \"models\": { \"$LIB_ID\": { \"alarm_class\": \"<该模型的报警类别>\", \"note\": \"…\" } } }"
echo "     设置页「模型参数」表里改也等效。"
echo "  3) 把库 id 追加到需要它的通道（数组顺序 = 级联槽位顺序 = 画面 M1/M2 前缀）："
echo "       \"video\": { \"channels\": [ { \"channel\": 0, \"models\": [..., \"$LIB_ID\"] } ] }"
echo "     然后重启程序（改模型列表不做热更新，见 README §八 第 11 条）。"
echo
echo "NPU 预算：RK3588 只有 3 个核，任务数 = Σ各通道模型数；超过 6 个启动日志会打软告警。"
