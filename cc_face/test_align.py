import cv2
import numpy as np
from skimage import transform as trans

# 你的关键点（从终端输出复制）
landmarks = np.array([
    [207.3, 490.9],  # Left Eye
    [310.0, 433.2],  # Right Eye
    [270.4, 518.8],  # Nose
    [265.1, 583.2],  # Left Mouth
    [380.1, 531.2]   # Right Mouth
], dtype=np.float32)

# ArcFace 标准模板
arcface_dst = np.array([
    [38.2946, 51.6963],
    [73.5318, 51.5014],
    [56.0252, 71.7366],
    [41.5493, 92.3655],
    [70.7299, 92.2041]
], dtype=np.float32)

# 读取图片
img = cv2.imread('3.jpg')
print(f"原图尺寸: {img.shape}")

# 计算变换矩阵
tform = trans.SimilarityTransform()
tform.estimate(landmarks, arcface_dst)
M = tform.params[0:2, :]

print("\n变换矩阵 M:")
print(M)
print()

# 应用变换
aligned = cv2.warpAffine(img, M, (112, 112), borderValue=0.0)

cv2.imwrite('python_aligned.jpg', aligned)
print("已保存到 python_aligned.jpg")

# 验证关键点是否正确映射
print("\n验证关键点映射:")
for i, name in enumerate(['Left Eye', 'Right Eye', 'Nose', 'Left Mouth', 'Right Mouth']):
    # 应用变换到关键点
    src_pt = np.array([landmarks[i][0], landmarks[i][1], 1])
    dst_pt = M @ src_pt
    
    print(f"{name}:")
    print(f"  原始: ({landmarks[i][0]:.1f}, {landmarks[i][1]:.1f})")
    print(f"  变换后: ({dst_pt[0]:.1f}, {dst_pt[1]:.1f})")
    print(f"  期望: ({arcface_dst[i][0]:.1f}, {arcface_dst[i][1]:.1f})")
    print(f"  误差: ({abs(dst_pt[0]-arcface_dst[i][0]):.2f}, {abs(dst_pt[1]-arcface_dst[i][1]):.2f})")
    print()