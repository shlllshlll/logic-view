# Linux build

The macOS and Linux ports share the same libusb backend
(`libsigrok/src/hardware/wch-ch32h417/ch375_wrapper_libusb.c`).

## Dependencies (Debian/Ubuntu)

```bash
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  qtbase5-dev qtbase5-dev-tools libqt5svg5-dev qttools5-dev-tools \
  libglib2.0-dev libglibmm-2.4-dev libsigc++-2.0-dev \
  libboost-dev libboost-filesystem-dev libboost-serialization-dev \
  libboost-system-dev libboost-thread-dev \
  libusb-1.0-0-dev libhidapi-dev libzip-dev zlib1g-dev python3-dev
```

## Build

```bash
./linux/build.sh --clean
```

Output: `dist/LogicAnalyzer-linux-<arch>.tar.gz`, containing `bin/`, `lib/`,
`share/libsigrokdecode/decoders`, the desktop entry and the udev rule.

## USB permissions

```bash
sudo cp linux/99-logicanalyzer.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Then reconnect the device. Both the normal mode (`1a86:5537`) and the IAP
mode (`1a86:fe17`) are covered.

## Run

```bash
tar -xzf dist/LogicAnalyzer-linux-$(uname -m).tar.gz
./LogicAnalyzer-linux-$(uname -m)/bin/LogicAnalyzer
```

If the desktop entry is used, install the tree somewhere stable and adjust
`Exec=` in `share/applications/logicanalyzer.desktop`.
