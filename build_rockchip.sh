#!/bin/sh
output=aarch64

# We run arm binary's under x86, crazy, right ;)
which qemu-arm-static 2>/dev/null > /dev/null || sudo apt-get install -y qemu-user-static

# download and unpack debian arm image
host=https://cloud.debian.org/images/cloud/bullseye
release=latest
system=debian-11-generic-arm64.tar
if [ ! -f disk.raw ]; then
    curl -s -k -L ${host}/${release}/${system}.xz -O
    tar -xf ${system}.xz
    rm ${system}.xz
fi

# loop mount the image
if [ ! -d output/tmp ]; then
    mkdir -p $output
    device=$(sudo losetup -P --show -f disk.raw)
    sudo mount ${device}p1 $output
    sudo mkdir -p $output/usr/src/msposd
    sudo mount -o bind $(pwd) $output/usr/src/msposd
fi

if [ ! -f $output/tmp/prepare_chroot.done ]; then
    cat > prepare_chroot.sh << EOL
    #!/bin/bash
    cd /home
    # install radxa APT repo, see https://radxa-repo.github.io/bullseye/
    keyring="/home/keyring.deb"
    version="\$(curl -L https://github.com/radxa-pkg/radxa-archive-keyring/releases/latest/download/VERSION)"
    curl -L --output "\$keyring" "https://github.com/radxa-pkg/radxa-archive-keyring/releases/download/\${version}/radxa-archive-keyring_\${version}_all.deb"
    dpkg -i \$keyring
    echo 'deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bullseye/ bullseye main' > /etc/apt/sources.list.d/70-radxa.list
    echo 'deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bullseye rockchip-bullseye main' > /etc/apt/sources.list.d/80-rockchip.list

    apt-get update
    apt-get install -y git gcc make pkg-config libspdlog-dev libevent-dev libcairo-dev
    apt clean
    touch /tmp/prepare_chroot.done
EOL
    chmod +x prepare_chroot.sh
    sudo cp prepare_chroot.sh $output/home
    sudo rm $output/etc/resolv.conf
    echo nameserver 1.1.1.1 | sudo tee $output/etc/resolv.conf
    sudo chroot $output /home/prepare_chroot.sh
    rm prepare_chroot.sh
fi

if [ "$(uname -m)" = "x86_64" ]; then
    sudo chroot aarch64 make OUTPUT=msposd_$1 $1 -C /usr/src/msposd
else
    make OUTPUT=$OUT $1
fi
sudo umount $output/usr/src/msposd
sudo umount $output
sudo losetup -d ${device}
