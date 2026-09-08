"""Choose the physical USB-UART port for ESP8266 uploads on Windows."""

from serial.tools import list_ports


# Keep normal builds usable without hardware connected.
if "upload" not in COMMAND_LINE_TARGETS:
    Return()


def _is_usb_uart(port):
    """Return true for USB serial adapters, not Bluetooth virtual ports."""
    hwid = (port.hwid or "").upper()
    description = (port.description or "").upper()
    return (
        hwid.startswith("USB")
        or "CH340" in description
        or "USB-SERIAL" in description
        or "CP210" in description
        or "FTDI" in description
    )


ports = [port for port in list_ports.comports() if _is_usb_uart(port)]

if len(ports) == 1:
    env["UPLOAD_PORT"] = ports[0].device
    print("Using USB-UART upload port: {} ({})".format(ports[0].device, ports[0].description))
elif len(ports) > 1:
    names = ", ".join(port.device for port in ports)
    raise RuntimeError(
        "Multiple USB-UART adapters found ({}). "
        "Set upload_port explicitly in platformio.ini or unplug the extra adapter.".format(names)
    )
else:
    raise RuntimeError(
        "No USB-UART adapter found. Reconnect the ESP8285 USB cable, "
        "install the CH340 driver, and check that a USB serial port appears."
    )
