#!/bin/bash
# 卡死现场抓取脚本
# 用法：./dump_hang.sh <程序PID>
# 作用：当 idge 卡死(界面不动)时，抓取所有线程的调用栈，
#       从堆栈能直接看出卡在哪个锁/哪个函数，从而定位死锁。
# 示例：
#   ps aux | grep idge          # 找到 idge 的 PID
#   ./dump_hang.sh <PID>
PID=$1
if [ -z "$PID" ]; then
    echo "用法: $0 <程序PID>"
    exit 1
fi
echo "=== 进程信息 ==="
ps -o pid,stat,etime,cmd -p $PID
echo
echo "=== 所有线程堆栈（卡在哪一帧一目了然）==="
gdb -p $PID -batch \
    -ex "set pagination off" \
    -ex "thread apply all bt" \
    2>&1 | grep -vE "^\[New |^\[Thread |Downloading|Reading symbols|^$" | head -300
