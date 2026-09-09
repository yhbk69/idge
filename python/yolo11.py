# ============================================================================
# yolo11.py - YOLO11 推理和后处理
# ============================================================================
#
# 功能：
#   1. 加载 RKNN/ONNX/PyTorch 模型
#   2. 对图像进行预处理（LetterBox 缩放）
#   3. 执行推理（NPU/CPU）
#   4. 后处理（NMS、坐标还原）
#   5. 绘制检测结果
#   6. COCO mAP 评估（可选）
#
# 支持的模型格式：
#   - .rknn: Rockchip NPU 模型（在 RK3588 上硬件加速）
#   - .onnx: ONNX 模型（在 CPU 上运行）
#   - .pt/.torchscript: PyTorch 模型（在 CPU 上运行）
#
# 后处理流程：
#   1. box_process(): 解码边界框（DFL + grid + stride）
#   2. filter_boxes(): 按置信度过滤
#   3. nms_boxes(): 非极大值抑制（消除重叠框）
#
# 使用方法：
#   # 在 RK3588 上运行 RKNN 模型
#   python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --img_folder ../model
#
#   # 在 PC 上运行 ONNX 模型
#   python3 yolo11.py --model_path yolo11n.onnx --img_folder ../model
#
# ============================================================================

import os
import cv2
import sys
import argparse

# ============================================================================
# 路径配置（用于导入 rknn_model_zoo 工具）
# ============================================================================
realpath = os.path.abspath(__file__)
_sep = os.path.sep
realpath = realpath.split(_sep)
sys.path.append(os.path.join(realpath[0]+_sep, *realpath[1:realpath.index('rknn_model_zoo')+1]))

from py_utils.coco_utils import COCO_test_helper
import numpy as np

# ============================================================================
# 检测参数
# ============================================================================
OBJ_THRESH = 0.25    # 置信度阈值（低于此值的检测框被丢弃）
NMS_THRESH = 0.45    # NMS 阈值（IoU 高于此值的重叠框被合并）
IMG_SIZE = (640, 640) # 模型输入尺寸（宽, 高）

# ============================================================================
# COCO 数据集类别（80 类）
# ============================================================================
# 注意：CLASSES 元组的索引直接对应模型输出的类别 ID
# 但 coco_id_list 是 COCO 官方的类别 ID（有间隔）
CLASSES = ("person", "bicycle", "car","motorbike ","aeroplane ","bus ","train","truck ","boat","traffic light",
           "fire hydrant","stop sign ","parking meter","bench","bird","cat","dog ","horse ","sheep","cow","elephant",
           "bear","zebra ","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite",
           "baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife ",
           "spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza ","donut","cake","chair","sofa",
           "pottedplant","bed","diningtable","toilet ","tvmonitor","laptop\t","mouse\t","remote ","keyboard ","cell phone","microwave ",
           "oven ","toaster","sink","refrigerator ","book","clock","vase","scissors ","teddy bear ","hair drier", "toothbrush ")

# COCO 官方类别 ID 列表（模型输出的类别索引需要映射到 COCO ID）
coco_id_list = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 28, 31, 32, 33, 34,
         35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
         64, 65, 67, 70, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 84, 85, 86, 87, 88, 89, 90]


# ============================================================================
# filter_boxes - 按置信度过滤检测框
# ============================================================================
# 参数：
#   - boxes: 边界框 [N, 4] (x1, y1, x2, y2)
#   - box_confidences: 置信度 [N, 1]
#   - box_class_probs: 类别概率 [N, 80]
#
# 返回：
#   - boxes: 过滤后的边界框
#   - classes: 类别索引
#   - scores: 最终得分（置信度 × 类别概率）
#
# 算法：
#   1. 计算每个框的最大类别概率
#   2. 计算最终得分 = 置信度 × 类别概率
#   3. 过滤得分 < OBJ_THRESH 的框
# ============================================================================
def filter_boxes(boxes, box_confidences, box_class_probs):
    box_confidences = box_confidences.reshape(-1)
    candidate, class_num = box_class_probs.shape

    class_max_score = np.max(box_class_probs, axis=-1)   # 每个框的最大类别概率
    classes = np.argmax(box_class_probs, axis=-1)         # 每个框的类别索引

    # 过滤：得分 = 置信度 × 类别概率 ≥ 阈值
    _class_pos = np.where(class_max_score* box_confidences >= OBJ_THRESH)
    scores = (class_max_score* box_confidences)[_class_pos]

    boxes = boxes[_class_pos]
    classes = classes[_class_pos]

    return boxes, classes, scores


# ============================================================================
# nms_boxes - 非极大值抑制（NMS）
# ============================================================================
# 参数：
#   - boxes: 边界框 [N, 4]
#   - scores: 置信度分数 [N]
#
# 返回：
#   - keep: 保留的框索引
#
# 算法（贪心 NMS）：
#   1. 按分数从高到低排序
#   2. 选择分数最高的框，加入保留列表
#   3. 计算其与所有剩余框的 IoU（交并比）
#   4. 删除 IoU > NMS_THRESH 的框
#   5. 重复直到所有框都被处理
# ============================================================================
def nms_boxes(boxes, scores):
    x = boxes[:, 0]
    y = boxes[:, 1]
    w = boxes[:, 2] - boxes[:, 0]  # 宽度
    h = boxes[:, 3] - boxes[:, 1]  # 高度

    areas = w * h                   # 面积
    order = scores.argsort()[::-1]  # 按分数从高到低排序

    keep = []
    while order.size > 0:
        i = order[0]                # 选择分数最高的框
        keep.append(i)

        # 计算当前框与剩余框的交集
        xx1 = np.maximum(x[i], x[order[1:]])
        yy1 = np.maximum(y[i], y[order[1:]])
        xx2 = np.minimum(x[i] + w[i], x[order[1:]] + w[order[1:]])
        yy2 = np.minimum(y[i] + h[i], y[order[1:]] + h[order[1:]])

        w1 = np.maximum(0.0, xx2 - xx1 + 0.00001)  # 交集宽度
        h1 = np.maximum(0.0, yy2 - yy1 + 0.00001)  # 交集高度
        inter = w1 * h1                              # 交集面积

        # IoU = 交集 / (当前框面积 + 其他框面积 - 交集)
        ovr = inter / (areas[i] + areas[order[1:]] - inter)
        inds = np.where(ovr <= NMS_THRESH)[0]  # 保留 IoU ≤ 阈值的框
        order = order[inds + 1]

    keep = np.array(keep)
    return keep


# ============================================================================
# dfl - Distribution Focal Loss（DFL 解码）
# ============================================================================
# 用于 YOLOv8/v11 的边界框回归：
#   - 不直接预测坐标，而是预测分布
#   - 通过 softmax 计算期望值作为最终坐标
#
# 参数：
#   - position: [1, 64, H, W]（4 个坐标 × 16 个 bin）
#
# 返回：
#   - y: [1, 4, H, W]（解码后的坐标）
# ============================================================================
def dfl(position):
    import torch
    x = torch.tensor(position)
    n, c, h, w = x.shape
    p_num = 4                        # 4 个坐标（x1, y1, x2, y2）
    mc = c // p_num                  # 每个坐标的 bin 数量（16）
    y = x.reshape(n, p_num, mc, h, w)
    y = y.softmax(2)                 # 对每个坐标的 bin 做 softmax
    acc_metrix = torch.tensor(range(mc)).float().reshape(1, 1, mc, 1, 1)
    y = (y * acc_metrix).sum(2)      # 加权求和（计算期望值）
    return y.numpy()


# ============================================================================
# box_process - 边界框解码
# ============================================================================
# 将模型输出的原始边界框解码为实际坐标（像素值）
#
# 步骤：
#   1. 生成网格坐标（grid）
#   2. DFL 解码
#   3. 计算实际坐标 = (grid + 0.5 - offset) × stride
#
# 参数：
#   - position: [1, 64, H, W]（模型输出的边界框）
#
# 返回：
#   - xyxy: [1, 4, H×W]（解码后的边界框，x1/y1/x2/y2 格式）
# ============================================================================
def box_process(position):
    grid_h, grid_w = position.shape[2:4]

    # 生成网格坐标
    col, row = np.meshgrid(np.arange(0, grid_w), np.arange(0, grid_h))
    col = col.reshape(1, 1, grid_h, grid_w)
    row = row.reshape(1, 1, grid_h, grid_w)
    grid = np.concatenate((col, row), axis=1)

    # 计算 stride（每个网格单元对应的实际像素数）
    stride = np.array([IMG_SIZE[1]//grid_h, IMG_SIZE[0]//grid_w]).reshape(1, 2, 1, 1)

    # DFL 解码
    position = dfl(position)

    # 计算边界框坐标
    box_xy = grid + 0.5 - position[:, 0:2, :, :]   # 左上角
    box_xy2 = grid + 0.5 + position[:, 2:4, :, :]  # 右下角

    # 转换为像素坐标
    xyxy = np.concatenate((box_xy * stride, box_xy2 * stride), axis=1)

    return xyxy


# ============================================================================
# post_process - 后处理主函数
# ============================================================================
# 将模型原始输出转换为最终检测结果
#
# 输入：
#   - input_data: 模型输出列表（3 个分支 × 2 个输出 = 6 个张量）
#     - 每个分支：[边界框, 类别概率]
#
# 输出：
#   - boxes: 边界框 [N, 4]
#   - classes: 类别索引 [N]
#   - scores: 置信度 [N]
#
# 处理流程：
#   1. 解码 3 个分支的边界框（box_process）
#   2. 展平所有分支的结果
#   3. 按置信度过滤（filter_boxes）
#   4. NMS 去重（nms_boxes）
# ============================================================================
def post_process(input_data):  
    boxes, scores, classes_conf = [], [], []
    defualt_branch = 3                # YOLOv8/v11 有 3 个检测分支（P3/P4/P5）
    pair_per_branch = len(input_data) // defualt_branch  # 每个分支的输出数量（2：边界框 + 类别）

    for i in range(defualt_branch):
        boxes.append(box_process(input_data[pair_per_branch * i]))           # 解码边界框
        classes_conf.append(input_data[pair_per_branch * i + 1])             # 类别概率
        scores.append(np.ones_like(input_data[pair_per_branch * i + 1][:, :1, :, :], dtype=np.float32))  # 占位分数

    # 展平：将 [1, C, H, W] 转换为 [H*W, C]
    def sp_flatten(_in):
        ch = _in.shape[1]
        _in = _in.transpose(0, 2, 3, 1)
        return _in.reshape(-1, ch)

    boxes = [sp_flatten(_v) for _v in boxes]
    classes_conf = [sp_flatten(_v) for _v in classes_conf]
    scores = [sp_flatten(_v) for _v in scores]

    # 合并 3 个分支的结果
    boxes = np.concatenate(boxes)
    classes_conf = np.concatenate(classes_conf)
    scores = np.concatenate(scores)

    # 按置信度过滤
    boxes, classes, scores = filter_boxes(boxes, scores, classes_conf)

    # NMS 去重（按类别分别处理）
    nboxes, nclasses, nscores = [], [], []
    for c in set(classes):
        inds = np.where(classes == c)
        b = boxes[inds]
        c = classes[inds]
        s = scores[inds]
        keep = nms_boxes(b, s)

        if len(keep) != 0:
            nboxes.append(b[keep])
            nclasses.append(c[keep])
            nscores.append(s[keep])

    if not nclasses and not nscores:
        return None, None, None

    boxes = np.concatenate(nboxes)
    classes = np.concatenate(nclasses)
    scores = np.concatenate(nscores)

    return boxes, classes, scores


# ============================================================================
# draw - 绘制检测结果
# ============================================================================
# 在图像上绘制边界框和类别标签
# ============================================================================
def draw(image, boxes, scores, classes):
    for box, score, cl in zip(boxes, scores, classes):
        top, left, right, bottom = [int(_b) for _b in box]
        print("%s @ (%d %d %d %d) %.3f" % (CLASSES[cl], top, left, right, bottom, score))
        cv2.rectangle(image, (top, left), (right, bottom), (255, 0, 0), 2)
        cv2.putText(image, '{0} {1:.2f}'.format(CLASSES[cl], score),
                    (top, left - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2)


# ============================================================================
# setup_model - 加载模型
# ============================================================================
# 根据文件扩展名自动选择模型格式：
#   - .pt/.torchscript: PyTorch 模型
#   - .rknn: Rockchip RKNN 模型
#   - .onnx: ONNX 模型
# ============================================================================
def setup_model(args):
    model_path = args.model_path
    if model_path.endswith('.pt') or model_path.endswith('.torchscript'):
        platform = 'pytorch'
        from py_utils.pytorch_executor import Torch_model_container
        model = Torch_model_container(args.model_path)
    elif model_path.endswith('.rknn'):
        platform = 'rknn'
        from py_utils.rknn_executor import RKNN_model_container
        model = RKNN_model_container(args.model_path, args.target, args.device_id)
    elif model_path.endswith('onnx'):
        platform = 'onnx'
        from py_utils.onnx_executor import ONNX_model_container
        model = ONNX_model_container(args.model_path)
    else:
        assert False, "{} is not rknn/pytorch/onnx model".format(model_path)
    print('Model-{} is {} model, starting val'.format(model_path, platform))
    return model, platform


# ============================================================================
# img_check - 检查文件是否为图片格式
# ============================================================================
def img_check(path):
    img_type = ['.jpg', '.jpeg', '.png', '.bmp']
    for _type in img_type:
        if path.endswith(_type) or path.endswith(_type.upper()):
            return True
    return False


# ============================================================================
# 主程序：执行推理和评估
# ============================================================================
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Process some integers.')

    # 基本参数
    parser.add_argument('--model_path', type=str, required=True, help='model path, could be .pt or .rknn file')
    parser.add_argument('--target', type=str, default='rk3566', help='target RKNPU platform')
    parser.add_argument('--device_id', type=str, default=None, help='device id')

    parser.add_argument('--img_show', action='store_true', default=False, help='draw the result and show')
    parser.add_argument('--img_save', action='store_true', default=False, help='save the result')

    # 数据参数
    parser.add_argument('--anno_json', type=str, default='../../../datasets/COCO/annotations/instances_val2017.json', help='coco annotation path')
    parser.add_argument('--img_folder', type=str, default='../model', help='img folder path')
    parser.add_argument('--coco_map_test', action='store_true', help='enable coco map test')

    args = parser.parse_args()

    # 初始化模型
    model, platform = setup_model(args)

    # 收集图片文件
    file_list = sorted(os.listdir(args.img_folder))
    img_list = []
    for path in file_list:
        if img_check(path):
            img_list.append(path)
    co_helper = COCO_test_helper(enable_letter_box=True)

    # 推理循环
    for i in range(len(img_list)):
        print('infer {}/{}'.format(i+1, len(img_list)), end='\r')

        img_name = img_list[i]
        img_path = os.path.join(args.img_folder, img_name)
        if not os.path.exists(img_path):
            print("{} is not found", img_name)
            continue

        img_src = cv2.imread(img_path)
        if img_src is None:
            continue

        # LetterBox 缩放（保持宽高比，灰色背景填充）
        # 注意：pad_color 用 (0,0,0) 而非 (114,114,114)，因为 RGA 硬件初始化为黑色
        pad_color = (0, 0, 0)
        img = co_helper.letter_box(im=img_src.copy(), new_shape=(IMG_SIZE[1], IMG_SIZE[0]), pad_color=(0, 0, 0))
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

        # 预处理（非 RKNN 模型需要手动归一化）
        if platform in ['pytorch', 'onnx']:
            input_data = img.transpose((2, 0, 1))                    # HWC → CHW
            input_data = input_data.reshape(1, *input_data.shape).astype(np.float32)
            input_data = input_data / 255.                           # 归一化到 [0, 1]
        else:
            input_data = img  # RKNN 模型在 config() 中已配置预处理

        # 执行推理
        outputs = model.run([input_data])
        boxes, classes, scores = post_process(outputs)

        # 绘制结果
        if args.img_show or args.img_save:
            print('\n\nIMG: {}'.format(img_name))
            img_p = img_src.copy()
            if boxes is not None:
                draw(img_p, co_helper.get_real_box(boxes), scores, classes)

            if args.img_save:
                if not os.path.exists('./result'):
                    os.mkdir('./result')
                result_path = os.path.join('./result', img_name)
                cv2.imwrite(result_path, img_p)
                print('Detection result save to {}'.format(result_path))

            if args.img_show:
                cv2.imshow("full post process result", img_p)
                cv2.waitKeyEx(0)

        # COCO mAP 评估（可选）
        if args.coco_map_test is True:
            if boxes is not None:
                for i in range(boxes.shape[0]):
                    co_helper.add_single_record(image_id=int(img_name.split('.')[0]),
                                                category_id=coco_id_list[int(classes[i])],
                                                bbox=boxes[i],
                                                score=round(scores[i], 5).item()
                                                )

    # 计算 mAP
    if args.coco_map_test is True:
        pred_json = args.model_path.split('.')[-2] + '_{}'.format(platform) + '.json'
        pred_json = pred_json.split('/')[-1]
        pred_json = os.path.join('./', pred_json)
        co_helper.export_to_json(pred_json)

        from py_utils.coco_utils import coco_eval_with_json
        coco_eval_with_json(args.anno_json, pred_json)

    # 释放模型资源
    model.release()
