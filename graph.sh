#!/bin/bash
#To plot graph of CPU/Memory
NAME=$1
FIO=`ps aux |grep fio |grep root |grep -v grep | awk -F ' ' '{print $2}'`

psrecord ${FIO} --interval 1 --plot ${NAME}.png
