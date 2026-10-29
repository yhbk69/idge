# src/base — 基础设施域

无业务依赖的底层设施，被 media/ai/biz/ui 各域引用；本域不得反向依赖上层。
（目录重组已完成：buffer、queue、config、imaging 均已并入本域；原先的 threadpool 已作为死代码删除，
原 `utils/` 拆成 `imaging/`（图像）+ `biz/service/task_manager`（业务任务目录）+ 删除零调用的 logging/path_utils。）

## 当前内容

- `runtime_paths.h` — 运行时数据路径唯一事实源（header-only）
  - 约定：config.json / idge.db / backups/ / alarms/ / roll_call_data/ 统一落在
    **进程当前工作目录**下的 `data/`（全是相对路径）。⚠ `run.sh` 会先 `cd` 进
    `build/build_rk3588_linux`，所以经它启动时用的是构建目录里的那份 `data/`，
    与源码根的 `data/` 互不相干；`roll_call_data` 另受 `IDGE_WORKSPACE` 影响（见 `rollCallDir()`）
  - `migrateLegacy()`：main() 首行调用，把旧布局（根目录散落）一次性迁入 data/
  - `resolveStoredPath()`：兼容历史 DB 记录里的旧 `alarms/...` 截图路径前缀
  - 新增运行时产物路径时必须在此加常量，禁止在业务代码写路径字面量
- `perf_probe.h` — GUI 卡顿排查的插桩开关（header-only，两个函数级 static）
  - `perfProbe()`：`IDGE_PERF=1` 开启计时日志；默认关闭以免输出本身污染测量。
    需配 `QT_MESSAGE_PATTERN='[%{time hh:mm:ss.zzz}] %{type} %{message}'` 才有可对齐的时间戳
  - `noOverlayDraw()`：`IDGE_NODRAW=1` 跳过 CPU 叠加绘制（框+文字），用于做帧预算 A/B 反证
- `imaging/` — 图像读写、格式转换、letterbox 缩放、裸 buffer 软件绘制、Qt 安全解码、文件读写原语（详见 `imaging/README.md`）
