# ============================================================================
# convert.py - ONNX 模型转换为 RKNN 格式
# ============================================================================
#
# 功能：
#   将 YOLO11/YOLOv8 的 ONNX 模型转换为 Rockchip RKNN 格式，
#   用于在 RK3588 NPU 上进行硬件加速推理。
#
# 转换流程：
#   1. rknn.config(): 配置预处理参数（均值、标准差、目标平台）
#   2. rknn.load_onnx(): 加载 ONNX 模型
#   3. rknn.build(): 构建 RKNN 模型（可选量化）
#   4. rknn.export_rknn(): 导出 .rknn 文件
#
# 量化说明：
#   - INT8 量化（i8）：模型大小减小 4 倍，推理速度提升，精度略有损失
#   - FP16 保留（fp）：不量化，保持原始精度，模型大小不变
#   - 量化需要校准数据集（coco_subset_20.txt 包含 20 张 COCO 图片路径）
#
# 平台支持：
#   - RK3562: 入门级 NPU（0.5 TOPS）
#   - RK3566/RK3568: 中端 NPU（1 TOPS）
#   - RK3576: 中高端 NPU（6 TOPS）
#   - RK3588: 旗舰 NPU（18 TOPS，3 核心）
#
# 使用方法：
#   python3 convert.py <onnx_model_path> <platform> [dtype] [output_rknn_path]
#
#   示例：
#   python3 convert.py yolo11n.onnx rk3588 i8 ../model/yolo11n.rknn
#   python3 convert.py yolo11n.onnx rk3588 fp ../model/yolo11n_fp16.rknn
#
# 注意事项：
#   - 转换过程在 x86 PC 上运行（不需要 RK3588 开发板）
#   - 但生成的 .rknn 文件只能在对应的 Rockchip NPU 上运行
#   - 量化需要校准数据集，否则精度会严重下降
#
# ============================================================================

import sys
from rknn.api import RKNN

# ============================================================================
# 默认配置
# ============================================================================
DATASET_PATH = '../../../datasets/COCO/coco_subset_20.txt'  # 量化校准数据集（20 张 COCO 图片）
DEFAULT_RKNN_PATH = '../model/yolo11.rknn'                  # 默认输出路径
DEFAULT_QUANT = True                                         # 默认启用 INT8 量化

# ============================================================================
# parse_arg - 解析命令行参数
# ============================================================================
def parse_arg():
    # 参数检查：至少需要 onnx_model_path 和 platform
    if len(sys.argv) < 3:
        print("Usage: python3 {} onnx_model_path [platform] [dtype(optional)] [output_rknn_path(optional)]".format(sys.argv[0]))
        print("       platform choose from [rk3562,rk3566,rk3568,rk3588,rk3576]")
        print("       dtype choose from [i8, fp] for [rk3562,rk3566,rk3568,rk3588,rk3576]")
        exit(1)

    model_path = sys.argv[1]   # ONNX 模型路径（如 yolo11n.onnx）
    platform = sys.argv[2]     # 目标平台（如 rk3588）

    # 默认启用 INT8 量化
    do_quant = DEFAULT_QUANT
    if len(sys.argv) > 3:
        model_type = sys.argv[3]
        if model_type not in ['i8', 'u8', 'fp']:
            print("ERROR: Invalid model type: {}".format(model_type))
            exit(1)
        elif model_type in ['i8', 'u8']:
            do_quant = True    # INT8 或 UINT8 量化
        else:
            do_quant = False   # FP16 保留

    # 输出路径（默认 ../model/yolo11.rknn）
    if len(sys.argv) > 4:
        output_path = sys.argv[4]
    else:
        output_path = DEFAULT_RKNN_PATH

    return model_path, platform, do_quant, output_path

# ============================================================================
# 主程序：执行模型转换
# ============================================================================
if __name__ == '__main__':
    model_path, platform, do_quant, output_path = parse_arg()

    # 1. 创建 RKNN 对象
    rknn = RKNN(verbose=False)

    # 2. 配置预处理参数
    # mean_values: 均值减法（[0,0,0] 表示不减均值）
    # std_values: 标准差除法（[255,255,255] 表示归一化到 [0,1]）
    # target_platform: 目标 NPU 平台（决定指令集和优化策略）
    print('--> Config model')
    rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]], target_platform=platform)
    print('done')

    # 3. 加载 ONNX 模型
    print('--> Loading model')
    ret = rknn.load_onnx(model=model_path)
    if ret != 0:
        print('Load model failed!')
        exit(ret)
    print('done')

    # 4. 构建 RKNN 模型（量化 + 编译）
    # do_quantization: 是否进行 INT8 量化
    # dataset: 量化校准数据集（包含图片路径的 txt 文件）
    print('--> Building model')
    ret = rknn.build(do_quantization=do_quant, dataset=DATASET_PATH)
    if ret != 0:
        print('Build model failed!')
        exit(ret)
    print('done')

    # 5. 导出 .rknn 模型文件
    print('--> Export rknn model')
    ret = rknn.export_rknn(output_path)
    if ret != 0:
        print('Export rknn model failed!')
        exit(ret)
    print('done')

    # 6. 释放 RKNN 资源
    rknn.release()
