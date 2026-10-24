Raspberry Pi boards (``raspi0``, ``raspi1ap``, ``raspi2b``, ``raspi3ap``, ``raspi3b``, ``raspi4b``, ``raspicm4``)
=================================================================================================================


QEMU provides models of the following Raspberry Pi boards:

``raspi0`` and ``raspi1ap``
  ARM1176JZF-S core, 512 MiB of RAM
``raspi2b``
  Cortex-A7 (4 cores), 1 GiB of RAM
``raspi3ap``
  Cortex-A53 (4 cores), 512 MiB of RAM
``raspi3b``
  Cortex-A53 (4 cores), 1 GiB of RAM
``raspi4b``
  Cortex-A72 (4 cores), 2 GiB of RAM
``raspicm4``
  Compute Module 4 Rev 1.0: Cortex-A72 (4 cores), 8 GiB of RAM, 76 MiB of
  it reserved for the VideoCore as with the default firmware configuration

Implemented devices
-------------------

 * ARM1176JZF-S, Cortex-A7, Cortex-A53 or Cortex-A72 CPU
 * Interrupt controller
 * DMA controller, with DREQ pacing for SPI0
 * Clock and reset controller (CPRMAN)
 * System Timer
 * GPIO controller
 * Serial ports (BCM2835 AUX - 16550 based - and PL011)
 * Random Number Generator (RNG)
 * Frame Buffer
 * USB host (USBH)
 * GPIO controller
 * SD/MMC host controller
 * SoC thermal sensor
 * USB2 host controller (DWC2 and MPHI)
 * MailBox controller (MBOX)
 * VideoCore firmware (property)
 * VideoCore message queue (VCHIQ) with the shared memory (vc-sm-cma) and
   MMAL services; see `VideoCore services`_
 * Peripheral SPI controller (SPI), also in DMA mode; SPI3 to SPI6 on
   raspi4b and raspicm4
 * Broadcom Serial Controller (I2C)
 * PCIe Root Complex (raspi4b, raspicm4)
 * GENET Ethernet Controller (raspi4b, raspicm4)
 * PL011 UART2 to UART5 (raspi4b, raspicm4)
 * xHCI USB host controller, interface version 1.1 with 64-byte contexts,
   one USB 2.0 port (raspi4b, raspicm4)
 * AXI bridge (ASB) controls of the power domains (raspi4b, raspicm4)
 * Always-on block: HDMI DVP, L2 interrupt controller, HDMI DDC I2C
   controllers (raspi4b, raspicm4)
 * Display pipeline: HVS, pixel valves, transposer (TXP), HDMI with CEC
   (raspi4b, raspicm4); see `Display and GPU`_
 * V3D 4.2 GPU (raspi4b, raspicm4); see `Display and GPU`_
 * BCM4345/6 (CYW43455) wireless LAN on SDIO (raspicm4)

Missing devices
---------------

 * Pulse Width Modulation (PWM)
 * Display output: frame composition, video timing, writeback and HDMI
   signalling (raspi4b, raspicm4)
 * V3D job execution (raspi4b, raspicm4)
 * VideoCore multimedia processing: MMAL buffers, VCHIQ bulk transfers
 * PCIe MSI/MSI-X interrupt delivery (raspi4b) -- INTx works; a PCIe
   device that only signals via MSI/MSI-X needs ``msi=off,msix=off``

Board interfaces
----------------

Serial ports are connected in this order: ``-serial`` 1 is UART0 (PL011),
2 the mini UART (AUX), and on ``raspi4b`` and ``raspicm4`` 3 to 6 are UART2
to UART5. With a host serial port backend (``-chardev serial``) the baud
rate, data bits, parity, stop bits and the RTS and DTR lines programmed by
the guest are applied to the host port.

On ``raspi4b`` and ``raspicm4`` the CTS input of each UART follows the GPIO
pin multiplexer: a UART whose CTS function is not selected on any pin sees
CTS asserted, otherwise CTS is asserted while that pin is low. PL011 CTS
hardware flow control (``UARTCR.CTSEN``) holds back transmission while CTS
is deasserted.

The BCM2711 GPIO pins can be driven from outside the SoC with the ``gpioN``
properties of the GPIO controller, ``high``, ``low`` or ``none`` (released,
the pin follows its pull); edge and level events and the GPIO interrupts
follow the pin level. For example, from the monitor::

  (qemu) qom-set /machine/soc/peripherals/gpio gpio26 low

The ``pins`` property sets the state of the pins at reset, as ``pinctrl get``
prints it: comma-separated ``PIN:FUNC:PULL:LEVEL`` entries, ``FUNC`` one of
``ip``, ``op``, ``a0`` to ``a5``, ``PULL`` one of ``pu``, ``pd``, ``pn`` and
``LEVEL`` ``hi`` or ``lo``. For an output ``LEVEL`` is the output latch; for
any other pin a level its pull does not give is driven by the board. A later
entry for a pin replaces an earlier one. ``raspicm4`` starts from the pin
state its firmware leaves on a CM4 and applies the user's entries after it,
for example the ``gpio=`` lines of ``config.txt``::

  -global bcm2838-gpio.pins=19:op:pd:hi,20:ip:pu:hi

The ``chardev`` property of the GPIO controller connects the pins to a
program outside QEMU, one ``PIN DRIVE`` line per change in each direction,
``DRIVE`` being ``high``, ``low`` or ``none``. A line received drives the pin
like ``gpioN``. A line sent tells how the SoC drives the pin: ``high`` or
``low`` while it is a GPIO output, ``none`` once it is not. When the other
end connects it is sent the state of every output. A malformed line is
answered with a line starting with ``error:``. For example::

  -chardev socket,id=gpio,path=gpio.sock,server=on,wait=off \
  -global bcm2838-gpio.chardev=gpio

An I2C device can be modelled outside QEMU with the ``i2c-chardev`` device on
one of the BSC buses: every bus event at its address is sent as a line on
its chardev, and the program at the other end answers the events that need
an answer (``AA`` a 7-bit address, ``DD`` a byte, both hexadecimal):

=====================  ==================  =====================================
Sent                   Answer              Event
=====================  ==================  =====================================
``start AA write``     ``ack``/``nack``    the controller addresses the device
``start AA read``      ``ack``/``nack``
``send DD``            ``ack``/``nack``    the controller writes a byte
``recv``               ``DD``              the controller reads a byte
``nack``                                   the controller NACKed the byte read
``stop``                                   end of the transfer
=====================  ==================  =====================================

A repeated start is a ``start`` without a ``stop`` before it. The guest waits
for each answer. Without a connection the device does not acknowledge its
address. For example, a device at 0x48 on BSC1::

  -chardev socket,id=sensor,path=sensor.sock,server=on,wait=off \
  -device i2c-chardev,bus=i2c-bus.1,address=0x48,chardev=sensor

On ``raspi4b`` and ``raspicm4`` the SPI chip selects go through the GPIO
pins: an SPI device with chip select index ``cs`` N (``-device ...,cs=N``)
follows the CE N pin of its controller, GPIO 8 and 7 for SPI0, 0 and 24 for
SPI3, 4 and 25 for SPI4, 12 and 26 for SPI5, 18 and 27 for SPI6. In the
pin's SPI function the controller drives it, otherwise it is a GPIO (Linux
``cs-gpios``) or follows its pull. SPI0's bus is ``spi``, the others are
``spi3`` to ``spi6``.

An SPI device can be modelled outside QEMU with the ``spi-chardev`` device:
the traffic of the device is sent as lines on its chardev, and the program
at the other end answers the bytes (``DD`` hexadecimal):

====================  ==============  =====================================
Sent                  Answer          Event
====================  ==============  =====================================
``select``                            chip select asserted
``xfer DD [DD...]``   ``DD [DD...]``  bytes out, for each the byte the
                                      device shifts back while it receives
                                      it
``deselect``                          chip select released
====================  ==============  =====================================

The controller sends the bytes it has queued in one line, up to its FIFO
(64 bytes): it shifts them out when the guest waits for them (status or
FIFO reads, chip select changes), so the guest sees the FIFO fill as on a
bus slower than the CPU. An answer byte may depend on the bytes sent before
it only. The guest waits for each answer. Without a connection the device
answers 0x00. For example, a device on SPI3 CE0 (``spi3-1cs``)::

  -chardev socket,id=adc,path=adc.sock,server=on,wait=off \
  -device spi-chardev,bus=spi3,cs=0,chardev=adc

The BCM2711 die temperature is the ``temperature`` property of the thermal
sensor, in millidegrees Celsius (25000 by default; the range the 10-bit
sensor code covers, -88161 to 410040); the guest reads the nearest code, and
``vcgencmd measure_temp`` converts it as the firmware does::

  (qemu) qom-set /machine/soc/peripherals/thermal temperature 85000

``vcgencmd measure_clock`` reports what the BCM2711 firmware measures on a
Compute Module 4 at idle: the ARM and V3D clocks at their firmware clock
rate, the others at fixed rates, 0 for the gated ones, with the counting
resolution of the firmware's measurement (13.18 kHz, 52.73 kHz for the ARM).
PLLB is three times the ARM clock.

The board identity is set with properties of ``bcm2835-property``:
``board-serial`` (64-bit) and ``mac`` give the serial number and the MAC
address the firmware reports, in the mailbox and in OTP rows 28, 29, 64
and 65 as on a board, row 30 holding the board revision.
``firmware-revision`` (the build time, in seconds since the epoch),
``firmware-variant`` and ``firmware-version`` (starting with the 40 hex
digits of the git hash) describe the firmware build, as the ``start4.elf``
of an image does. For example::

  -global bcm2835-property.board-serial=0x51454d55 \
  -global bcm2835-property.mac=e4:5f:01:51:45:4d

The BCM2711 clock manager runs from a 54 MHz crystal and comes out of reset
in the state the firmware leaves it for the kernel on a Compute Module 4
(the ARM at 600 MHz, the PL011 UARTs at 48 MHz, EMMC2 at 100 MHz), so the
rates Linux computes from its registers are the ones the devices run at.
When the firmware configuration (the file the ``vc-config`` property of
``bcm2835-property`` names, as ``vcgencmd get_config`` reads it) has
``enable_uart=1``, the core clock stays at its rate when the guest asks for
another one: the firmware keeps the mini UART's baud rate.

The firmware reports the framebuffer as a display on HDMI0 (the
``num-displays`` property of ``bcm2835-property``, 1 by default). On
``raspicm4`` it is 0, as on a Compute Module 4 without a monitor: Linux's
``bcm2708_fb`` then disables itself and ``snd_bcm2835`` adds no HDMI sound
card. ``-global bcm2835-property.num-displays=1`` gives it the framebuffer.

The Compute Module 4 IO board has a USB2514 hub on the xHCI port; it can be
added with ``-device usb2514,port=1,id=hub``, and devices plugged in behind
it with ``port=1.1`` to ``port=1.4``.

The real-time clock of the Compute Module 4 IO board, an NXP PCF85063A, is
the ``pcf85063`` I2C device at its address 0x51; it runs on the host clock
and keeps its time across a guest reset. With ``power-lost=on`` it starts
with the oscillator-stop flag set, as on a board without a backup battery:
Linux reports the time as invalid until the guest sets it. For example, on
BSC0::

  -device pcf85063,bus=i2c-bus.0,power-lost=on

Display and GPU
---------------

On ``raspi4b`` and ``raspicm4`` the BCM2711 display pipeline (HVS, pixel
valves, transposer, HDMI) and the V3D GPU are register models: the Linux
``vc4`` and ``v3d`` drivers bind and register their DRM devices, as on a
board without a display. No display is connected: the HDMI hot-plug status
reads disconnected. A display channel, video timing, a writeback or a V3D
job that the guest starts never completes (``-d unimp`` logs it).

A CEC transmission behaves as on a bus with no other device: a directed
message is not acknowledged, a broadcast message is sent. An EDID for the
DDC bus can be added with ``-device i2c-ddc,bus=ddc0,address=0x50`` (DDC1:
``bus=ddc1``).

VideoCore services
------------------

The VideoCore end of VCHIQ, the message queue between Linux and the
firmware, is part of the model: Linux hands over its slot memory with the
``VCHIQ_INIT`` property, then connects and opens services through the
slots and the VCHIQ doorbells. The model has the shared memory service
(``SMEM``, used by ``vc-sm-cma``) and the MMAL service (``mmal``) with the
components the Raspberry Pi 4 firmware has for the ``bcm2835-codec`` and
``bcm2835-isp`` drivers: ``ril.video_decode``, ``ril.video_encode``,
``ril.isp``, ``ril.image_fx`` and ``ril.image_encode``, with the ports and
encodings of that firmware, and ``camera_info``, which finds no camera for
the legacy camera stack. The drivers register their V4L2 devices and media
controllers as on the board, and formats can be enumerated and set. The
components process nothing: enabling a port fails with ``ENOSYS``. Other
services are closed when opened, and a bulk transfer is aborted.

Wireless LAN
------------

The ``raspicm4`` wireless chip, a BCM4345/6 on the Arasan SDHCI (GPIO 34 to
39), is a QEMU NIC model, ``bcm4345``. Linux ``brcmfmac`` drives it as on
the board: it downloads the firmware image, NVRAM and CLM blob it finds in
the guest filesystem, and the model then acts as that firmware, version
7.45.241. The identity, capabilities, iovars, firmware commands and events
are those of the chip on a Compute Module 4: station, access point with
WPA2-PSK (the 4-way handshake runs in the host, ``hostapd`` or
``wpa_supplicant``), 2.4 GHz HT20 and 5 GHz up to VHT80, scans, ARP and
IPv6 neighbour discovery offload in station mode, RSSI events.

The air is the NIC's network backend: 802.11 frames with a radiotap header
(channel, signal, transmit power). Guests whose ``bcm4345`` NICs share a
backend, for example a socket multicast group, see each other's access
points and can associate::

  -nic socket,model=bcm4345,mac=e4:5f:01:51:45:a1,mcast=230.0.0.1:5400,localaddr=127.0.0.1

The transmit power is the one the host sets (``qtxpower``) within the
board limits of the NVRAM (``maxp2ga0``, ``maxp5ga0``). The received signal
is that power less the receiver's ``path-loss`` property in dB (default 75),
which can be changed at run time::

  (qemu) qom-set /machine/wlan path-loss 95

Frames are not encrypted on the air (the keys stay in the model), and there
is no airtime or retransmission model: throughput is that of the backend.
Bluetooth (the chip's UART part, on UART0) is not modelled.
