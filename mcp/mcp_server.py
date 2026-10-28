#!/usr/bin/env python3
"""
esp32-hid-mcp — mobile-mcp 风格的 MCP 服务器，但"手"走 ESP32 HID，不走 adb input。

设计（与 4xf00/esp32-hid-wifi-server 的双通道理念一致）：
  - 输入类工具（tap/swipe/type/长按/组合键/音量键）→ 转发到 ESP32 HID 中转站
    （手机看到的是真蓝牙键鼠，与物理输入同驱动路径，无注入痕迹）
  - 感知类工具（截图/界面树/分辨率/应用列表）→ 只读 adb（纯观察，无输入事件）
  - 工具名与 mobile-next/mobile-mcp 保持一致，agent 侧零迁移成本

特例（如实告知用户的降级）：
  - 中文/emoji：BLE HID 物理上打不出（HID 报键位不报字符），自动降级 adb input text
    并在结果中注明 —— 安全键盘等敏感场景请勿用降级路径
  - BACK/HOME 系统导航键：物理键盘本没有这两个键，走 adb keyevent；
    ENTER/音量/媒体键走 HID

环境变量：
  BRIDGE_URL   默认 http://127.0.0.1:8765（中转站与本工具同机时的默认）
  BRIDGE_TOKEN 或 BRIDGE_TOKEN_FILE
  ANDROID_SERIAL  adb 目标设备（默认 127.0.0.1:15555）
  ADB_BIN         默认 adb（PATH 查找），显式路径如 /opt/android-sdk/platform-tools/adb
"""
import base64
import hashlib
import hmac
import json
import os
import re
import shutil
import subprocess
import urllib.request

from mcp.server.fastmcp import FastMCP, Image

BRIDGE_URL = os.environ.get("BRIDGE_URL", "http://127.0.0.1:8765").rstrip("/")
ADB = os.environ.get("ADB_BIN") or shutil.which("adb") or "adb"
SERIAL = os.environ.get("ANDROID_SERIAL", "127.0.0.1:15555")

TOKEN = os.environ.get("BRIDGE_TOKEN", "")
if not TOKEN and os.environ.get("BRIDGE_TOKEN_FILE"):
    TOKEN = open(os.environ["BRIDGE_TOKEN_FILE"]).read().strip()
GUARD = hashlib.sha256(TOKEN.encode()).hexdigest() if TOKEN else ""

mcp = FastMCP("esp32-hid")


# ---------------------------------------------------------------- HID 转发 ----

def hid(op: str, **params) -> dict:
    """把命令转发给 ESP32 HID 中转站。返回 {"ok":True,...} 或抛 RuntimeError。"""
    body = json.dumps({"op": op, **params}).encode()
    req = urllib.request.Request(
        f"{BRIDGE_URL}/cmd", data=body, method="POST",
        headers={"Content-Type": "application/json", "X-HID-Guard": GUARD},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"HID 中转站拒绝: HTTP {e.code} {e.read().decode()[:120]}") from None


def hid_status() -> dict:
    with urllib.request.urlopen(f"{BRIDGE_URL}/status", timeout=5) as r:
        return json.loads(r.read())


# ---------------------------------------------------------------- adb 封装 ----

def adb(*args, binary=False, timeout=20):
    cmd = [ADB, "-s", SERIAL, *args]
    p = subprocess.run(cmd, capture_output=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args[:2])} 失败: {p.stderr.decode(errors='replace')[:200]}")
    return p.stdout if binary else p.stdout.decode(errors="replace")


def screen_size() -> tuple[int, int]:
    out = adb("shell", "wm", "size")
    m = re.search(r"(\d+)x(\d+)", out.split("Override")[-1])
    if not m:
        raise RuntimeError(f"读不到分辨率: {out[:80]}")
    return int(m.group(1)), int(m.group(2))


def ui_elements() -> list[dict]:
    """uiautomator dump → 精简元素列表（mobile-mcp 的 list_elements 风格）。"""
    adb("shell", "uiautomator", "dump", "/sdcard/uidump.xml", timeout=30)
    xml = adb("shell", "cat", "/sdcard/uidump.xml")
    adb("shell", "rm", "/sdcard/uidump.xml")
    out = []
    for m in re.finditer(r"<node[^>]*>", xml):
        tag = m.group(0)
        text = _attr(tag, "text") or _attr(tag, "content-desc")
        bounds = _attr(tag, "bounds")
        if not text and not _attr(tag, "resource-id"):
            continue
        bm = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", bounds or "")
        el = {
            "text": text or "",
            "resource-id": _attr(tag, "resource-id") or "",
            "class": _attr(tag, "class") or "",
            "clickable": _attr(tag, "clickable") == "true",
        }
        if bm:
            x, y, x2, y2 = map(int, bm.groups())
            el["rect"] = {"x": x, "y": y, "w": x2 - x, "h": y2 - y}
            el["center"] = [x + (x2 - x) // 2, y + (y2 - y) // 2]
        out.append(el)
    return out


def _attr(tag: str, name: str):
    m = re.search(f'{name}="([^"]*)"', tag)
    return m.group(1) if m else None


# ---------------------------------------------------------------- 工具定义 ----

@mcp.tool()
def hid_bridge_status() -> dict:
    """ESP32 HID 中转站与板子状态：板子在不在线、固件版本、BLE 是否连着手机。
    动手类工具前先看这里，板子离线时输入类工具都会失败。"""
    return hid_status()


@mcp.tool()
def mobile_list_available_devices() -> dict:
    """列出可用设备：adb 目标 + HID 板子在线状态。"""
    out = adb("devices")
    devs = [l.split()[0] for l in out.splitlines() if l.strip() and "List of" not in l and l.strip().split()[1] == "device"]
    return {"adb_devices": devs, "hid_bridge": hid_status()}


@mcp.tool()
def mobile_get_screen_size() -> dict:
    """获取手机屏幕分辨率（像素）。坐标点按前先看这个。"""
    w, h = screen_size()
    return {"width": w, "height": h}


@mcp.tool()
def mobile_take_screenshot(query: str = "") -> Image:
    """截取手机屏幕（只读，走 adb，无输入事件）。query 写你想在屏幕上找什么。"""
    return Image(data=adb("exec-out", "screencap", "-p", binary=True), format="png")


@mcp.tool()
def mobile_list_elements_on_screen() -> dict:
    """读取当前屏幕的 UI 元素（文本/坐标中心点/可点击性，只读）。
    优先用这个拿按钮坐标，再调坐标点按工具。"""
    els = ui_elements()
    return {"count": len(els), "elements": els[:60]}


@mcp.tool()
def mobile_click_on_screen_at_coordinates(x: int, y: int, ms: int = 70) -> dict:
    """在坐标处点击 —— 走 ESP32 真蓝牙键鼠（digitizer 绝对坐标），无注入痕迹。
    坐标来自截图或 list_elements。"""
    r = hid("tap", x=x, y=y, ms=max(20, min(1000, ms)))
    return {"ok": True, "via": "esp32-hid", "bridge": r}


@mcp.tool()
def mobile_double_tap_on_screen(x: int, y: int) -> dict:
    """双击坐标（两次 HID 点按，间隔 90ms）。"""
    hid("tap", x=x, y=y, ms=40)
    import time; time.sleep(0.09)
    hid("tap", x=x, y=y, ms=40)
    return {"ok": True, "via": "esp32-hid"}


@mcp.tool()
def mobile_long_press_on_screen_at_coordinates(x: int, y: int, ms: int = 800) -> dict:
    """长按坐标（HID 按住 ms 毫秒后抬起，20-1000ms）。长按只有 HID 做得到，
    adb input 没有按住语义。"""
    r = hid("tap", x=x, y=y, ms=max(20, min(1000, ms)))
    return {"ok": True, "via": "esp32-hid", "hold_ms": ms}


@mcp.tool()
def mobile_swipe_on_screen(direction: str, distance_ratio: float = 0.5) -> dict:
    """朝 up/down/left/right 滑动屏幕（HID 线性手势，更像人）。
    distance_ratio 滑动距离占屏幕短边的比例，0.2-0.9。"""
    w, h = screen_size()
    cx, cy = w // 2, h // 2
    d = int(min(w, h) * max(0.2, min(0.9, distance_ratio)))
    moves = {"up": (cx, cy + d // 2, cx, cy - d // 2), "down": (cx, cy - d // 2, cx, cy + d // 2),
             "left": (cx + d // 2, cy, cx - d // 2, cy), "right": (cx - d // 2, cy, cx + d // 2, cy)}
    x1, y1, x2, y2 = moves[direction]
    hid("swipe", x1=x1, y1=y1, x2=x2, y2=y2, ms=350)
    return {"ok": True, "via": "esp32-hid", "from": [x1, y1], "to": [x2, y2]}


@mcp.tool()
def mobile_type_keys(text: str, submit: bool = False) -> dict:
    """打字 —— 走 ESP32 真蓝牙键盘。仅 ASCII（BLE HID 物理限制：报键位不报字符）。
    含中文/emoji 时自动降级 adb input text 并在结果注明（有注入痕迹，敏感场景勿用）。
    submit=True 时回车提交（HID）。"""
    ascii_ok = all(32 <= ord(c) < 127 for c in text)
    if ascii_ok:
        if len(text) > 200:
            text = text[:200]
        hid("type", text=text)
        if submit:
            hid("key", key="ENTER")
        return {"ok": True, "via": "esp32-hid", "chars": len(text)}
    adb("shell", "input", "text", text.replace(" ", "%s").replace("'", ""))
    if submit:
        adb("shell", "input", "keyevent", "66")
    return {"ok": True, "via": "adb-fallback（非 ASCII 走了输入注入，安全键盘场景勿用）"}


@mcp.tool()
def mobile_press_button(button: str) -> dict:
    """按设备按键。VOLUME_UP/DOWN/MUTE 走 HID 媒体键（真路径）；
    ENTER 走 HID；BACK/HOME 是系统导航键（物理键盘本没有），走 adb keyevent。"""
    b = button.upper()
    if b in ("VOLUME_UP", "VOLUME_DOWN", "MUTE"):
        hid("key", key=b)
        return {"ok": True, "via": "esp32-hid"}
    if b == "ENTER":
        hid("key", key="ENTER")
        return {"ok": True, "via": "esp32-hid"}
    codes = {"BACK": 4, "HOME": 3, "RECENT": 187, "POWER": 26}
    if b not in codes:
        raise RuntimeError(f"不支持的按键: {button}（支持 VOLUME_UP/DOWN/MUTE/ENTER/BACK/HOME/RECENT/POWER）")
    adb("shell", "input", "keyevent", str(codes[b]))
    return {"ok": True, "via": "adb（系统导航键，无 HID 物理等价）"}


@mcp.tool()
def mobile_combo(keys: list[str]) -> dict:
    """组合键（必须含 CTRL/ALT/SHIFT + 一个普通键），走 HID。如 ["CTRL","A"]。"""
    r = hid("combo", keys=keys[:2])
    return {"ok": True, "via": "esp32-hid", "bridge": r}


@mcp.tool()
def mobile_list_apps() -> dict:
    """列出已安装应用包名（只读 adb）。"""
    out = adb("shell", "pm", "list", "packages", "-3")
    return {"packages": [l.replace("package:", "").strip() for l in out.splitlines() if l.strip()]}


@mcp.tool()
def mobile_launch_app(package: str) -> dict:
    """启动应用（resolve-activity + am start，只读性质地拉起 Activity）。"""
    out = adb("shell", "cmd", "package", "resolve-activity", "--brief", package, timeout=15)
    comp = out.strip().splitlines()[-1].strip() if out.strip() else ""
    if "/" not in comp:
        raise RuntimeError(f"解析不到启动 Activity: {out[:80]}")
    adb("shell", "am", "start", "-n", comp)
    return {"ok": True, "package": package, "activity": comp}


if __name__ == "__main__":
    mcp.run()  # stdio
