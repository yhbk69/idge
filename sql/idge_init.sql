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
--
-- 这份建表与 src/biz/db/database_manager.cpp 的 createTables() **必须同步改**
-- （两边都是 CREATE TABLE IF NOT EXISTS，老库靠 ensureColumns() 的 ALTER 补列）。
--
-- 时间列口径：一律是**定宽本地时间文本** "yyyy-MM-ddTHH:mm:ss"（无时区偏移），
-- 因为所有时间过滤都靠字典序 BETWEEN（判据与实测见 src/biz/db/alarm_dao.cpp 的
-- buildFilterWhere()）；对外要带偏移的 ISO-8601 在出门口补
-- （src/biz/pad/pad_wire.cpp），不动库。
-- ============================================================================

CREATE TABLE IF NOT EXISTS alarms (
    id              TEXT PRIMARY KEY,                  -- 32 位十六进制 UUID（去花括号去横线，makeAlarmRecord 生成）。报警的对外身份：v1 的 alarmId、抓拍文件名对齐、UPDATE 的 WHERE 都靠它，绝不复用
    alarm_type      TEXT NOT NULL,                     -- 现在只产出三种值：detection / fence_in（闯入禁区）/ fence_out（离岗）。⚠ 历史值 `fence`（不带后缀）才是多数：实测两份库 `data/idge.db` 是 fence 933 / detection 2499、**fence_in/fence_out 零行**，`build/build_rk3588_linux/data/idge.db`（run.sh 实际加载那份）是 fence 25862 / detection 10682 / fence_in 1739 / fence_out 11。下游按 "fence" **前缀**认三代（`alarm_dao.cpp:480`、`:1243`、`alarm_list_widget.cpp:778`），精确匹配 fence_in/fence_out 的新代码都会漏掉老行——`pad_wire.cpp:148` 已经踩了（审查报告 §八 P2-4）
    alarm_level     INTEGER NOT NULL DEFAULT 3,        -- 报警级别：1 紧急 / 2 重要 / 3 一般 / 4 提示。当前硬编码只有两档产出：普通检测=3、围栏=2；1/4 级没有产出路径（级别映射表未做）。v1 按 int 传，level 1/2 触发 Pad 全屏强提醒
    alarm_time      TEXT NOT NULL,                     -- 报警**发生**时间，取自推理结果的帧时刻（异常时回落当前墙钟）。定宽无偏移，见上面那段口径
    channel         INTEGER NOT NULL,                  -- 视频通道编号，0-based（0~3）；对外出数是 1-based，UI 列名"通道N"也是 +1
    class_id        INTEGER NOT NULL,                  -- 触发报警的类别在**该槽位模型** labels.txt 里的行号；跨模型不可比，去重限流用的是它不是 class_name
    class_name      TEXT NOT NULL,                     -- labels.txt 的行内容（英文，如 un_helmet / person / cigarette）。三处靠它：报警判定（槽位的 alarm_class 比对）、抓拍/片段文件名、Pad 出站类型表（src/biz/pad/pad_wire.cpp 的 kTable）
    confidence      REAL NOT NULL,                     -- 置信度 0.0~1.0（推理结果 prop）
    image_path      TEXT,                              -- 抓拍图**相对路径** data/alarms/yyyyMMdd/ch{N+1}_{类别}_{HHmmss_zzz}.jpg。入库前就填好（解码线程 deliverAlarms 先写 imagePath 再 storeAndNotify），所以报警一落地就有图可对外。⚠ 老行可能是迁移前的 "alarms/..." 前缀，读取时过 RuntimePaths::resolveStoredPath 重写
    video_path      TEXT,                              -- insert 时**必为空**：片段要"报警后约 2 秒"才由 ClipWriter 写出来，事后 updateVideoPath 回填。存的是 Annex-B 裸流（.h265/.h264）；网页/Pad 要播的同名 .mp4 副本不入库（入库会让本地详情框找不到那段视频）
    status          TEXT NOT NULL DEFAULT 'pending',   -- pending / rectified / false_alarm。⚠ `rectified` 是本机"已确认"的语义 = **我看过了**，不是"已处置"；v1 只有 PENDING/DISPOSED/FALSE_ALARM 三值，所以出站映射成 `PENDING + readAt`（B3 的落地口径）
    dispose_result  TEXT,                              -- 处置措施文本（v1 的 dispose.measure）。**全仓零写入方**：AlarmDAO::dispose() 有实现无调用，等 /api/alarms/{id}/dispose
    dispose_user_id INTEGER,                           -- 处置人 ID。⚠ 类型已与 v1 冲突：对端的 personId 是字符串工号（"05-0101"/"P-001"），处置闭环落地时要改成 TEXT，别在 INTEGER 上硬塞
    dispose_user_name TEXT,                            -- 处置人/误报人姓名（同 dispose_result，无写入方）
    dispose_time    TEXT,                              -- 处置时间（同 alarm_time 口径；v1 的 disposedAt，终态时必填）
    dispose_photo   TEXT,                              -- 现场整改照片**单张**路径。v1 允许 0~5 张（photoUrls 数组）→ 要么换成 JSON 数组列，要么加子表，现在这列装不下
    remark          TEXT,                              -- 补充说明（v1 的 dispose.remark）；AlarmDAO::updateRemark() 有实现，写入方待处置表单
    read_time       TEXT,                              -- **最早**已读时间。markRead() 现在保证"保留最早 + 必推 update_time"（含墙钟回拨），但调用方还没有：v1 的 POST /alarms/{id}/read 未实现
    create_time     TEXT NOT NULL,                     -- 入库时间。建行那一刻由 makeAlarmRecord 写死（现等于 alarm_time）；早期版本留空、靠 DAO 兜底当前时间，所以老行可能是 NULL
    update_time     TEXT,                              -- 最后变更时间：状态/误报/已读/备注/图片/片段每条 UPDATE 都推它（`alarm_dao.cpp:785/811/840/869/900/923/944/965`）。⚠ **v1 的断线补漏游标就是它**（`AlarmDAO::queryUpdatedSince` 按 `>= 游标` 过滤），所以建行即写不能留空——空串会让刚产生的报警被增量整批漏掉。实测两份库的空串行数差两个量级：`data/idge.db` 1992/3432 为空，`build/build_rk3588_linux/data/idge.db` 只有 136/38294 为空（片段回填顺带 bump 了绝大多数行）→ `COALESCE(NULLIF(update_time,''),alarm_time)` 这条兜底在哪份库上真正生效，取决于跑的是哪一个
    worker_name     TEXT,                              -- 施工负责人姓名，扫码登录会话定死后在**产生这一刻**盖上（不等到显示/上行再回查）。报警列表那一列与"按负责人"筛选用它——为这个才单独成列，省掉每行解析 JSON
    construction_info TEXT                             -- 整份施工信息 JSON（compact 原样，含电话/工号/工作内容/construction_id）。详情框逐键渲染与上行 data["construction"] 都从它出，**加字段不用改表**。v1 的 taskId 也取这里的 construction_id（是否等同 v1 施工计划 ID 待甲方确认，报告 B2）
);

-- 索引
CREATE INDEX IF NOT EXISTS idx_alarms_type ON alarms(alarm_type);
CREATE INDEX IF NOT EXISTS idx_alarms_level ON alarms(alarm_level);
CREATE INDEX IF NOT EXISTS idx_alarms_status ON alarms(status);
CREATE INDEX IF NOT EXISTS idx_alarms_time ON alarms(alarm_time);
CREATE INDEX IF NOT EXISTS idx_alarms_channel ON alarms(channel);
-- 检索用复合索引，必须与 database_manager.cpp 的建表段保持一致（建表语句有两份这条老规矩）
CREATE INDEX IF NOT EXISTS idx_alarms_channel_time ON alarms(channel, alarm_time);
CREATE INDEX IF NOT EXISTS idx_alarms_worker ON alarms(worker_name);

-- ============================================================================
-- 检测信息表 (detections)
-- ============================================================================
-- ⚠ 存的是**触发了报警**的那一条检测（AlarmManager::storeDetectionDetail），
--   不是全量帧明细；总开关是 config.json 的 database.storeDetections。别当抽样表用。
-- ============================================================================

CREATE TABLE IF NOT EXISTS detections (
    id              INTEGER PRIMARY KEY AUTOINCREMENT, -- 自增主键，无业务含义
    timestamp       INTEGER NOT NULL,                  -- 帧时间戳，**毫秒**（推理结果 time 的 epoch 纳秒 /1e6）。与 alarms.alarm_time 的文本口径不同，跨表比时间要先换算，别直接字符串比较
    channel         INTEGER NOT NULL,                  -- 视频通道编号，0-based（0~3）。对外出数一律 +1，与 alarms.channel 同一口径
    frame_id        INTEGER,                           -- ⚠ 存的是**级联槽位下标**，不是帧号：storeDetectionDetail 把 slot 塞进 singleResult.id 透传到这里。作用是"这个框是本通道第几个模型检出的"
    class_id        INTEGER NOT NULL,                  -- 类别在**该槽位模型** labels.txt 里的行号。只在同槽位内有意义：不同模型的同一 id 是不同类别
    class_name      TEXT NOT NULL,                     -- labels.txt 的行内容（英文，如 person / un_helmet / cigarette），与 alarms.class_name 同源
    confidence      REAL NOT NULL,                     -- 置信度 0.0~1.0（取自推理结果 prop，不是百分数）
    bbox_left       INTEGER NOT NULL,                  -- 检测框左边界，**原始帧像素**坐标（不是归一化、也不是模型输入 640 空间）
    bbox_top        INTEGER NOT NULL,                  -- 检测框上边界，原始帧像素
    bbox_right      INTEGER NOT NULL,                  -- 检测框右边界，原始帧像素
    bbox_bottom     INTEGER NOT NULL,                  -- 检测框下边界，原始帧像素（= 人的"脚点"所在行，围栏判定用的就是它）
    created_at      DATETIME DEFAULT CURRENT_TIMESTAMP -- ⚠ SQLite 默认值是 **UTC**，且格式是 "yyyy-MM-dd HH:mm:ss"（空格分隔）：与本表的 timestamp、alarms.alarm_time 都差 8 小时且分隔符不同，别拿它做时间过滤，只是插入痕迹
);

-- 索引
CREATE INDEX IF NOT EXISTS idx_detections_timestamp ON detections(timestamp);
CREATE INDEX IF NOT EXISTS idx_detections_channel ON detections(channel);
CREATE INDEX IF NOT EXISTS idx_detections_class ON detections(class_name);
CREATE INDEX IF NOT EXISTS idx_detections_channel_time ON detections(channel, timestamp);

-- ============================================================================
-- 完成
-- ============================================================================
