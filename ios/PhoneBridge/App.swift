import SwiftUI
import CryptoKit

@main
struct PhoneBridgeApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

final class Bridge: ObservableObject {
    @AppStorage("bridgeURL") var urlText = "http://192.168.1.100:8765"
    @AppStorage("bridgeToken") var token = ""
    @Published var statusText = "未连接"
    @Published var boardOnline = false
    @Published var fw = "-"
    @Published var lastResult = ""

    private var guardHeader: String {
        SHA256.hash(data: Data(token.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    func refresh() async {
        guard let url = URL(string: urlText + "/status") else { statusText = "URL 无效"; return }
        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any] ?? [:]
            if let board = obj["board"] as? [String: Any], !board.isEmpty {
                boardOnline = true
                fw = board["fw"] as? String ?? "-"
                statusText = "板子在线"
            } else {
                boardOnline = false
                statusText = "板子离线"
            }
        } catch {
            boardOnline = false
            statusText = "连不上: \(error.localizedDescription.prefix(60))"
        }
    }

    func send(_ op: String, _ params: [String: Any] = [:]) async {
        guard let url = URL(string: urlText + "/cmd") else { return }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue(guardHeader, forHTTPHeaderField: "X-HID-Guard")
        var body: [String: Any] = ["op": op]
        body.merge(params) { _, new in new }
        req.httpBody = try? JSONSerialization.data(withJSONObject: body)
        do {
            let (data, resp) = try await URLSession.shared.data(for: req)
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            let text = String(data: data, encoding: .utf8) ?? ""
            await MainActor.run { lastResult = "HTTP \(code) \(text.prefix(120))" }
        } catch {
            await MainActor.run { lastResult = "错误: \(error.localizedDescription)" }
        }
    }
}

struct ContentView: View {
    @StateObject private var bridge = Bridge()
    @State private var xText = "540"
    @State private var yText = "1200"
    @State private var textInput = ""

    var body: some View {
        NavigationView {
            Form {
                Section("中转站") {
                    TextField("http://192.168.1.100:8765", text: $bridge.urlText)
                        .keyboardType(.URL).autocorrectionDisabled().textInputAutocapitalization(.never)
                    SecureField("Token（32 hex）", text: $bridge.token)
                        .autocorrectionDisabled().textInputAutocapitalization(.never)
                    HStack {
                        Text(bridge.statusText).foregroundStyle(bridge.boardOnline ? .green : .secondary)
                        Spacer()
                        Text("fw \(bridge.fw)").font(.caption).foregroundStyle(.secondary)
                        Button("刷新") { Task { await bridge.refresh() } }
                    }
                }
                Section("坐标点按（HID digitizer）") {
                    HStack {
                        TextField("x", text: $xText).keyboardType(.numberPad)
                        TextField("y", text: $yText).keyboardType(.numberPad)
                        Button("Tap") {
                            if let x = Int(xText), let y = Int(yText) {
                                Task { await bridge.send("tap", ["x": x, "y": y, "ms": 70]) }
                            }
                        }.buttonStyle(.borderedProminent)
                    }
                }
                Section("打字（仅 ASCII，走真蓝牙键盘）") {
                    TextField("hello", text: $textInput)
                        .autocorrectionDisabled().textInputAutocapitalization(.never)
                    HStack {
                        Button("Type") { Task { await bridge.send("type", ["text": String(textInput.prefix(200))]) } }
                            .disabled(textInput.isEmpty)
                        Button("回车") { Task { await bridge.send("key", ["key": "ENTER"]) } }
                    }
                }
                Section("按键") {
                    LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())]) {
                        ForEach([["VOLUME_UP", "音量+"], ["VOLUME_DOWN", "音量-"], ["MUTE", "静音"]], id: \.0) { pair in
                            Button(pair[1]) { Task { await bridge.send("key", ["key": pair[0]]) } }
                                .buttonStyle(.bordered)
                        }
                    }
                }
                Section {
                    Button("Ping 自检") { Task { await bridge.send("ping") } }
                }
                Section("最近响应") {
                    Text(bridge.lastResult.isEmpty ? "—" : bridge.lastResult)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
                Section {
                    Text("输入走 ESP32 真蓝牙键鼠（无注入痕迹）。中文输入、截图请走服务端 MCP/phone_agent。")
                        .font(.caption2).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("ESP32 HID")
            .task { await bridge.refresh() }
        }
    }
}
