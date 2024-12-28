#!/bin/bash
DL="https://github.com/OpenIPC/firmware/releases/download/toolchain/toolchain"

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 [goke|hisi|star6b0|star6e|native]"
	exit 1
fi

if [[ "$1" == *"star6b0" ]]; then
	CC=sigmastar-infinity6b0
elif [[ "$1" == *"star6e" ]]; then
	CC=sigmastar-infinity6e
elif [[ "$1" == *"goke" ]]; then
	CC=goke-gk7205v200
elif [[ "$1" == *"hisi" ]]; then
	CC=hisilicon-hi3516ev200
fi

GCC=$PWD/toolchain/$CC/bin/arm-linux-gcc
OUT=msposd

if [[ "$1" != *"native"* && "$1" != *"rockhip"* ]]; then
	if [ ! -e toolchain/$CC ]; then
		wget -c -q --show-progress $DL.$CC.tgz -P $PWD
		mkdir -p toolchain/$CC
		tar -xf toolchain.$CC.tgz -C toolchain/$CC --strip-components=1 || exit 1
		rm -f $CC.tgz
	fi
	OUT=msposd_$1
fi

if [ ! -e firmware ]; then
	git clone https://github.com/openipc/firmware --depth=1
fi

if [ "$1" = "goke" ]; then
	DRV=$PWD/firmware/general/package/goke-osdrv-gk7205v200/files/lib
	make -B CC=$GCC DRV=$DRV TOOLCHAIN=$PWD/toolchain/$CC OUTPUT=$OUT $1
elif [ "$1" = "hisi" ]; then
	DRV=$PWD/firmware/general/package/hisilicon-osdrv-hi3516ev200/files/lib
	make -B CC=$GCC DRV=$DRV TOOLCHAIN=$PWD/toolchain/$CC OUTPUT=$OUT $1
elif [ "$1" = "star6b0" ]; then
	DRV=$PWD/firmware/general/package/sigmastar-osdrv-infinity6b0/files/lib
	make -B CC=$GCC DRV=$DRV TOOLCHAIN=$PWD/toolchain/$CC OUTPUT=$OUT $1
elif [ "$1" = "star6e" ]; then
	DRV=$PWD/firmware/general/package/sigmastar-osdrv-infinity6e/files/lib
	make -B CC=$GCC DRV=$DRV TOOLCHAIN=$PWD/toolchain/$CC OUTPUT=$OUT $1
elif [ "$1" = "rockchip" ]; then
    ./build_rockchip.sh $1
else
	DRV=$PWD
	make DRV=$DRV OUTPUT=$OUT $1
fi
