#!/usr/bin/env python3
"""物料盒扫码上报 —— 本地测试后台

复刻固件上报的目标接口，并提供网页实时查看扫码记录。
仅供局域网内联调使用：没有任何认证，任何能访问本机的设备都可读写记录。
"""

import argparse
import json
import re
import threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

SCAN_PATH = "/api/externalinterface/addMaterialBoxScanningRecord"
BASE_DIR = Path(__file__).resolve().parent
LOG_FILE = BASE_DIR / "records.jsonl"

_lock = threading.Lock()
_records = []
_reply_data = 3.0


def parse_qrcode(body):
    """返回 (qrcode, 解析方式)。

    固件用字符串拼接生成 JSON 且不转义引号/反斜杠，条码含这些字符时 body 是非法
    JSON，所以 json 解析失败后退回正则，避免真机联调时丢记录。
    """
    try:
        obj = json.loads(body)
        if isinstance(obj, dict) and "qrcode" in obj:
            return str(obj["qrcode"]), "json"
    except (json.JSONDecodeError, ValueError):
        pass
    m = re.search(r'"qrcode"\s*:\s*"(.*)"\s*\}\s*$', body, re.S)
    if m:
        return m.group(1), "regex"
    return None, "failed"


def add_record(client_ip, body, reply):
    qrcode, how = parse_qrcode(body)
    rec = {
        "seq": 0,
        "time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "ip": client_ip,
        "qrcode": qrcode,
        "parse": how,
        "raw": body,
        "reply": reply,
        "alarm": 0 <= reply < 1,
    }
    with _lock:
        rec["seq"] = len(_records) + 1
        _records.append(rec)
    try:
        with LOG_FILE.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except OSError as exc:
        print(f"  ! 写入 {LOG_FILE.name} 失败: {exc}", flush=True)
    return rec


def load_existing():
    if not LOG_FILE.exists():
        return
    for line in LOG_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            _records.append(json.loads(line))
        except ValueError:
            continue
    for i, rec in enumerate(_records, 1):
        rec["seq"] = i
    if _records:
        print(f"已载入 {len(_records)} 条历史记录（{LOG_FILE.name}）")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MaterialBoxMock/1.0"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, code, obj):
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def _read_body(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n <= 0:
            return ""
        return self.rfile.read(n).decode("utf-8", errors="replace")

    def do_POST(self):
        path = urlsplit(self.path).path
        body = self._read_body()
        if path == SCAN_PATH:
            self._handle_scan(body)
        elif path == "/api/reply":
            self._handle_set_reply(body)
        elif path == "/api/clear":
            self._handle_clear()
        else:
            self._json(404, {"code": 404, "msg": "not found"})

    def _handle_scan(self, body):
        with _lock:
            reply = _reply_data
        rec = add_record(self.client_address[0], body, reply)
        flag = "长鸣报警" if rec["alarm"] else "正常"
        note = "" if rec["parse"] == "json" else f"  [{rec['parse']} 兜底]"
        print(
            f"[{rec['time']}] #{rec['seq']} {rec['ip']}  "
            f"qrcode={rec['qrcode']!r}  -> data={reply} ({flag}){note}",
            flush=True,
        )
        self._json(200, {"code": 0, "msg": "ok", "data": reply})

    def _handle_set_reply(self, body):
        global _reply_data
        try:
            value = float(json.loads(body)["data"])
        except (ValueError, KeyError, TypeError):
            self._json(400, {"code": 400, "msg": "需要 {\"data\": 数字}"})
            return
        with _lock:
            _reply_data = value
        print(f"→ 返回值改为 data={value}"
              f"（{'长鸣报警' if 0 <= value < 1 else '正常'}）", flush=True)
        self._json(200, {"code": 0, "reply": value})

    def _handle_clear(self):
        with _lock:
            _records.clear()
        try:
            LOG_FILE.unlink(missing_ok=True)
        except OSError as exc:
            print(f"  ! 删除 {LOG_FILE.name} 失败: {exc}", flush=True)
        print("→ 已清空记录", flush=True)
        self._json(200, {"code": 0, "total": 0})

    def do_GET(self):
        parts = urlsplit(self.path)
        path = parts.path
        if path in ("/", "/index.html"):
            try:
                html = (BASE_DIR / "index.html").read_bytes()
            except OSError:
                self._send(500, b"index.html missing", "text/plain; charset=utf-8")
                return
            self._send(200, html, "text/html; charset=utf-8")
        elif path == "/api/records":
            try:
                since = int(parse_qs(parts.query).get("since", ["0"])[0] or 0)
            except ValueError:
                since = 0
            with _lock:
                fresh = [r for r in _records if r["seq"] > since]
                total = len(_records)
                reply = _reply_data
            self._json(200, {"records": fresh, "total": total, "reply": reply})
        else:
            self._json(404, {"code": 404, "msg": "not found"})


def main():
    ap = argparse.ArgumentParser(description="物料盒扫码上报本地测试后台")
    ap.add_argument("--host", default="0.0.0.0", help="监听地址，默认所有网卡")
    ap.add_argument("--port", type=int, default=90, help="监听端口，固件默认访问 90")
    ap.add_argument("--data", type=float, default=3.0,
                    help="返回给设备的 data 值；落在 [0,1) 会让设备长鸣报警，默认 3")
    args = ap.parse_args()

    global _reply_data
    _reply_data = args.data
    load_existing()

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.daemon_threads = True
    print(f"扫码上报接口  POST http://<本机IP>:{args.port}{SCAN_PATH}")
    print(f"网页查看      http://127.0.0.1:{args.port}/")
    print(f"当前返回      data={args.data}"
          f"（{'长鸣报警' if 0 <= args.data < 1 else '正常'}）")
    print("无认证，仅限可信局域网。Ctrl+C 退出。\n")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已退出")
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
