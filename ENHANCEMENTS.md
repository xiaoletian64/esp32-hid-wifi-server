# 增强版说明（本仓库相对上游的增量）

上游：[4xf00/esp32-hid-wifi-server](https://github.com/4xf00/esp32-hid-wifi-server)（MIT）。本仓库在其基础上增加以下能力，全部保持"服务端能要求的永远少于固件愿意发的"安全模型：

## 1. OTA 无线升级（烧一次，永不再插线）
- **分区表 `scripts/partitions_ota.csv`**：factory + ota_1 双分区（4MB flash，每槽 1.94MB）。首启走上游实测过的 factory 路径，OTA 升级写 ota_1；factory 槽永远是回滚镜像，重刷 0x10000 即回。
- **固件新增 `ota` op**：服务端下发 `{"op":"ota","url":"...","md5":"..."}`，板子 HTTP 拉取（`x-MD5` 响应头自动校验完整性）、写 ota_1、重启。**双层 URL 白名单**：服务端校验 `--ota-prefix` 前缀，固件校验 `config.h` 的 `OTA_URL_PREFIX` 前缀——中转站被攻破也无法把板子刷成别的包。
- **`server/ota_httpd.py`**：零依赖固件分发服务（`/latest.bin` 带 x-MD5 头、`/version.json`、`/health`）。
- **发版一条命令**：`PATH=<toolchain>:$PATH bash scripts/build_firmware.sh build-ota s3` 编译 → 拷贝 latest.bin → 生成 version.json。
- 板子侧升级指令（agent 发）：
  ```json
  {"op":"ota","url":"http://your-server:8766/latest.bin","md5":"32位hex"}
  ```

## 2. ESP32-S3 支持（双芯片）
- `build_firmware.sh build-ota [c3|s3]` / `flash-ota [c3|s3]`：S3（双核 LX7，含原生 USB-OTG 的板型）与 C3 共用同一张分区表，产物分目录。
- S3 已实测编译通过（1.4MB / 2MB 槽）；S3 没有 C3 core 2.0.17 的 rst:0x3 bootloader 坑。

## 3. MCP 适配层（`mcp/`）—— mobile-mcp 同名工具 + 物理手
`mcp/mcp_server.py`：标准 MCP (stdio) 服务器，工具名与 [mobile-next/mobile-mcp](https://github.com/mobile-next/mobile-mcp) 一致（14 个），但：
- **输入类**（tap/双击/长按/滑动/ASCII 打字/组合键/音量键）→ 转发到本项目中转站 → ESP32 真蓝牙键鼠，无注入痕迹
- **感知类**（截图/界面树/分辨率/应用列表）→ 只读 adb
- 中文/emoji 自动降级 adb input 并在结果注明（BLE HID 物理限制）；BACK/HOME 系统导航键走 adb keyevent（物理键盘本没有）

```bash
pip install "mcp<2"
BRIDGE_URL=http://127.0.0.1:8765 BRIDGE_TOKEN_FILE=~/.config/esp32-hid-bridge/token \
  python3 mcp/mcp_server.py   # 接入 Claude Code / Codex / 任何 MCP 客户端
```

## 4. /phone 任务流（`mcp/phone_agent.py`）—— 授权式多步执行
plan（生成任务号）→ confirm（用户授权落盘）→ run（板子在线检查 → launch → uiautomator 读树找目标 → HID 点击 → 截图核验）/ status / stop / list。任务与审计落盘 `PHONE_TASKS_DIR`。
安全特性：未 confirm 不执行；**板子离线拒绝盲执行**；**界面树找不到目标文本就诚实失败，绝不盲点**（含零坐标文本节点的父容器坐标解析——uiautomator 全零 bounds 是常态）。

## 5. 上游兼容性
- 上游的 `build` / `upload` / `upload-flash` / `size` / `install-lib` 命令与安全策略**原样保留**，本仓库自测脚本 `run_tests.sh` 覆盖 OTA op 的拒绝路径（前缀不符 / md5 非 hex / 未启用）。
- 上游 README（下文）描述的 7 个 op、安全模型、排查手册全部适用，新增第 8 个 op 即 `ota`。

## License
MIT（同上游）。`mcp/` 部分同为 MIT。
