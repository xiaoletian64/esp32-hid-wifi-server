#!/usr/bin/env python3
"""
phone_agent.py — /phone 命令执行器（s3-phone-control 新链路后端）

流程（与 openclaw 微信入口配合）：
  openclaw: phone_agent.py plan --task "打开微信找到李洋" --app com.tencent.mm --target 李洋
            → 生成任务号，回给用户等确认
  用户:     /phone 确认 <task_id>
  openclaw: phone_agent.py confirm <id> && phone_agent.py run <id>
            → 执行多步（板子在线检查 → launch → 读界面树 → 找目标点击 → 截图核验）→ 汇报
  用户:     /phone 状态 <id>  /phone 停止 <id>

安全设计：
  - 未 confirm 的任务绝不执行；run 前强制检查 ESP32 板子在线（board_connected）
  - 每步动作写审计 jsonl；截图存档可回溯
  - 找不到目标就诚实失败，绝不盲点
"""
import argparse
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import time
import urllib.request

TASKS_DIR = os.environ.get("PHONE_TASKS_DIR", "/var/lib/esp32-phone-tasks")
AUDIT = os.path.join(TASKS_DIR, "audit.jsonl")
ADB = os.environ.get("ADB_BIN") or shutil.which("adb") or "adb"
SERIAL = os.environ.get("ANDROID_SERIAL", "127.0.0.1:15555")
BRIDGE = os.environ.get("BRIDGE_URL", "http://127.0.0.1:8765").rstrip("/")
TOKEN = open(os.environ.get("BRIDGE_TOKEN_FILE", "/root/.config/esp32-hid-bridge/token")).read().strip()
GUARD = __import__("hashlib").sha256(TOKEN.encode()).hexdigest()

os.makedirs(TASKS_DIR, exist_ok=True)


def audit(event, **kw):
    row = {"ts": int(time.time() * 1000), "event": event, **kw}
    with open(AUDIT, "a") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")


def adb(*args, binary=False, timeout=25):
    p = subprocess.run([ADB, "-s", SERIAL, *args], capture_output=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError(f"adb 失败: {p.stderr.decode(errors='replace')[:150]}")
    return p.stdout if binary else p.stdout.decode(errors="replace")


def hid(op, **params):
    body = json.dumps({"op": op, **params}).encode()
    req = urllib.request.Request(f"{BRIDGE}/cmd", data=body, method="POST",
                                 headers={"Content-Type": "application/json", "X-HID-Guard": GUARD})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"HID 拒绝: HTTP {e.code} {e.read().decode()[:100]}")


def board_online():
    try:
        with urllib.request.urlopen(f"{BRIDGE}/status", timeout=5) as r:
            b = json.loads(r.read()).get("board", {})
            return bool(b.get("peer") or b.get("fw"))  # 在线时 board 带 peer/fw，离线是 {}
    except Exception:
        return False


def tpath(tid):
    return os.path.join(TASKS_DIR, f"{tid}.json")


def load(tid):
    if not os.path.isfile(tpath(tid)):
        raise SystemExit(f"任务不存在: {tid}")
    return json.load(open(tpath(tid)))


def save(t):
    json.dump(t, open(tpath(t["id"]), "w"), ensure_ascii=False, indent=1)


def read_elements():
    """uiautomator dump → 节点列表（保持 XML 出现顺序，父在前子在后）。
    文本节点 bounds 全零是常态（redroid/部分 ROM），可点区域在父容器，由 find_target 处理。"""
    adb("shell", "uiautomator", "dump", "/sdcard/uidump.xml", timeout=30)
    xml = adb("shell", "cat", "/sdcard/uidump.xml")
    adb("shell", "rm", "/sdcard/uidump.xml")
    els = []
    for m in re.finditer(r"<node[^>]*>", xml):
        tag = m.group(0)
        def a(name):
            mm = re.search(name + r'="([^"]*)"', tag)
            return mm.group(1) if mm else ""
        text = a("text") or a("content-desc")
        bm = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", a("bounds"))
        if not bm:
            continue
        x, y, x2, y2 = map(int, bm.groups())
        els.append({"text": text, "rid": a("resource-id"), "clickable": a("clickable") == "true",
                    "bounds_zero": (x2 - x <= 0 or y2 - y <= 0),
                    "center": [x + (x2 - x) // 2, y + (y2 - y) // 2]})
    return els


def find_target(els, target):
    """按文本找目标。命中零 bounds 的文本节点时，向前找最近的可点击父容器
    （uiautomator XML 父标签出现在子标签之前）。"""
    tl = target.lower()
    for i, e in enumerate(els):
        if tl not in (e["text"] or "").lower():
            continue
        if not e["bounds_zero"]:
            return e
        for j in range(i - 1, -1, -1):
            p = els[j]
            if p["clickable"] and not p["bounds_zero"]:
                return {**p, "text": e["text"], "via_parent": True}
        # 没有可点击祖先就找前面最近一个有真实面积的
        for j in range(i - 1, -1, -1):
            if not els[j]["bounds_zero"]:
                return {**els[j], "text": e["text"], "via_parent": True}
    return None


def cmd_plan(args):
    tid = secrets.token_hex(4)
    t = {"id": tid, "task": args.task, "app": args.app or "", "target": args.target or "",
         "status": "pending", "created": int(time.time()),
         "plan": [
             f"1. 启动 {args.app or '(当前应用)'}",
             f"2. 读取界面树，找「{args.target or '(无指定目标)'}」",
             "3. 经 ESP32 HID 点击目标（真蓝牙键鼠路径）",
             "4. 截图核验并汇报",
         ]}
    save(t)
    audit("plan", id=tid, task=args.task)
    print(json.dumps({"ok": True, "task_id": tid, "status": "pending", "plan": t["plan"],
                      "hint": f"等待确认：/phone 确认 {tid}"}, ensure_ascii=False))


def cmd_confirm(args):
    t = load(args.task_id)
    if t["status"] != "pending":
        raise SystemExit(f"状态不对: {t['status']}（只有 pending 可确认）")
    t["status"] = "confirmed"
    t["confirmed_at"] = int(time.time())
    save(t)
    audit("confirm", id=t["id"])
    print(json.dumps({"ok": True, "task_id": t["id"], "status": "confirmed",
                      "hint": "执行：phone_agent.py run " + t["id"]}, ensure_ascii=False))


def cmd_run(args):
    t = load(args.task_id)
    if t["status"] not in ("confirmed", "running"):
        raise SystemExit(f"状态不对: {t['status']}（需先 confirm）")
    if not board_online():
        t["status"] = "confirmed"
        save(t)
        raise SystemExit("ESP32 板子不在线（board_connected=false）——拒绝盲执行。检查板子电源/WiFi/中转站。")
    t["status"] = "running"
    save(t)
    audit("run_start", id=t["id"])
    steps, ok = [], True
    try:
        # 1. 启动应用
        if t["app"]:
            out = adb("shell", "cmd", "package", "resolve-activity", "--brief", t["app"], timeout=15).strip().splitlines()[-1]
            if "/" in out:
                adb("shell", "am", "start", "-n", out.strip())
                steps.append(f"launch {t['app']} ok ({out.strip()})")
                time.sleep(2.5)
            else:
                raise RuntimeError(f"解析不到 Activity: {out[:60]}")
        # 2-3. 读树找目标 → HID 点击
        if t["target"]:
            els = read_elements()
            hit = find_target(els, t["target"])
            audit("elements", id=t["id"], count=len(els), hit=bool(hit))
            if not hit:
                raise RuntimeError(f"界面树里没有「{t['target']}」（{len(els)} 个带文本元素）——不盲点")
            x, y = hit["center"]
            hid("tap", x=min(x, 3999), y=min(y, 3999), ms=70)
            steps.append(f"HID tap ({x},{y}) on「{hit['text'][:20]}」")
            time.sleep(2)
        # 4. 截图核验
        shot = os.path.join(TASKS_DIR, f"{t['id']}-final.png")
        open(shot, "wb").write(adb("exec-out", "screencap", "-p", binary=True))
        steps.append(f"核验截图: {shot}")
        t["status"], t["result"] = "done", {"ok": True, "steps": steps, "screenshot": shot}
        audit("run_done", id=t["id"], steps=steps)
    except Exception as e:
        t["status"], t["result"] = "failed", {"ok": False, "error": str(e), "steps": steps}
        audit("run_failed", id=t["id"], error=str(e))
        ok = False
    save(t)
    print(json.dumps({"ok": ok, "task_id": t["id"], "status": t["status"], "result": t["result"]},
                     ensure_ascii=False))


def cmd_status(args):
    t = load(args.task_id)
    print(json.dumps(t, ensure_ascii=False))


def cmd_stop(args):
    t = load(args.task_id)
    if t["status"] in ("pending", "confirmed"):
        t["status"] = "stopped"
        save(t)
    print(json.dumps({"ok": True, "task_id": t["id"], "status": t["status"]}, ensure_ascii=False))


def cmd_list(args):
    rows = []
    for f in sorted(os.listdir(TASKS_DIR)):
        if f.endswith(".json"):
            t = json.load(open(os.path.join(TASKS_DIR, f)))
            rows.append({"id": t["id"], "status": t["status"], "task": t["task"][:36]})
    print(json.dumps(rows, ensure_ascii=False))


p = argparse.ArgumentParser(description=__doc__.split("\n")[1])
sub = p.add_subparsers(dest="cmd", required=True)
sp = sub.add_parser("plan"); sp.add_argument("--task", required=True); sp.add_argument("--app"); sp.add_argument("--target"); sp.set_defaults(f=cmd_plan)
sc = sub.add_parser("confirm"); sc.add_argument("task_id"); sc.set_defaults(f=cmd_confirm)
sr = sub.add_parser("run"); sr.add_argument("task_id"); sr.set_defaults(f=cmd_run)
ss = sub.add_parser("status"); ss.add_argument("task_id"); ss.set_defaults(f=cmd_status)
sx = sub.add_parser("stop"); sx.add_argument("task_id"); sx.set_defaults(f=cmd_stop)
sl = sub.add_parser("list"); sl.set_defaults(f=cmd_list)

if __name__ == "__main__":
    a = p.parse_args()
    a.f(a)
