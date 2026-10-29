#!/data/data/com.termux/files/usr/bin/bash
# phone-tunnel.sh — 手机反向隧道一键脚本（把手机的 adb 无线调试端口暴露到云端）
#
# 用法（Termux 里）：
#   bash <(curl -sL https://raw.githubusercontent.com/xiaoletian64/esp32-hid-wifi-server/main/tools/phone-tunnel.sh)
#
# 前提：手机已开「开发者选项 → 无线调试」（记下端口，默认尝试 5555）
# 效果：云端（106）上 `adb connect 127.0.0.1:16666` 即可控制本手机，任何网络都通。
#
set -u
PW_FILE="$HOME/.phonegw_pass"
PORT_LOCAL="${PHONE_ADB_PORT:-5555}"
REMOTE_PORT="${REMOTE_PORT:-16666}"
HOST="${TUNNEL_HOST:-106.52.123.149}"
USER_="${TUNNEL_USER:-phonegw}"

echo "== 手机反向隧道（phonegw@$HOST）=="
command -v ssh >/dev/null || { echo "安装 openssh..."; pkg install -y openssh; }
command -v sshpass >/dev/null || pkg install -y sshpass
command -v termux-usb >/dev/null || pkg install -y termux-api

if [ ! -f "$PW_FILE" ]; then
  echo -n "输入 phonegw 密码: "; read -r PW
  printf "%s" "$PW" > "$PW_FILE"; chmod 600 "$PW_FILE"
fi

while true; do
  # 手机自身的无线 ADB 端口（无线调试开关页显示的端口，5555 是 tcpip 模式端口）
  echo "[$(date +%T)] 连接 $HOST，反向隧道 127.0.0.1:$REMOTE_PORT -> 手机:$PORT_LOCAL"
  sshpass -f "$PW_FILE" ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=15 \
    -o ExitOnForwardFailure=yes -N \
    -R "127.0.0.1:$REMOTE_PORT:127.0.0.1:$PORT_LOCAL" "$USER_@$HOST"
  echo "[$(date +%T)] 断了，5 秒后重连"
  sleep 5
done
