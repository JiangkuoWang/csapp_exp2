#!/bin/bash
> tsh.out                           # 清空输出文件
for i in {01..16}; do               # 循环遍历所有 Trace 文件
    trace_file="trace${i}.txt"      # 设置 Trace 文件名
    # 清理残留进程
    pkill -TERM -P $$  # 终止当前脚本派生的所有子进程
    command="./sdriver.pl -t $trace_file -s ./tsh -a \"-p\""    # 构建测试命令
    echo "$command"                 # 将命令输出到终端
    echo "$command" >> tsh.out      # 将命令打印到 tsh.out 文件中
    eval $command >> tsh.out        # 执行测试并记录输出
done