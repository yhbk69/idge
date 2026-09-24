# src/base — 基础设施域

无业务依赖的底层设施，被 media/ai/biz/ui 各域引用；本域不得反向依赖上层。
（S4a 迁移进行中：buffer、queue、threadpool、config、utils 将陆续并入本域。）

## 当前内容

- `runtime_paths.h` — 运行时数据路径唯一事实源（header-only）
  - 约定：config.json / idge.db / backups/ / alarms/ / roll_call_data/ 统一落在启动目录下的 `data/`
  - `migrateLegacy()`：main() 首行调用，把旧布局（根目录散落）一次性迁入 data/
  - `resolveStoredPath()`：兼容历史 DB 记录里的旧 `alarms/...` 截图路径前缀
  - 新增运行时产物路径时必须在此加常量，禁止在业务代码写路径字面量
