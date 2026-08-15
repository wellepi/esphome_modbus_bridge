import argparse
import logging

from pymodbus.client import ModbusTcpClient, ModbusSerialClient

logging.basicConfig(
    format="%(asctime)s %(levelname)s: %(message)s",
    level=logging.DEBUG,
)


def build_client(args):
    # Implicit selection:
    # - TCP if --host is provided
    # - RTU if --serial-port is provided
    if args.host:
        return ModbusTcpClient(
            host=args.host,
            port=args.tcp_port,
            timeout=args.timeout,
        )

    return ModbusSerialClient(
        port=args.serial_port,
        framer="rtu",
        baudrate=args.baudrate,
        parity=args.parity,
        stopbits=args.stopbits,
        bytesize=args.bytesize,
        timeout=args.timeout,
    )


def main():
    parser = argparse.ArgumentParser(
        description="Modbus TCP or RTU Register Read/Write (PyModbus 3.10+)"
    )

    # Implicit mode inputs
    parser.add_argument(
        "--host",
        help="Modbus TCP Host (if set → TCP mode)",
    )
    parser.add_argument(
        "--tcp-port",
        type=int,
        default=502,
        help="Modbus TCP Port (default: 502)",
    )

    parser.add_argument(
        "--serial-port",
        help="Serial port (if set → RTU mode), e.g. COM3 or /dev/ttyUSB0",
    )
    parser.add_argument(
        "--baudrate",
        type=int,
        default=9600,
        help="RTU baudrate (default: 9600)",
    )
    parser.add_argument(
        "--parity",
        default="N",
        choices=["N", "E", "O"],
        help="RTU parity (default: N)",
    )
    parser.add_argument(
        "--stopbits",
        type=int,
        default=1,
        choices=[1, 2],
        help="RTU stopbits (default: 1)",
    )
    parser.add_argument(
        "--bytesize",
        type=int,
        default=8,
        choices=[7, 8],
        help="RTU bytesize (default: 8)",
    )

    # Common
    parser.add_argument(
        "--timeout",
        type=float,
        default=1.0,
        help="Timeout seconds (default: 1.0)",
    )
    parser.add_argument(
        "--unit",
        default=1,
        type=int,
        help="Modbus Device/Unit ID (default: 1)",
    )
    parser.add_argument(
        "--register",
        required=True,
        type=lambda x: int(x, 0),
        help="Register address (decimal or hex)",
    )
    parser.add_argument(
        "--value",
        type=lambda x: int(x, 0),
        help="Value to write (decimal or hex)",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=1,
        help="Number of registers to read",
    )
    parser.add_argument(
        "--read",
        action="store_true",
        help="Read Holding Registers (FC 0x03)",
    )
    parser.add_argument(
        "--read_input",
        action="store_true",
        help="Read Input Registers (FC 0x04)",
    )

    args = parser.parse_args()

    # Validation of implicit mode selection
    if bool(args.host) == bool(args.serial_port):
        parser.error(
            "Specify exactly one of --host (TCP) or --serial-port (RTU)."
        )

    if args.value is None and not (args.read or args.read_input):
        parser.error(
            "Specify either --value (write) or --read / --read_input (read)."
        )

    if args.value is not None and (args.read or args.read_input):
        parser.error(
            "Specify either --value (write) or a read operation, not both."
        )

    if args.read and args.read_input:
        parser.error(
            "Specify only one of --read or --read_input."
        )

    if args.count < 1:
        parser.error("--count must be >= 1.")

    if not 0 <= args.unit <= 247:
        parser.error("--unit must be between 0 and 247.")

    if not 0 <= args.register <= 0xFFFF:
        parser.error("--register must be between 0 and 65535.")

    if args.value is not None and not 0 <= args.value <= 0xFFFF:
        parser.error("--value must be between 0 and 65535.")

    client = build_client(args)

    if not client.connect():
        if args.host:
            print(
                f"Could not connect to Modbus TCP server "
                f"at {args.host}:{args.tcp_port}"
            )
        else:
            print(
                f"Could not open serial port {args.serial_port}"
            )
        return

    try:
        if args.value is not None:
            print(
                f"Writing Register (FC 0x06), "
                f"Addr={args.register}, "
                f"Value={args.value}, "
                f"Device ID={args.unit}"
            )

            result = client.write_register(
                address=args.register,
                value=args.value,
                device_id=args.unit,
            )

            if result.isError():
                print(f"Error writing register: {result}")
            else:
                logging.debug(
                    "Write response: %s",
                    result,
                )
                print("Write successful")

        else:
            fc_str = (
                "Input Registers (FC 0x04)"
                if args.read_input
                else "Holding Registers (FC 0x03)"
            )

            print(
                f"Reading {fc_str}, "
                f"Addr={args.register}, "
                f"Count={args.count}, "
                f"Device ID={args.unit}"
            )

            if args.read_input:
                result = client.read_input_registers(
                    address=args.register,
                    count=args.count,
                    device_id=args.unit,
                )
            else:
                result = client.read_holding_registers(
                    address=args.register,
                    count=args.count,
                    device_id=args.unit,
                )

            if result.isError():
                print(
                    f"Error reading register: {result}"
                )
            else:
                logging.debug(
                    "Raw register data: %s",
                    result.registers,
                )

                byte_data = b"".join(
                    register.to_bytes(
                        2,
                        byteorder="big",
                    )
                    for register in result.registers
                )

                raw_hex = " ".join(
                    f"{byte:02X}"
                    for byte in byte_data
                )

                print(
                    f"Data ({len(result.registers)} registers):\n"
                    f"{raw_hex}"
                )

    finally:
        client.close()


if __name__ == "__main__":
    main()
