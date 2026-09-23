import cv2
import numpy as np
import onnxruntime as ort

# 加载模型
sess = ort.InferenceSession('detection.fp32.onnx')

# 读取图片
img = cv2.imread('3.jpg')
img_resized = cv2.resize(img, (640, 640))
img_rgb = cv2.cvtColor(img_resized, cv2.COLOR_BGR2RGB)
img_norm = (img_rgb.astype(np.float32) - 127.5) / 128.0
img_input = np.expand_dims(img_norm, 0)  # NCHW or NHWC?

# 检查输入格式
print("输入shape:", sess.get_inputs()[0].shape)
print("输入name:", sess.get_inputs()[0].name)

# 可能需要转置
if 'NCHW' in str(sess.get_inputs()[0].shape):
    img_input = np.transpose(img_input, (0, 3, 1, 2))

# 推理
outputs = sess.run(None, {sess.get_inputs()[0].name: img_input})

# 打印所有输出的 shape
for i, out in enumerate(outputs):
    print(f"Output {i}: shape={out.shape}, min={out.min():.4f}, max={out.max():.4f}")

# 找到最高分的检测
score_8 = outputs[0]  # stride 8 的 score
print(f"\nStride 8 score shape: {score_8.shape}")
max_idx = score_8.argmax()
max_score = score_8.flatten()[max_idx]
print(f"Max score: {max_score:.4f} at index {max_idx}")

# 获取对应的关键点
kps_8 = outputs[6]  # stride 8 的 keypoints
print(f"Stride 8 keypoints shape: {kps_8.shape}")
kps_values = kps_8.flatten()[max_idx*10 : max_idx*10+10]
print(f"Keypoint values: {kps_values}")