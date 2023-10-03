ls
tar -xvzf MLNX_OFED_LINUX-4.9-7.1.0.0-ubuntu20.04-x86_64.tgz
pushd MLNX_OFED_LINUX-4.9-7.1.0.0-ubuntu20.04-x86_64
sudo ./mlnxofedinstall --upstream-libs --dpdk
popd
sudo /etc/init.d/openibd restart
sudo apt-get install meson python3-pyelftools
git clone https://github.com/DPDK/dpdk
cd dpdk
git checkout tags/v22.11 -b v22.11
meson build
ninja -C build
sudo ldconfig
echo 1024 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
cd ~
make
ls
mv dpdk_echo.c /dpdk/
mv dpdk_echo.c dpdk/
mv Makefile dpdk/
cd dpdk
make
cd ..
ls
cd dpdk
ls
scp Makefile tc24@128.110.217.219:~/
ls
make
mv Makefile ~/
mv dpdk_echo.c ~/
ls
cd ..
ls
make
pkg-config --libs libdpdk
dpdk
ls
cd dpdk
ls
cd lib
ls
cd ..
ls
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/opt/mellanox/dpdk/lib/aarch64-linuxgnu/pkgconfig
cd ..
make
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/opt/mellanox/dpdk/lib/x86_64-linuxgnu/pkgconfig
make
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/opt/mellanox/grpc/lib/pkgconfig
export PATH=${PATH}:/opt/mellanox/grpc/bin
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/opt/mellanox/doca/lib/aarch64-linuxgnu/pkgconfig
export PATH=${PATH}:/opt/mellanox/doca/tools
make
export PATH=${PATH}:/usr/local/lib/x86_64-linux-gnu/pkgconfig/libdpdk.pc
make
export PATH=${PATH}:/usr/local/lib/x86_64-linux-gnu/pkgconfig/libdpdk.pc
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/usr/local/lib/x86_64-linux-gnu/phgconfig/libdpdk.pc
make
locate -h
sudo apt apt install mlocate
sudo apt  install mlocate
PKG_CONFIG_PATH
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/users/tc24/dpdk/build/meson-private/libdpdk.pc
make
echo PKG_CONFIG_PATH
echo $PKG_CONFIG_PATH
make
echo $PKG_CONFIG_PATH
unset -f $PKG_CONFIG_PATH
echo $PKG_CONFIG_PATH
unset PKG_CONFIG_PATH
echo $PKG_CONFIG_PATH
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/users/tc24/dpdk/build/meson-private/libdpdk.pc
make
echo $PKG_CONFIG_PATH
unset PKG_CONFIG_PATH
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/users/tc24/dpdk/build/meson-private/libdpdk.pc
echo $PKG_CONFIG_PATH
make
exit
sudo ifconfig
