# src/base — 基础设施域

无业务依赖的底层设施，被 media/ai/biz/ui 各域引用；本域不得反向依赖上层。
（目录重组已完成：buffer、queue、threadpool、config、utils 均已并入本域。）

## 当前内容

- `runtime_paths.h` — 运行时数据路径唯一事实源（header-only）
  - 约定：config.json / idge.db / backups/ / alarms/ / roll_call_data/ 统一落在
    **进程当前工作目录**下的 `data/`（全是相对路径）。⚠ `run.sh` 会先 `cd` 进
    `build/build_rk3588_linux`，所以经它启动时用的是构建目录里的那份 `data/`，
    与源码根的 `data/` 互不相干；`roll_call_data` 另受 `IDGE_WORKSPACE` 影响（见 `rollCallDir()`）
  - `migrateLegacy()`：main() 首行调用，把旧布局（根目录散落）一次性迁入 data/
  - `resolveStoredPath()`：兼容历史 DB 记录里的旧 `alarms/...` 截图路径前缀
  - 新增运行时产物路径时必须在此加常量，禁止在业务代码写路径字面量
