#!/bin/bash
# 一键启动本地测试后台。
# 按固件规则算出上报地址（本机网段 + .96，第三段为 2 时端口 92 否则 90），
# 临时给网卡挂上该地址，起服务；Ctrl+C 退出时自动撤销别名。
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
ADDED=0

if [ "$(id -u)" -ne 0 ]; then
  echo "需要 root：挂 IP 别名和绑定 90 端口都要管理员权限" >&2
  echo "请改用： sudo $0 $*" >&2
  exit 1
fi

IFACE="${IFACE:-$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')}"
if [ -z "$IFACE" ]; then
  echo "找不到默认网卡，先确认已连上 WiFi" >&2
  exit 1
fi

LOCAL_IP="$(ipconfig getifaddr "$IFACE" 2>/dev/null || true)"
if [ -z "$LOCAL_IP" ]; then
  echo "拿不到 ${IFACE} 的 IP，先确认已连上 WiFi" >&2
  exit 1
fi

IFS=. read -r A B C _ <<<"$LOCAL_IP"
TARGET_IP="$A.$B.$C.96"
PORT=90
[ "$C" -eq 2 ] && PORT=92

cleanup() {
  if [ "$ADDED" = "1" ]; then
    ifconfig "$IFACE" -alias "$TARGET_IP" 2>/dev/null || true
    echo "已撤销别名 ${TARGET_IP}"
  fi
  if [ -n "${SUDO_USER:-}" ] && [ -f "$DIR/records.jsonl" ]; then
    chown "$SUDO_USER" "$DIR/records.jsonl" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if ifconfig "$IFACE" | awk '/inet /{print $2}' | grep -qFx "$TARGET_IP"; then
  echo "本机已持有 ${TARGET_IP}，跳过挂别名"
elif ping -c 1 -W 800 "$TARGET_IP" >/dev/null 2>&1; then
  echo "中止：${TARGET_IP} 已被局域网内其他设备占用。" >&2
  echo "很可能是真正的后台服务器在线，抢这个地址会造成 IP 冲突。" >&2
  exit 1
else
  # 同网段的别名必须用 /32：用 255.255.255.0 会让内核把这个自有地址当成远端主机去
  # ARP，本机访问它会报 no route to host。
  ifconfig "$IFACE" alias "$TARGET_IP" 255.255.255.255
  ADDED=1
  echo "已挂上临时别名 ${TARGET_IP}（退出时自动撤销）"
fi

echo "网卡 ${IFACE}   本机 ${LOCAL_IP}"
echo "网页 http://$TARGET_IP:$PORT/"
echo
python3 "$DIR/server.py" --port "$PORT" "$@"
