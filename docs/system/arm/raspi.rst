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
 * DMA controller
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
 * Peripheral SPI controller (SPI)
 * Broadcom Serial Controller (I2C)
 * PCIe Root Complex (raspi4b, raspicm4)
 * GENET Ethernet Controller (raspi4b, raspicm4)

Missing devices
---------------

 * Pulse Width Modulation (PWM)
 * PCIe MSI/MSI-X interrupt delivery (raspi4b) -- INTx works; a PCIe
   device that only signals via MSI/MSI-X needs ``msi=off,msix=off``

Board interfaces
----------------

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
