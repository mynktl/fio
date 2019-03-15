#!/bin/bash
CUR_DIR=$PWD
PATCH_FILE=fio_istgt_v0.8.1-RC3_6686759b.patch
ISTGT_TAG="0.8.1"
ISTGT_COMMIT="39771ce57e3efa8e708fcdedc43459756686759b"

if [ ! -f ${PATCH_FILE} ]; then
	echo "patch file for ISTGT not found" >/dev/null 2>&1
	exit 0
fi

if [ $# -ne 1 ]; then
	echo "ISTGT source directory not provided" >/dev/null 2>&1
	exit 0
fi

istgt_dir=$1
cd $istgt_dir
git checkout ${ISTGT_TAG}  >/dev/null 2>&1
if [ $? -ne 0 ]; then
	echo "Failed to checkout 0.8.1 TAG for ISTGT code"
	exit 1
fi
latest_commit=`git rev-list HEAD |head -n 1`
if [ ${latest_commit} != ${ISTGT_COMMIT} ]; then
	echo "Version mismatch.. update the patch"
	exit 1
fi

patch -p1 -N --dry-run --silent< ${CUR_DIR}/${PATCH_FILE}  >/dev/null 2>&1
if [ $? -eq 0 ]; then
	echo "Applying patch to ISTGT source code"
	patch -p1 -N --silent< ${CUR_DIR}/${PATCH_FILE}
	if [ $? -ne 0 ]; then
		echo "Patching ISTGT code failed"
		exit 1
	fi
fi
cd -
