#!/bin/bash
# 打印当前供电与低电量模式（基准结果必须标注：电池 + 低电量模式下 CPU 明显变慢）
src=$(pmset -g batt | head -1 | sed -E "s/.*'(.*)'.*/\1/")
lpm=$(pmset -g | awk '/lowpowermode/{print $2}')
echo "POWER,$(date +%H:%M:%S),source=$src,lowpowermode=$lpm"
