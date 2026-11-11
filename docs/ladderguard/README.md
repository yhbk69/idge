# 登高守护参考材料

用户提供的原始参考三件套（2026-11-10 收录，原样保留）：

| 文件 | 说明 |
|------|------|
| `登高守护实现方案.md` | 规则定义、几何阈值、状态机与实测记录（含 640 vs 1280 输入精度对比） |
| `detect_ladderguard.py` | Python 参考实现（ultralytics + 规则引擎） |
| `ladderguard_geometry.svg` | 几何判定示意图（方案 md 内引用此文件名） |

C++ 落地：`src/biz/ladderguard/`（纯逻辑规则引擎，离线单测 `tests/test_ladder_guard.cpp`），
接线在 `src/media/reader/ffmpeg_video_decoder.cpp` 的 4.9c/4.10c 段，
报警类型 `ladder_no_spotter`，配置段 `ladderguard`（设置页「登高守护」分组）。