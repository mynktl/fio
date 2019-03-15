#!/bin/bash
config=config-host.mak

if [ ! -f ${config} ]; then
	exit 0
fi

istgt=`cat config-host.mak |grep ISTGT |grep -v CONFIG |awk -F '=' '{print $2}'`
if [ -z "${istgt}" ]; then
	exit 0
fi

for file in `ls ${istgt}/src/*.o`; do
	rm $file >/dev/null 2>&1
done
exit 0
