#!/usr/bin/env python3
import argparse


def parse_args():
    p = argparse.ArgumentParser(
        description="Send raw L2 frames for my_ice rx-listen testing."
    )
    p.add_argument(
        "-i",
        "--iface",
        required=True,
        help="Sender interface (for example: enp23s0f0)",
    )
    p.add_argument(
        "-d",
        "--dst-mac",
        required=True,
        help="Destination MAC (my_ice receiver MAC)",
    )
    p.add_argument(
        "-s",
        "--src-mac",
        default=None,
        help="Optional source MAC (default: interface MAC)",
    )
    p.add_argument(
        "-e",
        "--ethertype",
        default="0x88B5",
        help="Ethertype (default: 0x88B5)",
    )
    p.add_argument(
        "-p",
        "--payload",
        default="hello-my-ice",
        help="Payload string (default: hello-my-ice)",
    )
    p.add_argument(
        "-l",
        "--packet-size",
        type=int,
        default=64,
        help=(
            "Exact Ethernet frame size on the wire in bytes, including FCS "
            "(default: 64). The script pads payload as needed; the NIC adds FCS."
        ),
    )
    p.add_argument(
        "-c",
        "--count",
        type=int,
        default=20,
        help="Number of frames (default: 20)",
    )
    p.add_argument(
        "-t",
        "--interval",
        type=float,
        default=0.1,
        help="Inter-packet interval in seconds (default: 0.1)",
    )
    return p.parse_args()


def main():
    args = parse_args()

    try:
        from scapy.all import Ether, Raw, sendp, get_if_hwaddr
    except ImportError as exc:
        raise SystemExit("scapy is required to run send_l2.py") from exc

    src = args.src_mac if args.src_mac else get_if_hwaddr(args.iface)
    etype = int(args.ethertype, 0)
    if args.packet_size < 64:
        raise SystemExit("--packet-size must be at least 64 bytes on the wire")

    payload = args.payload.encode()
    target_l2_len = args.packet_size - 4
    target_payload_len = max(0, target_l2_len - 14)
    if len(payload) > target_payload_len:
        raise SystemExit(
            "--payload is too large for --packet-size {} (max payload {} bytes)".format(
                args.packet_size, target_payload_len
            )
        )
    if len(payload) < target_payload_len:
        payload += b"\x00" * (target_payload_len - len(payload))

    pkt = Ether(dst=args.dst_mac, src=src, type=etype) / Raw(payload)

    sendp(pkt, iface=args.iface, count=args.count, inter=args.interval, verbose=False)
    print(
        (
            "sent count={} iface={} src={} dst={} ethertype=0x{:04x} "
            "payload_len={} l2_bytes={} wire_bytes={} payload='{}'"
        ).format(
            args.count,
            args.iface,
            src,
            args.dst_mac,
            etype,
            len(payload),
            len(pkt),
            len(pkt) + 4,
            args.payload,
        )
    )


if __name__ == "__main__":
    main()
