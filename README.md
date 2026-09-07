# Thrust bench

## Build

```bash
cd firmware
mkdir build
cd build

cmake ..
make

../flash.sh
```

## PPP session

```bash
sudo pppd /dev/ttyACM0 1000000 192.168.7.1:192.168.7.2 local noauth debug nodetach
```

## SLIP session

```bash
sudo slattach -L -p slip -s 115200 /dev/ttyUSB0
sudo ip addr add 192.168.9.2/24 dev sl0
sudo ip link set sl0 up
```

## iPerf

```bash
iperf -c 192.168.7.2 -e -i 1 -M 5000 -l 8192
```
