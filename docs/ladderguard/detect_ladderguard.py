"""
登高守护（ladderguard）：有人在梯子上时，梯下必须有人扶梯/看护，否则判定为违规并告警。

════════════════════════════════════════════════════════════════════════════
一、整体思路
════════════════════════════════════════════════════════════════════════════
不新增模型、不改训练：用**已有的两个检测模型**做跨框几何匹配，再由一个时间状态机
做防抖，最后输出一句明确的「违规 / 不违规」终判。

  双模型组合（同一帧各推理一次，互不干扰）：
    梯子模型  runs/detect/ladder_yolo11n/weights/best.pt  1 类 Ladder
              -> 梯子框，是本功能的「触发源」；画面里没梯子，整套守护逻辑不激活。
    人员模型  runs/detect/ppe2_yolo11n/weights/best.pt     5 类，其中 people 是人员类
              -> 人框，用来区分「梯上攀爬者」和「梯下扶梯人」。

  为什么人模型用 PPE 权重：ppe2_yolo11n 的 people 类在工地/地铁站全场景画面上
  实测能同时检出梯上的人（conf 0.75）与梯下弯腰扶梯的人（conf 0.63），
  在 scripts/media 全 8 张上与 COCO person 判定结果完全一致，且属于本项目自有域，
  后续可随 PPE 训练一起迭代。两个备选权重的实测结论（供换权重时参考）：
    - ppe2_yolo11n-2/weights/best.pt：漏掉梯上攀爬者 -> 功能「静默失效」，不要用。
    - laddersafe_yolo11n/weights/best.pt：worker_* 类在 1080p 全场景漏掉弯腰扶梯的人
      -> 会把合规画面误判成违规，不要用。
    - scripts/yolo11n.pt（COCO）：效果与 ppe2_yolo11n 相当，作为域外兜底备选。
  三者都只见过 640×640 裁剪图，搬到整幅工地画面一定存在域偏移，生产环境建议
  用现场画面（含弯腰、攀爬姿态、正负样本）微调 person 模型后再上线。

  imgsz 默认 1280：实测 640 会把梯上的人整个漏掉（等于告警失效），
  1080p 全场景必须 1280；小图 / 裁剪图域可降回 640 提速。

════════════════════════════════════════════════════════════════════════════
二、判定规则（几何）
════════════════════════════════════════════════════════════════════════════
所有阈值都取「梯子框自身宽高 W/H 的比例」，不写死像素，因此换分辨率、换机位、
梯子远近都无需重新调参。图像坐标系 y 轴向下，框格式统一为 xyxy = (x1,y1,x2,y2)。

  攀爬者 = 人框水平中心落在梯框 ±CLIMBER_DX*W 内，
           且人框底部（脚）高于梯底 ≥ CLIMBER_DY*H（脚离地了）
  扶梯人 = 非攀爬者，水平距梯框 ≤ SPOTTER_DX*W，
           且脚部落在梯底基线 ±SPOTTER_DY*H 内（站在地面、贴着梯子）

  只看「脚部 y 坐标」而不要求人与梯子的 IoU：攀爬者的人体框与梯子框高度重叠是常态，
  但被梯梁遮挡时 IoU 会掉得很低；脚离地/贴地这个几何特征反而更稳定。

  梯子框先经 merge_ladders() 聚合：模型常同时给出「整梯框」+「只框住上半段的子框」，
  子框底边不是真梯底，拿它单独判定会凭空多出告警。聚合取并集框才拿到真实梯底。

════════════════════════════════════════════════════════════════════════════
三、状态机（仅视频 / 摄像头模式，按每把梯子一个实例，用 IoU 跨帧跟踪梯子身份）
════════════════════════════════════════════════════════════════════════════
  IDLE     --检测到攀爬者-->                  CLIMBING
  CLIMBING --无扶梯人持续 >= grace 秒-->      ALARM（红框 + NO SPOTTER + 抓拍一次）
  ALARM    --扶梯人连续在场 >= clear 秒-->    CLIMBING
  任意状态 --攀爬者下梯-->                    IDLE
  梯子连续丢失 >= LOST_SEC -->                状态机销毁

  grace 宽限：给「工人刚上梯、扶梯人正走过来」的正常时序留缓冲，
              也吸收单帧漏检，避免告警随 NMS 抖动反复闪烁。
  clear 门槛：要求扶梯人连续在场，防遮挡漏检一帧就误消警。

════════════════════════════════════════════════════════════════════════════
四、终判输出（违规 = 有人在梯子上 且 梯下无人看护）
════════════════════════════════════════════════════════════════════════════
  四种情形：无梯子=不涉及；有人登高+无人看护=违规；有人登高+有人看护=不违规；
            有梯子但无人登高=不违规（此时看护要求根本不成立）。
  图片模式：逐张打印「违规判定: 违规/不违规 —— 原因」，结尾打印
            「最终结果: ...（违规 N 张）」。
  视频模式：逐帧在画面左上角画红/绿判定横幅，结尾打印
            「最终结果: ...（违规帧 N/总帧）」；只有状态机进入 ALARM 才算违规帧，
            2s 宽限内的短时漏检不算。

════════════════════════════════════════════════════════════════════════════
五、用法（在仓库根目录 D:/code/ultralytics 下执行）
════════════════════════════════════════════════════════════════════════════
  python scripts/ladderguard/detect_ladderguard.py --source scripts/media --no-show
  python scripts/ladderguard/detect_ladderguard.py --source scripts/media/ladder3.png --no-show
  python scripts/ladderguard/detect_ladderguard.py --source 0                     # 摄像头，q 退出
  python scripts/ladderguard/detect_ladderguard.py --source video.mp4 --conf 0.4
  python scripts/ladderguard/detect_ladderguard.py --source 0 \
      --ladder-model runs/detect/ladder_yolo11m/weights/best.pt \
      --person-model runs/detect/ppe2_yolo11n-2/weights/best.pt

  注意：Windows 下 `python` 常被 PATH 上其它环境劫持，建议显式调用：
  C:/Users/dltt6/.conda/envs/ultralytics/python.exe scripts/ladderguard/detect_ladderguard.py ...

注意 / 已知边界：
  - 图片模式没有时间轴，状态机不生效，退化为单帧几何判定（有攀爬者无扶梯人 -> 直接画红框）。
  - 告警抓拍落在 <out>/alarm/ 下，每次新进入 ALARM 保存一张。
  - 结果图 / 视频落在 <out>/，图片加 _pred 后缀，视频为 output.mp4。
  - 画面里只画 ASCII：cv2.putText 不带字体回退，中文会画成方框，中文判定走控制台。
  - 单目视觉的物理上限：扶梯人被完全遮挡时无法检出，靠 clear 门槛 + 抓拍人工复核兜底。
  - ladder 模型只见过 640×640 梯子裁剪图，域外图（人像、街景）可能误检梯子；
    只要没有「脚离地的攀爬者」，误检梯子不会造成误报。

自检（不需要完整权重也能秒级验通路）：
  python scripts/ladderguard/detect_ladderguard.py --source scripts/media/bus.png --no-show
  期望「无梯子，不涉及登高守护」；若误检出梯子但没有攀爬者，仍应判不违规。
"""

import argparse
import os

# OpenMP 重复加载检查发生在动态库加载那一刻，必须排在 import torch / ultralytics 之前，
# 否则 conda 环境里同时存在多份 libomp 时会直接 abort（常见于 Windows + PyTorch）。
os.environ.setdefault("KMP_DUPLICATE_LIB_OK", "TRUE")

import threading   # 摄像头采集线程（实时流专用）
import time        # time.perf_counter 测耗时；time.monotonic 驱动状态机（不受系统改时影响）
from collections import Counter  # 逐帧统计各类框数量，用于 HUD 和终判
from pathlib import Path         # 路径拼接 / 权重存在性检查 / 扩展名判断

import cv2         # 读图、读视频、画框与文字
import torch       # 仅用于判断 CUDA 是否可用，自动选择 device
from ultralytics import YOLO


# ══════════════════════════════════════════════════════════════════════════
# 几何阈值 / 时间常量
# ── 全部按梯子框自身 W/H 取比例，因此与图像分辨率、机位距离无关。
# ── 修改方式：直接改这里的代码常量，或用命令行覆盖 grace / clear / imgsz / conf。
# ── 调参方向见「登高守护实现方案.md」第 8 节。
# ══════════════════════════════════════════════════════════════════════════
CLIMBER_DY = 0.20  # 攀爬者：脚部高于梯底 ≥ 0.2H 才算「离地」。
                   # 调小 -> 脚刚离踏板就算攀爬（更灵敏，但地面的人易被误判）；
                   # 调大 -> 只认爬得较高的人（更保守，矮梯作业可能漏判）。
CLIMBER_DX = 0.20  # 攀爬者：人框水平中心允许落在梯框左右各外扩 0.2W。
                   # 留出外扩是因为人在梯上作业时会侧身、身体中心偏出梯梁。
SPOTTER_DY = 0.15  # 扶梯人：脚部与梯底基线的容许偏差 ±0.15H，用来界定「站在地面」。
                   # 仰角大的机位地面上的人在像素上会明显偏上，此时要放宽到 0.25H 左右。
SPOTTER_DX = 0.80  # 扶梯人：人框中心到梯框最近边的水平距离上限（0.8W，约一把梯宽）。
                   # 调大能容纳站得远的看护人，但会把路人误算成扶梯人（漏报风险）。
GRACE_SEC = 2.0    # CLIMBING 状态下「无扶梯人」持续多久才升到 ALARM（秒）。
                   # 现场扶梯人到位慢就调大；要求更严苛就调小，但太小会闪报。
CLEAR_SEC = 1.0    # ALARM 状态下「扶梯人连续在场」多久才降回 CLIMBING（秒）。
                   # 扶梯人频繁被遮挡导致误消警时调大。
LOST_SEC = 1.0     # 梯子连续丢失多久销毁对应的状态机（秒）。
                   # 容忍约 1 秒的检测抖动，避免梯子一漏检就丢掉计时重新宽限。
IOU_MATCH = 0.3    # 跨帧把「本帧梯子框」匹配到「旧梯子状态机」的 IoU 下限。
                   # 梯子框抖动剧烈、频繁新建状态机时调小（但过小会串梯）。

# ── 绘制配色（OpenCV 使用 BGR 顺序，不是 RGB） ─────────────────────────────
COLOR_LADDER = (0, 200, 0)      # 绿：梯子框，也是「不违规」终判横幅颜色
COLOR_CLIMBER = (0, 180, 255)   # 橙：攀爬者（合规状态，即梯下有人看护）
COLOR_SPOTTER = (255, 160, 0)   # 蓝：扶梯人 / 看护人
COLOR_ALARM = (0, 0, 255)       # 红：违规告警框、NO SPOTTER 标签、违规终判横幅

# ── 输入源类型判定用的扩展名表（大小写不敏感，代码里统一 .lower() 后比较） ────
IMAGE_EXT = (".jpg", ".jpeg", ".png", ".bmp", ".webp")
VIDEO_EXT = (".mp4", ".avi", ".mov", ".mkv")
WINDOW = "ladderguard"          # cv2.imshow 窗口名，图片/视频模式共用


# ══════════════════════════════════════════════════════════════════════════
# 框工具：类别识别、结果抽取、重复梯框聚合、IoU
# ══════════════════════════════════════════════════════════════════════════
def pick_classes(names):
    """从模型类别表里自动挑出「梯子类 id 集合」与「人员类 id 集合」。

    Args:
        names: ultralytics 的 model.names，形如 {0: 'Ladder', 1: 'people', ...}。

    Returns:
        (ladders, persons)：两个 set[int]。只取类别 id，具体框在 extract() 里过滤。

    设计动机：不同权重的类名不一样，按小写子串匹配就不用每换一个权重改一次代码：
      ladder 模型 -> 'Ladder'
      laddersafe  -> 'worker_with_helmet' / 'worker_without_helmet'
      ppe2        -> 'people'（另有 un_helmet / helmet / vest / workWear，均不算人员）
      COCO        -> 'person'
    注意 'workWear' 不含 'worker'，因此 ppe2 只有 people 会被选为人员类；
    ladder 模型本身不会被要求提供人员类，反之亦然，两个分支互不干扰。
    """
    ladders, persons = set(), set()
    for cid, name in names.items():
        low = str(name).lower()
        if "ladder" in low:
            ladders.add(cid)
        elif any(k in low for k in ("worker", "person", "people", "pedestrian")):
            persons.add(cid)
    return ladders, persons


def extract(result, class_ids):
    """把一次 YOLO 推理结果中指定类别的框，转成 [(xyxy_tuple, conf_float), ...]。

    Args:
        result: ultralytics Results 对象（取 predict(...)[0]，已含一帧的全部框）。
        class_ids: pick_classes() 返回的类别 id 集合。

    Returns:
        坐标已转成原图像素坐标的 float 四元组 (x1, y1, x2, y2)，配套该框置信度。
        无框 / 无目标类别时返回空列表（调用方无需判 None）。

    说明：ultralytics 的 boxes.xyxy 是 letterbox 还原后的原图坐标，
    因此本文件后续所有几何阈值都直接在同一坐标系里比较，不用再做缩放。
    """
    out = []
    if result.boxes is None or len(result.boxes) == 0:
        return out
    xyxy = result.boxes.xyxy.cpu()      # (N, 4) tensor，可能落在 GPU 上，先搬回 CPU
    confs = result.boxes.conf.cpu()     # (N,) 置信度
    clss = result.boxes.cls.cpu().int().tolist()  # (N,) 类别 id -> python int 列表
    for i, cid in enumerate(clss):
        if cid in class_ids:
            out.append((tuple(float(v) for v in xyxy[i]), float(confs[i])))
    return out


def merge_ladders(boxes, iou_thres=0.1, contain_thres=0.5):
    """把「同一把梯子」的多个重叠 / 包含框聚合成一个并集框，再交给判定逻辑。

    Args:
        boxes: extract() 得到的 [(xyxy, conf), ...]，本函数只处理梯子框。
        iou_thres: 两框 IoU 超过此值即视为同一把梯子（阈值取得很低，宁可合并）。
        contain_thres: 「交集 / 较小框面积」超过此值也算同一把梯子。

    Returns:
        聚合后的 [(并集 xyxy, 簇内最大 conf), ...]。

    为什么必须聚合（实测踩过的坑）：梯子模型常同时输出「整梯框」和「只框住
    上半段的子框」。子框底边远高于真梯底，而扶梯人的判定基准正是梯底 y 坐标，
    于是扶梯人永远落在容差带之外 -> 凭空多出一条违规告警。
    单纯按 IoU 去重不够：子框被父框包含时 IoU 可能很低（细长框尤其明显），
    所以补一层「交集 / 较小框面积」的包含判断。

    算法：按 conf 降序放列表，反复扫描任意两框，命中条件就取并集、conf 取大者、
    删掉被合并的那个，直到一轮扫描不再发生变化。并集的底边即真实梯底。
    """
    items = [[b, c] for b, c in sorted(boxes, key=lambda x: -x[1])]
    merged = True
    while merged and len(items) > 1:
        merged = False
        for i in range(len(items)):
            for j in range(i + 1, len(items)):
                a, b = items[i][0], items[j][0]
                # 交集矩形（xyxy 坐标系）
                ix1, iy1 = max(a[0], b[0]), max(a[1], b[1])
                ix2, iy2 = min(a[2], b[2]), min(a[3], b[3])
                inter = max(0.0, ix2 - ix1) * max(0.0, iy2 - iy1)
                if inter <= 0:
                    continue
                area_a = (a[2] - a[0]) * (a[3] - a[1])
                area_b = (b[2] - b[0]) * (b[3] - b[1])
                # 子框被父框包含时 IoU 会很低，用「交集 / 较小框面积」补一层包含判断
                if iou(a, b) >= iou_thres or inter / max(min(area_a, area_b), 1e-6) >= contain_thres:
                    # 并集框：左上取最小、右下取最大，保证底边是最低的那个（真梯底）
                    items[i][0] = (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))
                    items[i][1] = max(items[i][1], items[j][1])
                    items.pop(j)
                    merged = True
                    break
            if merged:
                break
    return [(b, c) for b, c in items]


def iou(a, b):
    """两个 xyxy 框的交并比；无交集返回 0.0。

    用途有二：merge_ladders() 判断重复梯框；GuardTracker.step() 跨帧匹配梯子身份。
    """
    x1, y1 = max(a[0], b[0]), max(a[1], b[1])
    x2, y2 = min(a[2], b[2]), min(a[3], b[3])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    if inter <= 0:
        return 0.0
    area_a = (a[2] - a[0]) * (a[3] - a[1])
    area_b = (b[2] - b[0]) * (b[3] - b[1])
    return inter / (area_a + area_b - inter)


# ══════════════════════════════════════════════════════════════════════════
# 几何判定：把一帧里的人分成「攀爬者」「扶梯人」「无关路人」
# ══════════════════════════════════════════════════════════════════════════
def classify_persons(ladder, persons):
    """针对某一把梯子，把全部人框分类成 (攀爬者列表, 扶梯人列表)。

    Args:
        ladder: 该梯子的 xyxy 框（应为 merge_ladders() 之后的并集框）。
        persons: 本帧人模型给出的全部 [(xyxy, conf), ...]。

    Returns:
        (climbers, spotters)，元素格式与入参 persons 相同；未参与任何判定的路人
        不出现在返回值里（图片模式会把他们画成灰色细框供人工核对）。

    顺序很关键：先判攀爬者，剩下的人才有资格判扶梯人。否则一个站在梯上的
    人若同时满足「脚贴地」容差，就会被自己当成扶梯人，违规永远报不出来。
    """
    lx1, ly1, lx2, ly2 = ladder
    w, h = lx2 - lx1, ly2 - ly1
    if w <= 1 or h <= 1:  # 退化框（宽或高不足 1 像素）直接放弃，避免除零/比例失真
        return [], []

    # ① 攀爬者：水平中心在梯框附近 + 脚明显高于梯底（离地）
    climbers, others = [], []
    for pbox, conf in persons:
        px1, py1, px2, py2 = pbox
        cx = (px1 + px2) / 2.0
        feet = py2                       # 人框下边缘近似为脚部位置
        in_x = (lx1 - CLIMBER_DX * w) <= cx <= (lx2 + CLIMBER_DX * w)
        off_ground = feet <= ly2 - CLIMBER_DY * h  # y 轴向下，脚比梯底高即坐标更小
        if in_x and off_ground:
            climbers.append((pbox, conf))
        else:
            others.append((pbox, conf))  # 只有非攀爬者才进入扶梯人候选池

    # ② 扶梯人：脚与梯底基线齐平（站在地面） + 水平贴近梯子
    spotters = []
    for pbox, conf in others:
        px1, py1, px2, py2 = pbox
        cx = (px1 + px2) / 2.0
        feet = py2
        # 水平距梯框（人到框最近边的距离；落在梯框内则为 0）
        dx = max(lx1 - cx, cx - lx2, 0.0)
        near_base = abs(feet - ly2) <= SPOTTER_DY * h
        near_ladder = dx <= SPOTTER_DX * w
        if near_base and near_ladder:
            spotters.append((pbox, conf))
    return climbers, spotters


# ══════════════════════════════════════════════════════════════════════════
# 状态机：只在视频 / 摄像头模式下生效；图片模式没有时间轴，走单帧判定
# ══════════════════════════════════════════════════════════════════════════
class LadderGuard:
    """单把梯子的守护状态机，三态 IDLE / CLIMBING / ALARM。

    IDLE      没人上梯，不关心扶梯人是否在场。
    CLIMBING  有人上梯；用 no_spotter_since 累计「无扶梯人」时长，超 grace 转 ALARM。
    ALARM     已确认违规；用 spotter_since 累计「扶梯人连续在场」时长，超 clear 回落。

    lost_since / alarm_count 由 GuardTracker 跨帧维护，本类只负责状态迁移。
    所有时间都用 time.monotonic() 的秒值，不用墙钟时间（避免系统改时导致计时错乱）。
    """

    def __init__(self, gid, box, now):
        self.id = gid                     # 梯子身份编号，用于画面标签与抓拍文件名
        self.box = box                    # 最近一帧的梯子框，供跨帧 IoU 匹配
        self.state = "IDLE"              # 初始状态
        self.no_spotter_since = None      # CLIMBING：本轮「无扶梯人」起始时刻；None=当前有扶梯人
        self.spotter_since = None         # ALARM：本轮「扶梯人连续在场」起始时刻
        self.lost_since = None            # 梯子跨帧未被匹配到的起始时刻（由 Tracker 设置）
        self.alarm_count = 0              # 累计进入 ALARM 的次数，兼作抓拍去重依据

    def update(self, box, persons, now, grace, clear):
        """喂入本帧梯子框与全部人框，推进一次状态迁移。

        Args:
            box: 本帧该梯子的并集框。
            persons: 本帧全部人框。
            now: time.monotonic() 秒值。
            grace / clear: 命令行传入的宽限秒数 / 消警秒数。

        Returns:
            (state, climbers, spotters)：当前状态 + 本帧分类结果，供绘制与终判使用。
        """
        self.box = box
        self.lost_since = None            # 本帧被匹配上了，清除丢失计时
        climbers, spotters = classify_persons(box, persons)
        has_climber, has_spotter = bool(climbers), bool(spotters)

        if self.state == "IDLE":
            # 有人上梯就进入 CLIMBING；若扶梯人已在场则不启动无扶梯人计时
            if has_climber:
                self.state = "CLIMBING"
                self.no_spotter_since = None if has_spotter else now

        elif self.state == "CLIMBING":
            if not has_climber:
                # 攀爬者下梯，看护要求立即失效，整个计时清零
                self.state = "IDLE"
                self.no_spotter_since = None
            elif has_spotter:
                # 有扶梯人：不累计违规时长（即便之前已经累计了一部分也清零，重新计）
                self.no_spotter_since = None
            else:
                # 无扶梯人：首次出现记下起点，之后每帧检查是否已超 grace
                if self.no_spotter_since is None:
                    self.no_spotter_since = now
                elif now - self.no_spotter_since >= grace:
                    self.state = "ALARM"
                    self.alarm_count += 1
                    self.spotter_since = None

        elif self.state == "ALARM":
            if not has_climber:
                # 攀爬者下梯 -> 直接复位，不必等扶梯人
                self.state = "IDLE"
                self.spotter_since = None
            elif has_spotter:
                # 扶梯人必须「连续」在场 clear 秒才消警；中途漏检一帧就重新计时
                if self.spotter_since is None:
                    self.spotter_since = now
                elif now - self.spotter_since >= clear:
                    self.state = "CLIMBING"
                    self.no_spotter_since = None
                    self.spotter_since = None
            else:
                self.spotter_since = None
        return self.state, climbers, spotters


class GuardTracker:
    """跨帧维护「梯子框 -> LadderGuard」映射。

    每帧做三件事：① 用 IoU 把本帧梯子框匹配到已有状态机（贪心取最大 IoU，
    且一个状态机一帧只能被匹配一次）；② 没匹配上的梯子开新状态机；
    ③ 本帧没被匹配上的旧状态机标记丢失，连续丢失超 LOST_SEC 就销毁。
    多把梯子各自独立计时、独立告警，互不干扰。
    """

    def __init__(self):
        self.guards = {}   # gid -> LadderGuard
        self._next = 0     # 自增编号，仅用于显示与抓拍命名

    def step(self, ladder_boxes, persons, now, grace, clear):
        """推进一帧。

        Args:
            ladder_boxes: 本帧 merge_ladders() 之后的梯子框列表。
            persons: 本帧全部人框（每把梯子都会拿到同一份全集，各自分类）。
            now / grace / clear: 同 LadderGuard.update()。

        Returns:
            [(guard, climbers, spotters), ...]，本帧在场的每把梯子一项；
            无梯子时返回空列表，调用方据此判定「不涉及」。
        """
        matched, results = {}, []
        for lbox, _conf in ladder_boxes:
            # ① 在尚未被本帧占用的状态机里，找 IoU 最大的那个作为同一把梯子
            best_gid, best_iou = None, IOU_MATCH
            for gid, g in self.guards.items():
                if gid in matched:
                    continue
                v = iou(lbox, g.box)
                if v > best_iou:
                    best_gid, best_iou = gid, v
            # ② 匹配不上就新建状态机（初始 IDLE，本帧才会推进到 CLIMBING）
            if best_gid is None:
                self._next += 1
                g = LadderGuard(self._next, lbox, now)
                self.guards[g.id] = g
            else:
                g = self.guards[best_gid]
            matched[g.id] = True
            # ③ 用本帧的框和人推进该梯子的状态机
            state, climbers, spotters = g.update(lbox, persons, now, grace, clear)
            results.append((g, climbers, spotters))

        # 没匹配上的老状态机：首次标记丢失时刻，超过 LOST_SEC 才真正销毁，
        # 这样梯子被短暂遮挡一两帧不会丢掉已经累计的 grace 计时。
        for gid in list(self.guards):
            if gid not in matched:
                g = self.guards[gid]
                if g.lost_since is None:
                    g.lost_since = now
                elif now - g.lost_since >= LOST_SEC:
                    del self.guards[gid]
        return results


# ══════════════════════════════════════════════════════════════════════════
# 绘制工具：目标框、底部 HUD、终判横幅
# ══════════════════════════════════════════════════════════════════════════
def draw_box(frame, box, color, label, thick=2):
    """在 frame 上画一个带实底标签条的检测框（原地修改，不返回）。

    标签默认画在框上方；框贴到画面顶部时自动改画在框内侧，避免文字被裁掉。
    label 必须是 ASCII（cv2.putText 无中文字体回退）。
    """
    x1, y1, x2, y2 = (int(v) for v in box)
    cv2.rectangle(frame, (x1, y1), (x2, y2), color, thick)
    (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 1)
    ty = y1 - th - 8 if y1 > th + 8 else y1 + 2  # 贴顶时标签条改画框内侧
    cv2.rectangle(frame, (x1, ty), (x1 + tw + 6, ty + th + 6), color, -1)
    cv2.putText(frame, label, (x1 + 3, ty + th + 2),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2, cv2.LINE_AA)


def draw_hud(frame, lines):
    """底部 HUD：把 [(text, color), ...] 从下往上叠加，每行垫黑底保证可读。

    lines 里第一行（通常是告警行）会显示在最上方，统计行显示在最下方。
    """
    h = frame.shape[0]
    y = h - 10
    for text, color in reversed(lines):
        (tw, th), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 1)
        cv2.rectangle(frame, (0, y - th - 8), (tw + 16, y + 2), (0, 0, 0), -1)
        cv2.putText(frame, text, (8, y - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2, cv2.LINE_AA)
        y -= th + 12


# ══════════════════════════════════════════════════════════════════════════
# 视频采集：摄像头用独立线程只保留最新帧，避免推理慢时延迟不断累积
# ══════════════════════════════════════════════════════════════════════════
class FrameStream:
    """摄像头采集线程：只保留最新一帧。

    主线程推理 + 显示慢于摄像头采集速度时，旧帧直接被覆盖而不是排队，
    因此画面始终是当前时刻（宁可跳帧，也不累积延迟）。视频文件不走这里，
    文件按帧顺序读，保证状态机时序与原始视频一致。沿用 detect_laddersafe.py 方案。
    """

    def __init__(self, cap):
        self.cap = cap
        self.lock = threading.Lock()      # 保护 pending 的读写
        self.pending = None              # 唯一待处理帧；None 表示已被主线程取走
        self.ended = False               # 采集线程读到流结尾 / 摄像头断开
        self.running = True              # 置 False 让采集线程自然退出
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        """采集线程主体：持续 read()，只覆盖 pending，不做任何推理或显示。"""
        while self.running:
            ok, frame = self.cap.read()
            if not ok:
                self.ended = True
                break
            with self.lock:
                self.pending = frame

    def read(self):
        """原子地取走最新帧并清空 pending；没有新帧时返回 None。"""
        with self.lock:
            frame, self.pending = self.pending, None
        return frame

    def stop(self):
        """通知采集线程退出并等待其收尾（最多 1 秒，超时不等，daemon 线程会随进程结束）。"""
        self.running = False
        self.thread.join(timeout=1.0)


def open_video(source):
    """打开视频源，返回 (cv2.VideoCapture, 是否实时流)。

    纯数字字符串（如 "0"）或 int 视为摄像头索引，其余按视频文件路径处理。
    打不开时打印提示并返回 (None, live)，由调用方直接结束。
    """
    live = isinstance(source, int) or str(source).isdigit()
    cap = cv2.VideoCapture(int(source) if live else str(source))
    if not cap.isOpened():
        print(f"无法打开视频源: {source}")
        return None, live
    return cap, live


# ── 推理封装：双模型对同一帧各跑一次 ───────────────────────────────────────
def infer(ladder_model, person_model, frame, args):
    """对一帧同时跑梯子模型 + 人模型，返回 (梯子框列表, 人框列表)。

    Args:
        ladder_model / person_model: load_models() 加载并挂好 *_ids 属性的 YOLO 实例。
        frame: BGR ndarray（原图或视频帧）。
        args: 命令行参数，取 conf / imgsz / device。

    说明：两个模型共用同一 conf / imgsz；verbose=False 关掉逐帧日志。
    类别过滤靠 load_models() 预先算好的 ladder_ids / person_ids，
    所以 PPE 权重里其它 4 类（安全帽、反光背心等）不会混进人框列表。
    """
    r1 = ladder_model.predict(frame, conf=args.conf, imgsz=args.imgsz, device=args.device, verbose=False)[0]
    r2 = person_model.predict(frame, conf=args.conf, imgsz=args.imgsz, device=args.device, verbose=False)[0]
    return extract(r1, ladder_model.ladder_ids), extract(r2, person_model.person_ids)


# ══════════════════════════════════════════════════════════════════════════
# 违规终判：违规 = 有人在梯子上 且 梯下无人看护
# ══════════════════════════════════════════════════════════════════════════
def frame_verdict(has_ladder, climbers, spotters, alarms):
    """把一帧的检测统计翻译成一句明确的终判。

    Args:
        has_ladder: 本帧是否检到梯子（图片模式看梯子列表，视频模式看 Tracker 结果）。
        climbers: 攀爬者数量（视频模式为 ON LADDER + NO SPOTTER 两类之和）。
        spotters: 扶梯人数量（仅用于语义完整性；判定主要看 alarms）。
        alarms: 本帧违规计数。图片模式=「有攀爬者且无扶梯人」的梯子数；
                视频模式=状态机处于 ALARM 的攀爬者框数（已过 grace 宽限）。

    Returns:
        (中文原因文案, 英文横幅文案, 是否违规)。中文走控制台，英文画在画面上。

    四种情形与优先级：
      1. 无梯子            -> 不违规（不涉及，最高优先级，避免域外误检引发连锁误报）
      2. alarms > 0        -> 违规（有人登高 + 无人看护）
      3. 有攀爬者          -> 不违规（有人登高 + 有人看护）
      4. 有梯子但无人登高  -> 不违规（看护要求不成立）
    """
    if not has_ladder:
        return "无梯子，不涉及登高守护", "N/A NO LADDER", False
    if alarms:
        return "有人登高、无人看护", "VIOLATION NO SPOTTER", True
    if climbers:
        return "有人登高、有人看护", "OK SPOTTED", False
    return "有梯子但无登高作业", "OK NO CLIMB", False


def draw_verdict(frame, en_text, bad):
    """左上角终判横幅：整幅画面唯一一处红/绿结论，扫一眼就知道是否违规。

    Args:
        en_text: frame_verdict() 返回的英文横幅文案。
        bad: 是否违规，决定红色（违规）/ 绿色（不违规）。

    cv2.putText 不支持中文（会画成方框），因此画面用英文，中文判定打印到控制台。
    """
    color = COLOR_ALARM if bad else COLOR_LADDER
    (tw, th), _ = cv2.getTextSize(en_text, cv2.FONT_HERSHEY_SIMPLEX, 0.8, 2)
    cv2.rectangle(frame, (0, 0), (tw + 16, th + 16), (0, 0, 0), -1)
    cv2.putText(frame, en_text, (8, th + 8), cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2, cv2.LINE_AA)


# ══════════════════════════════════════════════════════════════════════════
# 图片 / 文件夹模式：单帧几何判定（无时间轴，状态机不生效）
# ══════════════════════════════════════════════════════════════════════════
def run_images(ladder_model, person_model, files, args, out_dir):
    """逐张图片：双模型推理 -> 梯子框聚合 -> 单帧几何分类 -> 画框 -> 打印终判 -> 汇总。

    单张图片没有 grace 宽限，因此「有攀爬者且无扶梯人」当帧即判违规（画红框）。
    这是静态图的合理退化；真实连续监控请走视频/摄像头模式，由状态机防抖。

    Args:
        files: 已按文件名排序的 Path 列表。
        args / out_dir: 命令行参数与结果目录；args.save 控制是否写 *_pred 图，
                        args.show 控制是否弹窗口（按 q 提前结束）。
    """
    if args.save:
        out_dir.mkdir(parents=True, exist_ok=True)
    total_alarm = 0       # 全部图片的违规计数（按攀爬者框累加）
    hit_ladder = 0        # 检到梯子的图片张数
    violation_imgs = 0    # 判为违规的图片张数（用于结尾「最终结果」）
    for path in files:
        img = cv2.imread(str(path))
        if img is None:  # 文件损坏 / 扩展名不符，跳过而不中断整批
            print(f"无法读取: {path}")
            continue
        t0 = time.perf_counter()
        # ① 推理 + 梯子框聚合（去重复框，避免子框底边造成假违规）
        ladders, persons = infer(ladder_model, person_model, img, args)
        ladders = merge_ladders(ladders)

        # ② 对每把梯子独立做几何分类，并画框 / 统计
        counts = Counter()
        alarms = 0
        for lbox, lconf in ladders:
            draw_box(img, lbox, COLOR_LADDER, f"LADDER {lconf:.2f}")
            counts["ladder"] += 1
            climbers, spotters = classify_persons(lbox, persons)
            no_spotter = bool(climbers) and not spotters  # 本梯：有人上梯 + 梯下无人
            for pbox, conf in climbers:
                # 违规时攀爬者画红框 NO SPOTTER，合规时画橙框 ON LADDER
                color = COLOR_ALARM if no_spotter else COLOR_CLIMBER
                label = f"NO SPOTTER {conf:.2f}" if no_spotter else f"ON LADDER {conf:.2f}"
                draw_box(img, pbox, color, label, thick=3 if no_spotter else 2)
                counts["climber"] += 1
                alarms += int(no_spotter)
            for pbox, conf in spotters:
                draw_box(img, pbox, COLOR_SPOTTER, f"SPOTTER {conf:.2f}")
                counts["spotter"] += 1

        # ③ 没参与任何梯子判定的人画灰色细框，方便人工核对人模型到底看见了谁
        #    （classify_persons 复用 persons 里的同一个 tuple 对象，故可用 id() 比对）
        involved = set()
        for lbox, _ in ladders:
            climbers, spotters = classify_persons(lbox, persons)
            involved.update(id(p) for p, _ in climbers + spotters)
        for pbox, conf in persons:
            if id(pbox) not in involved:
                draw_box(img, pbox, (160, 160, 160), f"person {conf:.2f}", thick=1)

        # ④ 底部 HUD（告警行 + 各类框统计）与左上角终判横幅
        total_alarm += alarms
        hit_ladder += int(counts["ladder"] > 0)
        ms = (time.perf_counter() - t0) * 1000
        summary = " ".join(f"{k}:{v}" for k, v in sorted(counts.items())) or "no ladder"
        lines = [(f"NO SPOTTER x{alarms}", COLOR_ALARM)] if alarms else []
        lines.append((summary, (255, 255, 255)))
        draw_hud(img, lines)
        zh, en, bad = frame_verdict(bool(ladders), counts["climber"], counts["spotter"], alarms)
        draw_verdict(img, en, bad)
        verdict = "违规" if bad else "不违规"
        print(f"{path.name}: {ms:.0f} ms, {summary}")
        print(f"    违规判定: {verdict} —— {zh}")
        violation_imgs += int(bad)

        # ⑤ 存图 / 显示（按 q 可提前中断整批）
        if args.save:
            cv2.imwrite(str(out_dir / f"{path.stem}_pred{path.suffix}"), img)
        if args.show:
            cv2.imshow(WINDOW, img)
            if cv2.waitKey(0) & 0xFF == ord("q"):
                break
    cv2.destroyAllWindows()
    print(f"共 {len(files)} 张, 有梯子 {hit_ladder} 张, 告警合计 {total_alarm}")
    print(f"最终结果: {'违规 —— 存在登高无人看护画面' if violation_imgs else '不违规 —— 无登高无人看护画面'}"
          f"（违规 {violation_imgs} 张）{' -> ' + str(out_dir) if args.save else ''}")


# ══════════════════════════════════════════════════════════════════════════
# 视频 / 摄像头模式：逐帧 + GuardTracker 状态机 + 告警抓拍
# ══════════════════════════════════════════════════════════════════════════
def run_stream(ladder_model, person_model, source, args, out_dir):
    """连续帧流水线：采集 -> 双模型推理 -> 梯子聚合 -> 状态机 -> 画框/抓拍 -> 终判 -> 汇总。

    与图片模式的区别：违规不是「单帧没有扶梯人」，而是「状态机进入 ALARM」，
    即无扶梯人状态已持续超过 grace 秒。这是防止告警闪烁的关键。

    Args:
        source: 摄像头索引(int) 或视频文件路径。
        args / out_dir: 命令行参数与结果目录；保存视频为 output.mp4，
                       告警抓拍落在 out_dir/alarm/。
    """
    cap, live = open_video(source)
    if cap is None:
        return
    # 保存视频时沿用源视频的 fps 与尺寸，保证输出与输入等长、画面不变形
    fps_src = cap.get(cv2.CAP_PROP_FPS) or 25.0
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    writer = None
    if args.save:
        out_dir.mkdir(parents=True, exist_ok=True)
        fourcc = cv2.VideoWriter_fourcc(*"mp4v")
        writer = cv2.VideoWriter(str(out_dir / "output.mp4"), fourcc, fps_src, (width, height))
    alarm_dir = out_dir / "alarm" if args.save else None

    # 实时摄像头启用采集线程；视频文件按序逐帧读，保证状态机时间轴准确
    stream = FrameStream(cap) if live else None
    duration = 1000.0 / fps_src if not live else 1  # 文件回放按原速显示
    tracker = GuardTracker()
    frames = 0            # 已处理帧数
    ema_ms = 0.0          # 单帧耗时的指数滑动平均，用于显示 fps
    total_alarms = 0      # 累计进入 ALARM 的次数（=抓拍张数）
    violation_frames = 0  # 违规帧数，用于结尾「最终结果」

    while True:
        t0 = time.perf_counter()
        # ① 取帧：实时流从采集线程拿最新帧，文件按序读；取不到就稍等或结束
        frame = stream.read() if stream else None
        if frame is None and not stream:
            ok, frame = cap.read()
            if not ok:
                break
        if frame is None:
            if stream and stream.ended:
                break
            time.sleep(0.001)
            continue

        # ② 推理 + 推进状态机（now 用单调时钟，跨帧计时不受系统时间调整影响）
        now = time.monotonic()
        ladders, persons = infer(ladder_model, person_model, frame, args)
        result = tracker.step(merge_ladders(ladders), persons, now, args.grace, args.clear)

        # ③ 画框：梯子绿、攀爬者橙（ALARM 时红）、扶梯人蓝；同时累计各类计数
        counts = Counter()
        for g, climbers, spotters in result:
            draw_box(frame, g.box, COLOR_LADDER, f"LADDER {g.id}")
            counts["ladder"] += 1
            alarm = g.state == "ALARM"
            for pbox, conf in climbers:
                if alarm:
                    draw_box(frame, pbox, COLOR_ALARM, f"NO SPOTTER {conf:.2f}", thick=3)
                    counts["no_spotter"] += 1
                else:
                    draw_box(frame, pbox, COLOR_CLIMBER, f"ON LADDER {conf:.2f}")
                    counts["climber"] += 1
            for pbox, conf in spotters:
                draw_box(frame, pbox, COLOR_SPOTTER, f"SPOTTER {conf:.2f}")
                counts["spotter"] += 1

        # ④ 告警抓拍：g.alarm_count 在进入 ALARM 的瞬间 +1，
        #    用 g.saved 记录已存到第几次，保证一次告警只存一张（不会每帧刷图）
        for g, _, _ in result:
            if g.state == "ALARM" and alarm_dir is not None and g.alarm_count > getattr(g, "saved", 0):
                alarm_dir.mkdir(parents=True, exist_ok=True)
                ts = time.strftime("%H%M%S")
                cv2.imwrite(str(alarm_dir / f"alarm_ladder{g.id}_{ts}.jpg"), frame)
                g.saved = g.alarm_count
                total_alarms += 1

        # ⑤ 逐帧终判：只有状态机进入 ALARM 的攀爬者框才计入违规帧；
        #    CLIMBING 阶段（grace 宽限内）仍是「不违规」，避免短时漏检误报。
        summary = " ".join(f"{k}:{v}" for k, v in sorted(counts.items())) or "no ladder"
        lines = [(f"NO SPOTTER x{counts['no_spotter']}", COLOR_ALARM)] if counts["no_spotter"] else []
        lines.append((summary, (255, 255, 255)))
        draw_hud(frame, lines)
        on_climb = counts["climber"] + counts["no_spotter"]
        zh, en, bad = frame_verdict(bool(result), on_climb, counts["spotter"], counts["no_spotter"])
        draw_verdict(frame, en, bad)
        violation_frames += int(bad)
        frames += 1

        # ⑥ 右上角 fps（EMA 平滑），写视频 / 显示窗口 / 按 q 或 Esc 退出
        ms = (time.perf_counter() - t0) * 1000
        ema_ms = ms if ema_ms == 0 else 0.9 * ema_ms + 0.1 * ms
        cv2.putText(frame, f"{1000.0 / ema_ms:5.1f} fps", (width - 110, 28),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2, cv2.LINE_AA)

        if writer:
            writer.write(frame)
        if args.show:
            cv2.imshow(WINDOW, frame)
            key = cv2.waitKey(max(1, int(duration))) & 0xFF
            if key == ord("q") or key == 27:
                break
        if live and args.max_frames and frames >= args.max_frames:
            break

    # ⑦ 收尾：释放线程 / 摄像头 / 写盘句柄，打印汇总与最终结论
    if stream:
        stream.stop()
    cap.release()
    if writer:
        writer.release()
    cv2.destroyAllWindows()
    print(f"处理 {frames} 帧, 平均 {1000.0 / max(ema_ms, 1e-6):.1f} fps, 告警进入 {total_alarms} 次"
          f" -> {out_dir / 'output.mp4' if writer else '(未保存)'}")
    print(f"最终结果: {'违规 —— 出现登高无人看护时段' if violation_frames else '不违规 —— 全程无登高无人看护时段'}"
          f"（违规帧 {violation_frames}/{frames}）")


# ══════════════════════════════════════════════════════════════════════════
# 入口：权重加载 / 参数解析 / 输入源路由
# ══════════════════════════════════════════════════════════════════════════
def load_models(args):
    """加载两个 YOLO 权重，并预先算好各自的类别 id 集合。

    把 ladder_ids / person_ids 直接挂到模型实例上，infer() 每帧就不必重复解析
    names。若权重里找不到对应类别则直接报错退出，避免「跑起来但一个框都没有」。

    Returns:
        (ladder_model, person_model)
    """
    ladder_model = YOLO(str(Path(args.ladder_model)))
    person_model = YOLO(str(Path(args.person_model)))
    ladder_ids, _ = pick_classes(ladder_model.names)
    _, person_ids = pick_classes(person_model.names)
    if not ladder_ids:
        raise SystemExit(f"[error] 梯子模型里没有 ladder 类: {args.ladder_model} (classes={ladder_model.names})")
    if not person_ids:
        raise SystemExit(f"[error] 人员模型里没有 worker/person 类: {args.person_model} (classes={person_model.names})")
    ladder_model.ladder_ids = ladder_ids
    person_model.person_ids = person_ids
    # 打印实际生效的类别，换权重时一眼能看出有没有选错类（例如 ppe2 应只有 people）
    print(f"ladder model = {Path(args.ladder_model).name}, classes = {[ladder_model.names[i] for i in ladder_ids]}")
    print(f"person model = {Path(args.person_model).name}, classes = {[person_model.names[i] for i in person_ids]}")
    return ladder_model, person_model


def main():
    """解析命令行参数，检查权重 / 设备，按输入源类型分派到 run_images 或 run_stream。

    路由规则：纯数字 -> 摄像头(run_stream)；文件夹 -> 其中全部图片(run_images)；
    视频扩展名 -> run_stream；其余 -> 单张图片(run_images)。
    """
    parser = argparse.ArgumentParser(description="登高守护：梯上有人必须梯下有扶梯人（图片/文件夹/视频/摄像头）")
    parser.add_argument("--source", type=str, default="0", help="0=摄像头, 或图片/视频/文件夹路径")
    parser.add_argument("--ladder-model", type=str, default="runs/detect/ladder_yolo11n/weights/best.pt",
                        help="梯子检测权重（默认 ladder_yolo11n，1 类 Ladder）")
    parser.add_argument("--person-model", type=str, default="runs/detect/ppe2_yolo11n/weights/best.pt",
                        help="人员检测权重；默认 PPE 的 people 类（本项目自有域，实测梯上/梯下都能检出）。"
                             "备选：scripts/yolo11n.pt（COCO，效果相当）；"
                             "不要单独用 ppe2_yolo11n-2（漏梯上的人）或 laddersafe（漏弯腰扶梯人）")
    parser.add_argument("--conf", type=float, default=0.25, help="置信度阈值（两个模型共用）。"
                             "低于 0.15 会让梯子域外误检明显增多，可能引发误报")
    parser.add_argument("--imgsz", type=int, default=1280,
                        help="推理尺寸；1080p 场景用 1280，降到 640 会漏掉梯上的人（实测），"
                             "小图/裁剪图域用 640")
    parser.add_argument("--device", type=str, default=None, help="自动选择：有 CUDA 用 0, 否则 cpu")
    parser.add_argument("--out", type=str, default="runs/detect/ladderguard_result", help="结果输出目录")
    parser.add_argument("--grace", type=float, default=GRACE_SEC, help="无扶梯人持续多少秒才告警")
    parser.add_argument("--clear", type=float, default=CLEAR_SEC, help="扶梯人连续在场多少秒解除告警")
    parser.add_argument("--max-frames", type=int, default=0, help="摄像头最多跑多少帧后停, 0=不限")
    io_grp = parser.add_mutually_exclusive_group()
    io_grp.add_argument("--save", dest="save", action="store_true", help="保存结果（默认已开）")
    io_grp.add_argument("--no-save", dest="save", action="store_false", help="不保存结果")
    parser.set_defaults(save=True)
    parser.add_argument("--no-show", dest="show", action="store_false", help="不弹窗口")
    parser.set_defaults(show=True)
    args = parser.parse_args()

    # 权重文件缺失时提前退出，并明确报出是哪一条路径
    for m in (args.ladder_model, args.person_model):
        if not Path(m).is_file():
            raise SystemExit(f"权重不存在: {m}")
    if args.device is None:
        args.device = "0" if torch.cuda.is_available() else "cpu"

    ladder_model, person_model = load_models(args)
    print(f"device={args.device} conf={args.conf} imgsz={args.imgsz} grace={args.grace}s clear={args.clear}s")

    out_dir = Path(args.out)
    src = args.source
    if src.isdigit():
        run_stream(ladder_model, person_model, int(src), args, out_dir)
    elif Path(src).is_dir():
        files = [f for f in sorted(Path(src).iterdir()) if f.suffix.lower() in IMAGE_EXT]
        if not files:
            raise SystemExit(f"文件夹里没有图片: {src}")
        print(f"文件夹 {src}: {len(files)} 张")
        run_images(ladder_model, person_model, files, args, out_dir)
    elif Path(src).suffix.lower() in VIDEO_EXT:
        run_stream(ladder_model, person_model, src, args, out_dir)
    else:
        run_images(ladder_model, person_model, [Path(src)], args, out_dir)


if __name__ == "__main__":
    main()
