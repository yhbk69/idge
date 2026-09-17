-- ============================================================================
-- idge_init.sql - IDGE 数据库初始化脚本
-- ============================================================================
-- 参考 wvp_safety_alarm 表结构设计
-- ============================================================================

-- 启用 WAL 模式
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
PRAGMA busy_timeout=5000;

-- ============================================================================
-- 报警信息表 (alarms)
-- ============================================================================
-- 参考: wvp_safety_alarm 表结构
-- ============================================================================

CREATE TABLE IF NOT EXISTS alarms (
    id              TEXT PRIMARY KEY,            -- UUID 主键
    alarm_type      TEXT NOT NULL,               -- 报警类型 (no_helmet, fire, fence等)
    alarm_level     INTEGER NOT NULL DEFAULT 3,  -- 报警级别: 1紧急 2重要 3一般 4提示
    alarm_time      TEXT NOT NULL,               -- 报警发生时间 (ISO8601格式)
    channel         INTEGER NOT NULL,            -- 视频通道编号 (0-3)
    class_id        INTEGER NOT NULL,            -- 类别ID
    class_name      TEXT NOT NULL,               -- 类别名称
    confidence      REAL NOT NULL,               -- 置信度 (0.0-1.0)
    image_path      TEXT,                        -- 报警图片路径
    video_path      TEXT,                        -- 报警视频路径
    status          TEXT NOT NULL DEFAULT 'pending',  -- 状态: pending/rectified/false_alarm
    dispose_result  TEXT,                        -- 处置结果
    dispose_user_id INTEGER,                     -- 处置人ID
    dispose_user_name TEXT,                      -- 处置人姓名
    dispose_time    TEXT,                        -- 处置时间
    dispose_photo   TEXT,                        -- 现场处置照片路径
    remark          TEXT,                        -- 补充说明
    read_time       TEXT,                        -- 已读时间
    create_time     TEXT NOT NULL,               -- 入库时间
    update_time     TEXT                         -- 更新时间
);

-- 索引
CREATE INDEX IF NOT EXISTS idx_alarms_type ON alarms(alarm_type);
CREATE INDEX IF NOT EXISTS idx_alarms_level ON alarms(alarm_level);
CREATE INDEX IF NOT EXISTS idx_alarms_status ON alarms(status);
CREATE INDEX IF NOT EXISTS idx_alarms_time ON alarms(alarm_time);
CREATE INDEX IF NOT EXISTS idx_alarms_channel ON alarms(channel);

-- ============================================================================
-- 检测信息表 (detections)
-- ============================================================================
-- 存储每一帧的检测结果
-- ============================================================================

CREATE TABLE IF NOT EXISTS detections (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp       INTEGER NOT NULL,            -- 检测时间戳（毫秒）
    channel         INTEGER NOT NULL,            -- 视频通道编号 (0-3)
    frame_id        INTEGER,                     -- 帧编号
    class_id        INTEGER NOT NULL,            -- 类别ID
    class_name      TEXT NOT NULL,               -- 类别名称
    confidence      REAL NOT NULL,               -- 置信度 (0.0-1.0)
    bbox_left       INTEGER NOT NULL,            -- 检测框左上角X
    bbox_top        INTEGER NOT NULL,            -- 检测框左上角Y
    bbox_right      INTEGER NOT NULL,            -- 检测框右下角X
    bbox_bottom     INTEGER NOT NULL,            -- 检测框右下角Y
    created_at      DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 索引
CREATE INDEX IF NOT EXISTS idx_detections_timestamp ON detections(timestamp);
CREATE INDEX IF NOT EXISTS idx_detections_channel ON detections(channel);
CREATE INDEX IF NOT EXISTS idx_detections_class ON detections(class_name);
CREATE INDEX IF NOT EXISTS idx_detections_channel_time ON detections(channel, timestamp);

-- ============================================================================
-- 完成
-- ============================================================================
