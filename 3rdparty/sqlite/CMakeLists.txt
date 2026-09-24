# ============================================================================
# sqlite3 静态库构建
# ============================================================================
# SQLite 是公共领域代码，无需特殊许可证
# 使用 amalgamation 版本（单文件编译），性能最佳
# ============================================================================

cmake_minimum_required(VERSION 3.10)

# 编译 sqlite3 为静态库
add_library(sqlite3 STATIC
    sqlite/sqlite3.c
)

# 设置头文件搜索路径（供其他模块引用 sqlite3.h）
target_include_directories(sqlite3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/sqlite
)

# 编译选项
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1          # 启用线程安全模式
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1  # WAL 模式下性能优化
    SQLITE_ENABLE_FTS5           # 启用全文搜索（可选）
)

# 交叉编译优化选项
if(CMAKE_CROSSCOMPILING)
    target_compile_options(sqlite3 PRIVATE -O2)
endif()
