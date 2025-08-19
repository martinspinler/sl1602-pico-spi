Presonus StudioLive 16.0.2 FireWire SPI bridge
==============================================

This project aims to revive the good old Presonus StudioLive mixing console.
The FireWire interface is now outdated and often malfunctions.
This Bridge project replaces the FireWire interface and allows the mixing console to be controlled via USB or WiFi.

Basic communication protocol
----------------------------
Originally, the mixing console is controlled from the software (Presonus Universal Control) via FireWire using MIDI SysEx-like messages.
These messages pass from the TC2210 FireWire chip on the DSP board (DSPB) in the same form through SPI to the AT91SAM chip on the control panel board (CPB).
Communication is based on a request-response pattern, where the requests are created on the SPI slave and the responses are created on the SPI master.

When the SPI slave wants to send a request, it asserts the nIRQ pin and holds it until the whole request is pulled from the SPI master.
Then awaits until the SPI master pushes the response back.

*The same SPI is shared with the DSP AD21715, but no handling of DSP communication is currently planet in this project. Also the DSP probably boots from SPI Flash, at which time it is acting as an SPI master, and the B253 drivers disconnect SCK and MOSI from the AT91SAM chip.*


Hardware description
====================

The basic idea is to use the RPi Pico (W) as SPI interceptor and injector.
For this purpose are used both hardware SPIs on RPi Pico as slave.
SPI0 is used for communication with the CPB and the SPI1 is used for communication with the DSPB.
Both SPIs are driven by the same SCK and nSS.

For interception, the CPB MOSI is connected directly to the MOSI0 (RPi Pico SPI0 RX/input), the DSPB MISO is connected to the MOSI1 (RPi Pico SPI1 RX/input).

For injection, there is a muxing mechanism involved. The mux selects between default value and the injected value.
The CPB MISO is connected to the mux output MUX_MISO0, which select between DSPB MISO and MISO0 (RPi Pico SPI0 TX/output).
The DSPB MOSI is connected to the mux output MUX_MOSI1, which select between CPB MOSI and MISO1 (RPi Pico SPI1 TX/output).

In the injection mode it is also needed to cut off the propagation of the nIRQ1 from the DSPB to the CPB as well as the nSS0 from the CPB to the DSPB. For this purpose, another two muxes are used, but this time the injected mux inputs have constant value of '1'.

Signal multiplexing
-------------------

Although the multiplexing can be done in external ICs like 74HC157, RPi Pico has still lots of unused pins, so why not to use them?
There is a simple RPi PIO program, which does the multiplexing of the signals (maybe it is even faster than external ICs).

RPi Pico GPIO usage
-------------------

===== ===== ===== ===== =====
SPI   MOSI  nSS   SCK   MISO
===== ===== ===== ===== =====
SPI0      4     5     6     7
SPI1      8     9    10    11
===== ===== ===== ===== =====

============= ===== ===== ===== =====
Mux signals   nIRQ0 MOSI1 MISO0 nSS1
============= ===== ===== ===== =====
MUX Output        0     1     2     3
Default in       12    14    16    18
Injected in     -13    15    17   -19
Select          -20   -21   -22   -26
============= ===== ===== ===== =====

*The '-' symbol means not connected by wire: it is drived internaly and readen in PIO*

*Pins for the ``Default in`` has to be connected from the cable*

*Pins ``Injected in`` are used for data injection; it means that the pins 7-17 and 8-15 must be connected by wire*

Mixer modification
------------------

There is no need to make irreversible changes to the mixer.
The basic idea is to prepare an alternate 26 pin cable between DSP board and control panel board.
Some of the wires (Usage column in the table below) have to be cutted and driven from the RPi Pico board.
The SCK wire doesn't need to be spliced, but for better and unified manipulation it is suggested to cut the SCK wire too and connect by another wire on the RPi Pico PCB.

Be careful not to change the cable sides afterwards.

26 Pin connector pinout
~~~~~~~~~~~~~~~~~~~~~~~

===== ===== ============================================
Pin   Usage Notes
===== ===== ============================================
2           OE for B253 gates (SCK, MOSI)
4           AD21715: pin 196 SPIFLG0; maybe Ready?
6           AD21715: pin 197 SPIDS; maybe nSS for the DSP?
8     MISO  TC2210 pin 124 (GPIO2); resistor is on AD21715 & TC2210 output
10    SCK   TC2210 pin 98; gated by B253 (SN74AHCT1G125DBVT): pin A
12          74125 pin G3, A3, G4 + G1
14    MOSI  TC2210 pin 97; gated by B253 (SN74AHCT1G125DBVT): pin A
16          74125 pin A2
18          AD21715 pin 202 (nRESET)
20    nSS   TC2210 pin 123 (nSS)
22          TC2210 pin 108 (GPIO8); maybe Ready?
24    nIRQ  TC2210 pin  79 (GPIO5)
26          *NC*
===== ===== ============================================

*The odd pin numbers are connected to GND*