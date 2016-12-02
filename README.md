# OpenBttn

OpenBttn is a custom open source firmware for [bt.tn](https://bt.tn) buttons. This firmware does not communicate with the official bt.tn servers and contains some features not available in the original firmware. It's written using the [libopencm3](https://github.com/libopencm3/libopencm3) firmware library for ARM Cortex-M3 microcontrollers.

This firmware is in no way associated with The Button Corporation Ltd.

Installing this firmware will most likely void your warranty.

## Features

* Allow spaces in Wi-Fi SSIDs (the original firmware doesn't).
    * Probably other special characters as well.
* Single and long press
    * Super fast response times in a local setting
* Live configuration (from Web-UI hosted on bt.tn)
* OTA update of the SPWF01SA Wi-Fi module
* Custom led flashing by [POST request](https://github.com/mafredri/openbttn/commit/d2a22cb6291fbe04f809ac2fbe771ca0c1953c66)
* Password authentication for bt.tn configuration

## Supported models

The following bt.tn models have been tested and confirmed working:

* bttn Wi-Fi

### Untested models

* bttn Mobile Data
* bttn Mini Wi-Fi
* bttn Mini Mobile Data

## Gettings started

### First steps

1. Download the latest [release](https://github.com/mafredri/openbttn/releases) or build the project yourself
2. Install the firmware
    * Optionally, backup the original firmware
3. Configure bttn in recovery mode
    1. Boot bttn while holding the button for 4 seconds to enter recovery mode
    2. Connect to the 'OpenBttn' Wi-Fi network
    3. Navigate to `http://192.168.1.1/`
    4. Configure your Wi-Fi settings
4. Configure the button press URLs
    1. Navigate to `http://[bttn_IP_on_your_network]/`
5. Enjoy your OpenBttn!

### Building

Install the [GNU ARM Embedded Toolchain](https://launchpad.net/gcc-arm-embedded/+download):

```shell
brew cask install gcc-arm-embedded
```

Checkout and build the project:

```shell
git clone https://github.com/mafredri/openbttn && cd openbttn
git submodule update --init
make  # builds src/
```

Upload `main.elf` to your bttn using OpenOCD.

### Backup original firmware

1. Start in DFU mode (see below)
2. Run: `dfu-util --device 0483:df11 --dfuse-address 0x08000000 --alt 0 --upload backup.bin`

This will upload the firmware from the bt.tn to your computer and save it as `backup.bin`. Only use this backup to restore your bt.tn to its original state.

### Installing the firmware

This firmware can be installed in two ways, either via DFU mode or via the JTAG interface on the bt.tn board. Below are instructions for starting the bt.tn in DFU mode and downloading the firmware onto it:

1. Open the bt.tn, there are three torx screws at the bottom
2. Connect the [Boot pins](./resources/bttn-boot-pins.png) on the board (note: only the Boot pins, not the FWSel ones)
3. Connect via USB to computer
    * It should now show up in the DFU list (`dfu-util --list`)
4. `cd openbttn/src && make download`

`make download` is the same as manually issuing `dfu-util --device 0483:df11 --dfuse-address 0x08000000 --alt 0 --download main.bin` which requires the `main.bin` to be built first. This downloads the firmware to the bt.tn and overwrites the original firmware.

## Current status

### TODO

* Improve documentation

### Nice to have

* OTA update of OpenBttn (we are already able to OTA update the Wi-Fi module, updating OpenBttn remotely would also be nice)
* More ways to interact with the bt.tn (double press, ulta-long press, etc?)

### Limitations

* We cannot use hardware flow control for the SPWF01SA Wi-Fi module because the CTS/RTS ports of the Wi-Fi module are incorrectly set up in the bt.tn hardware. The CTS/RTS is set up as output/input whereas is should be the other way around, input/output. This makes it impossible for the bt.tn (RTS) to send signals to the Wi-Fi module (CTS) and vice-versa.
    * Using hardware flow control would allow us to use a smaller ring buffer and ask the WiFi module to take breaks in sending it's data, thus relying on it's, much larger, RAM.

## Motivation

* I happened to have a bricked (after OTA update) bt.tn around to play with and wanted some experience with embedded programming
* Local requests in original firmware were too slow
    * A token request to the bt.tn servers were issued before the local request was performed
* Investigate Wi-Fi issues in bt.tn
    * Turns out it's an issue with the SPWF01SA Wi-Fi module
    * Can be worked around by running the WiFi module in power save mode or by connecting to a different WiFi AP
