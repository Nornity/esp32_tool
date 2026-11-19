#!/usr/bin/env python3
"""A small, friendly serial helper for ESP32 development boards."""
from __future__ import annotations

import argparse
import codecs
import sys
import time
from typing import Sequence


def _serial_modules():
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        raise RuntimeError(
            "pyserial is required for serial commands. Install it with: "
            "python -m pip install pyserial"
        ) from None
    return serial, list_ports


def list_ports() -> int:
    try:
        _, port_tools = _serial_modules()
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 2

    ports = sorted(port_tools.comports(), key=lambda item: item.device)
    if not ports:
        print("No serial ports found. Connect your ESP32 and try again.")
        return 0

    for port in ports:
        details = port.description or "Serial device"
        if port.vid is not None and port.pid is not None:
            details += f" (VID:PID {port.vid:04X}:{port.pid:04X})"
        print(f"{port.device:<16} {details}")
    return 0


def _open_port(args):
    serial, _ = _serial_modules()
    port = args.port
    if not port:
        _, port_tools = _serial_modules()
        ports = list(port_tools.comports())
        if len(ports) == 1:
            port = ports[0].device
            print(f"Using {port}")
        elif not ports:
            raise RuntimeError("No serial ports found. Connect your ESP32 first.")
        else:
            choices = ", ".join(item.device for item in ports)
            raise RuntimeError(f"Several ports found ({choices}); specify --port.")
    try:
        return serial.Serial(port, args.baud, timeout=0.2, write_timeout=2)
    except serial.SerialException as exc:
        raise RuntimeError(f"Could not open {port}: {exc}") from None


def monitor(args) -> int:
    try:
        connection = _open_port(args)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 2

    print(f"Listening on {connection.port} at {args.baud} baud. Press Ctrl+C to stop.")
    try:
        with connection:
            while True:
                line = connection.readline()
                if line:
                    text = line.decode(args.encoding, errors="replace").rstrip("\r\n")
                    if args.timestamps:
                        print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)
                    else:
                        print(text, flush=True)
    except KeyboardInterrupt:
        print("\nMonitor stopped.")
    except OSError as exc:
        print(f"Serial connection lost: {exc}", file=sys.stderr)
        return 1
    return 0


def send(args) -> int:
    try:
        connection = _open_port(args)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 2

    payload = args.message.encode(args.encoding)
    if args.newline:
        payload += b"\n"
    try:
        with connection:
            connection.write(payload)
            connection.flush()
    except (OSError, UnicodeError) as exc:
        print(f"Could not send data: {exc}", file=sys.stderr)
        return 1
    print(f"Sent {len(payload)} bytes to {connection.port}.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Quick serial monitor and command sender for ESP32 boards."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("ports", help="list available serial ports")

    monitor_parser = commands.add_parser("monitor", help="watch serial output")
    _add_connection_options(monitor_parser)
    monitor_parser.add_argument("--encoding", default="utf-8", help="text encoding (default: utf-8)")
    monitor_parser.add_argument("--timestamps", action="store_true", help="prefix received lines with local time")
    monitor_parser.set_defaults(func=monitor)

    send_parser = commands.add_parser("send", help="send a line or message over serial")
    _add_connection_options(send_parser)
    send_parser.add_argument("message", help="text to send")
    send_parser.add_argument("--encoding", default="utf-8", help="text encoding (default: utf-8)")
    send_parser.add_argument("--no-newline", dest="newline", action="store_false", help="do not append a line feed")
    send_parser.set_defaults(func=send, newline=True)
    return parser


def _add_connection_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("-p", "--port", help="serial port, e.g. COM3 or /dev/ttyUSB0 (auto-select if only one)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (default: 115200)")


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "ports":
        return list_ports()
    if args.baud <= 0:
        parser.error("--baud must be a positive integer")
    try:
        codecs.lookup(args.encoding)
    except LookupError:
        parser.error(f"unknown text encoding: {args.encoding}")
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
