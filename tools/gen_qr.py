#!/usr/bin/env python3
"""生成 ESP8285 扫码终端的配网二维码，内容格式 SSID+密码+沿模式。"""

import argparse
import sys

import segno

MAX_SSID_BYTES = 32
MAX_PASS_BYTES = 64


def validate(ssid, password):
    errors = []
    warnings = []

    if not ssid:
        errors.append("SSID 不能为空")
    if not password:
        errors.append("密码不能为空")
    if "+" in ssid:
        errors.append("SSID 不能含加号，固件按前两个加号切分，会切错")
    if "+" in password:
        errors.append("密码不能含加号，固件按前两个加号切分，会切错")

    ssid_bytes = len(ssid.encode("utf-8"))
    pass_bytes = len(password.encode("utf-8"))
    if ssid_bytes > MAX_SSID_BYTES:
        errors.append(f"SSID 占 {ssid_bytes} 字节，超过 {MAX_SSID_BYTES}，写 EEPROM 时会被截断")
    if pass_bytes > MAX_PASS_BYTES:
        errors.append(f"密码占 {pass_bytes} 字节，超过 {MAX_PASS_BYTES}，写 EEPROM 时会被截断")

    if not ssid.isascii() or not password.isascii():
        warnings.append("含非 ASCII 字符，多数扫码模块的默认输出编码处理不了，建议先实测")

    return errors, warnings


def main():
    parser = argparse.ArgumentParser(
        description="生成 ESP8285 配网二维码，内容格式 SSID+密码+沿模式")
    parser.add_argument("ssid", help="WiFi 名称")
    parser.add_argument("password", help="WiFi 密码")
    parser.add_argument("mode", nargs="?", default="0", choices=["0", "1"],
                        help="GPIO5 中断沿，0=下降沿（默认），1=上升沿")
    parser.add_argument("-o", "--out", help="输出文件名前缀，默认 配网码_<SSID>")
    parser.add_argument("--scale", type=int, default=12, help="每个模块的像素数，默认 12")
    args = parser.parse_args()

    errors, warnings = validate(args.ssid, args.password)
    for text in warnings:
        print("警告:", text, file=sys.stderr)
    if errors:
        for text in errors:
            print("错误:", text, file=sys.stderr)
        return 1

    data = f"{args.ssid}+{args.password}+{args.mode}"
    qr = segno.make(data, error="h")
    prefix = args.out or f"配网码_{args.ssid}"
    qr.save(f"{prefix}.png", scale=args.scale, border=4)
    qr.save(f"{prefix}.svg", scale=args.scale, border=4)

    modules = qr.symbol_size(scale=1, border=0)[0]
    print(f"内容     : {data}")
    print(f"纠错等级 : H   版本: {qr.version}   模块: {modules}x{modules}")
    print(f"已生成   : {prefix}.png  {prefix}.svg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
