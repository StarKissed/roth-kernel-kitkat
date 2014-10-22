#!/bin/sh

# Copyright (C) 2011 Twisted Playground

# This script is designed by Twisted Playground for use on MacOSX 10.7 but can be modified for other distributions of Mac and Linux

PROPER=`echo $1 | sed 's/\([a-z]\)\([a-zA-Z0-9]*\)/\u\1\2/g'`

HANDLE=LoungeKatt
KERNELSPEC=/Volumes/android/roth-kernel-kitkat
KERNELREPO=$DROPBOX_SERVER/TwistedServer/Playground/kernels
TOOLCHAIN_PREFIX=/Volumes/android/android-toolchain-eabi-4.7/bin/arm-eabi-
MODULEOUT=$KERNELSPEC/buildimg/boot.img-ramdisk
GOOSERVER=loungekatt@upload.goo.im:public_html
PUNCHCARD=`date "+%m-%d-%Y_%H.%M"`

zipfile=$HANDLE"_StarKissed-KK44-Roth.zip"

CPU_JOB_NUM=8

if [ -e $KERNELSPEC/buildimg/boot.img ]; then
rm -R $KERNELSPEC/buildimg/boot.img
fi
if [ -e $KERNELSPEC/buildimg/newramdisk.cpio.gz ]; then
rm -R $KERNELSPEC/buildimg/newramdisk.cpio.gz
fi
if [ -e $KERNELSPEC/buildimg/zImage ]; then
rm -R $KERNELSPEC/buildimg/zImage
fi

make tegra11_android_defconfig -j$CPU_JOB_NUM ARCH=arm CROSS_COMPILE=$TOOLCHAIN_PREFIX
make tegra114-roth.dtb -j$CPU_JOB_NUM ARCH=arm CROSS_COMPILE=$TOOLCHAIN_PREFIX
make -j$CPU_JOB_NUM ARCH=arm CROSS_COMPILE=$TOOLCHAIN_PREFIX

if [ -e arch/arm/boot/zImage ]; then

    if [ `find . -name "*.ko" | grep -c ko` > 0 ]; then

        find . -name "*.ko" | xargs ${TOOLCHAIN_PREFIX}strip --strip-unneeded

        if [ ! -d $MODULEOUT ]; then
            mkdir $MODULEOUT
        fi
        if [ ! -d $MODULEOUT/lib ]; then
            mkdir $MODULEOUT/lib
        fi
        if [ ! -d $MODULEOUT/lib/modules ]; then
            mkdir $MODULEOUT/lib/modules
        else
            rm -r $MODULEOUT/lib/modules
            mkdir $MODULEOUT/lib/modules
        fi

        for j in $(find . -name "*.ko"); do
            cp -R "${j}" $MODULEOUT/lib/modules
        done

    fi

    cat arch/arm/boot/zImage arch/arm/boot/tegra114-roth.dtb > buildimg/zImage

    cd buildimg
    ./img.sh
    cd ../

    IMAGEFILE=boot-kk.$PUNCHCARD.img
    KENRELZIP="StarKissed-KK44_$PUNCHCARD-Roth.zip"

    cp -r  buildimg/boot.img $KERNELREPO/shieldroth/boot-kk.img
    cp -r  $KERNELREPO/shieldroth/boot-kk.img $KERNELREPO/gooserver/$IMAGEFILE
    scp $KERNELREPO/gooserver/$IMAGEFILE $GOOSERVER/shieldroth/kernel
    rm -R $KERNELREPO/gooserver/$IMAGEFILE

    echo "building boot package"
    cp -R boot.img shieldSKU
    cd shieldSKU
    rm *.zip
    zip -r $zipfile *
    cd ../
    cp -R $KERNELSPEC/shieldSKU/$zipfile $KERNELREPO/$zipfile
    if [ -e $KERNELREPO/$zipfile ]; then
        cp -R $KERNELREPO/$zipfile $KERNELREPO/gooserver/$KENRELZIP
        scp $KERNELREPO/gooserver/$KENRELZIP  $GOOSERVER/shieldroth
        rm -r $KERNELREPO/gooserver/*
    fi

fi

cd $KERNELSPEC
