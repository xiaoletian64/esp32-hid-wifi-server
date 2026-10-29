import SwiftUI
import CryptoKit

// MARK: - Models

struct HidBoard: Decodable {
    var fw: String?
    var connected: Bool?
}

struct HidStatus: Decodable {
    var board: HidBoard?
}

struct FacadeStatus: Decodable {
    var bridge: String?
    var screen: FacadeScreen?
}

struct FacadeScreen: Decodable {
    var w: Int?
    var h: Int?
}

struct UIElement: Identifiable {
    var text: String?
    var bounds: [Double]?
    var clickable: Bool?
    var id: Int = 0
}

extension UIElement: Decodable {
    private enum CodingKeys: String, CodingKey { case text, bounds, clickable }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        text = try c.decodeIfPresent(String.self, forKey: .text)
        bounds = try c.decodeIfPresent([Double].self, forKey: .bounds)
        clickable = try c.decodeIfPresent(Bool.self, forKey: .clickable)
        id = 0
    }
}

struct LogEntry: Identifiable {
    let id = UUID()
    let time: Date
    let text: String
}

// MARK: - Network client

final class Bridge: ObservableObject {
    @Published var connected = false
    @Published var hidOnline = false
    @Published var fw = "-"
    @Published var screenW = 0
    @Published var screenH = 0
    @Published var logs: [LogEntry] = []

    @AppStorage("baseURL") var baseURL: String = "https://jiujiu.zhangyue.site"
    @AppStorage("token") var token: String = ""

    static let timeout: TimeInterval = 10

    private var guardHeader: String {
        let d = Data(token.utf8)
        return SHA256.hash(data: d).map { String(format: "%02x", $0) }.joined()
    }

    private func cleanBase() -> String {
        var b = baseURL.trimmingCharacters(in: .whitespacesAndNewlines)
        while b.hasSuffix("/") { b.removeLast() }
        return b
    }

    func log(_ s: String) {
        let entry = LogEntry(time: Date(), text: s)
        DispatchQueue.main.async {
            self.logs.insert(entry, at: 0)
            if self.logs.count > 60 { self.logs.removeLast(self.logs.count - 60) }
        }
    }

    // Generic request; returns raw Data (2xx enforced).
    func request(_ method: String, _ path: String, body: [String: Any]? = nil) async throws -> Data {
        var req = URLRequest(url: URL(string: cleanBase() + path)!, timeoutInterval: Self.timeout)
        req.httpMethod = method
        req.setValue(guardHeader, forHTTPHeaderField: "X-HID-Guard")
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        if let body = body {
            req.httpBody = try JSONSerialization.data(withJSONObject: body)
        }
        let (data, resp) = try await URLSession.shared.data(for: req)
        guard let http = resp as? HTTPURLResponse else { throw URLError(.badServerResponse) }
        guard (200..<300).contains(http.statusCode) else {
            throw NSError(domain: "bridge", code: http.statusCode,
                          userInfo: [NSLocalizedDescriptionKey: "HTTP \(http.statusCode) \(path)"])
        }
        return data
    }

    func json<T: Decodable>(_ method: String, _ path: String, body: [String: Any]? = nil) async throws -> T {
        let data = try await request(method, path, body: body)
        return try JSONDecoder().decode(T.self, from: data)
    }

    // Command that expects 2xx; response body ignored (tolerates empty/non-JSON).
    func send(_ path: String, _ body: [String: Any]) async -> Bool {
        do {
            _ = try await request("POST", path, body: body)
            log("OK \(body["op"] ?? path)")
            return true
        } catch {
            log("ERR \(error.localizedDescription)")
            return false
        }
    }

    func refreshStatus() async {
        hidOnline = false
        connected = false
        do {
            let hs = try JSONDecoder().decode(HidStatus.self, from: try await request("GET", "/hid/status"))
            hidOnline = hs.board != nil
            fw = hs.board?.fw ?? "-"
        } catch { fw = "-"; log("hid/status: \(error.localizedDescription)") }
        do {
            let fs: FacadeStatus = try await json("GET", "/x/status")
            screenW = fs.screen?.w ?? 0
            screenH = fs.screen?.h ?? 0
            connected = true
        } catch {
            connected = false
            log("x/status: \(error.localizedDescription)")
        }
    }

    func hid(_ op: String, extra: [String: Any] = [:]) async -> Bool {
        var body: [String: Any] = ["op": op]
        for (k, v) in extra { body[k] = v }
        return await send("/hid/cmd", body)
    }

    func facadeKey(_ key: String) async -> Bool {
        await send("/x/hid", ["op": "key", "key": key])
    }
}

// MARK: - App

@main
struct PhoneBridgeApp: App {
    var body: some Scene {
        WindowGroup { RootView() }
    }
}

struct RootView: View {
    @StateObject var bridge = Bridge()

    var body: some View {
        VStack(spacing: 0) {
            ConnectSection(bridge: bridge)
            TabView {
                ScreenTab(bridge: bridge)
                    .tabItem { Label("屏幕", systemImage: "iphone") }
                InputTab(bridge: bridge)
                    .tabItem { Label("输入", systemImage: "keyboard") }
                TapTab(bridge: bridge)
                    .tabItem { Label("点按", systemImage: "hand.tap") }
            }
            LogSection(bridge: bridge)
        }
        .background(Color(red: 0.04, green: 0.04, blue: 0.04))
        .preferredColorScheme(.dark)
        .task { await bridge.refreshStatus() }
    }
}

// MARK: - Connection section

struct ConnectSection: View {
    @ObservedObject var bridge: Bridge

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            TextField("Base URL", text: Binding(get: { bridge.baseURL }, set: { bridge.baseURL = $0 }))
                .textFieldStyle(.plain)
                .keyboardType(.URL)
                .autocapitalization(.none)
                .disableAutocorrection(true)
                .font(.system(size: 13, design: .monospaced))
                .padding(8)
                .background(Color.white.opacity(0.08))
                .cornerRadius(8)
            SecureField("Token", text: Binding(get: { bridge.token }, set: { bridge.token = $0 }))
                .textFieldStyle(.plain)
                .autocapitalization(.none)
                .disableAutocorrection(true)
                .font(.system(size: 13, design: .monospaced))
                .padding(8)
                .background(Color.white.opacity(0.08))
                .cornerRadius(8)
            HStack(spacing: 12) {
                Circle().fill(bridge.connected ? Color.green : Color.red).frame(width: 9, height: 9)
                Text(bridge.connected ? "已连接" : "未连接").font(.caption)
                Circle().fill(bridge.hidOnline ? Color.green : Color.gray).frame(width: 9, height: 9)
                Text("HID \(bridge.hidOnline ? "fw \(bridge.fw)" : "离线")").font(.caption)
                if bridge.screenW > 0 {
                    Text("\(bridge.screenW)×\(bridge.screenH)").font(.caption).foregroundColor(.gray)
                }
                Spacer()
                Button {
                    Task { await bridge.refreshStatus() }
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
            }
            .foregroundColor(.white)
        }
        .padding(.horizontal)
        .padding(.vertical, 8)
    }
}

// MARK: - Log section

struct LogSection: View {
    @ObservedObject var bridge: Bridge

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text("日志").font(.caption2).foregroundColor(.gray)
                Spacer()
                Button("清空") { bridge.logs.removeAll() }.font(.caption2)
            }
            ScrollView {
                ForEach(bridge.logs) { e in
                    Text(e.text)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.gray)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .frame(height: 90)
        }
        .padding(.horizontal)
        .padding(.bottom, 6)
    }
}

// MARK: - Screen tab

struct ScreenTab: View {
    @ObservedObject var bridge: Bridge
    @State var shot: UIImage?
    @State var apps: [String] = []
    @State var showApps = false
    @State var showUI = false
    @State var uiElements: [UIElement] = []
    @State var loading = false
    @State var swipeStart: CGPoint?

    var body: some View {
        VStack(spacing: 8) {
            if let img = shot {
                GeometryReader { geo in
                    ZStack {
                        Image(uiImage: img)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .frame(width: geo.size.width, height: geo.size.height)
                            .contentShape(Rectangle())
                            .gesture(dragGesture(geo: geo, img: img))
                    }
                    .frame(width: geo.size.width, height: geo.size.height)
                }
            } else {
                Spacer()
                Text(loading ? "加载中…" : "无截图").foregroundColor(.gray)
                Spacer()
            }
            HStack(spacing: 10) {
                pill("刷新") { Task { await loadShot() } }
                pill("应用") { showApps = true; Task { await loadApps() } }
                pill("元素") { Task { await loadUI(); showUI = true } }
                pill("HOME") { Task { _ = await bridge.facadeKey("HOME") } }
                pill("BACK") { Task { _ = await bridge.facadeKey("BACK") } }
            }
            .padding(.horizontal)
            .padding(.bottom, 4)
        }
        .refreshable { await loadShot() }
        .task { await loadShot() }
        .sheet(isPresented: $showApps) { appList }
        .sheet(isPresented: $showUI) { uiList }
    }

    @ViewBuilder private var appList: some View {
        NavigationView {
            List(apps, id: \.self) { pkg in
                Button(pkg) {
                    showApps = false
                    Task { _ = await bridge.send("/x/launch", ["package": pkg]) }
                }
            }
            .navigationTitle("启动应用")
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("关闭") { showApps = false } } }
        }
    }

    @ViewBuilder private var uiList: some View {
        NavigationView {
            List(uiElements.indices.map { $0 }, id: \.self) { i in
                let e = uiElements[i]
                Button {
                    showUI = false
                    Task { _ = await bridge.send("/x/tap-text", ["text": e.text ?? ""]) }
                } label: {
                    HStack {
                        Text(e.text ?? "(no text)").lineLimit(1)
                        Spacer()
                        if e.clickable == true { Text("可点").font(.caption2).foregroundColor(.gray) }
                    }
                }
            }
            .navigationTitle("UI 元素")
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("关闭") { showUI = false } } }
        }
    }

    private func pill(_ label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(.black)
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .background(Color.white)
                .cornerRadius(16)
        }
    }

    // Convert a point in the displayed image to real screen coordinates.
    private func mapPoint(_ p: CGPoint, geo: GeometryProxy, img: UIImage) -> CGPoint? {
        guard bridge.screenW > 0, img.size.width > 0 else { return nil }
        let scale = min(geo.size.width / img.size.width, geo.size.height / img.size.height)
        let dw = img.size.width * scale, dh = img.size.height * scale
        let ox = (geo.size.width - dw) / 2, oy = (geo.size.height - dh) / 2
        let ix = (p.x - ox) / dw * CGFloat(bridge.screenW)
        let iy = (p.y - oy) / dh * CGFloat(bridge.screenH)
        guard ix >= 0, iy >= 0, ix <= CGFloat(bridge.screenW), iy <= CGFloat(bridge.screenH) else { return nil }
        return CGPoint(x: ix, y: iy)
    }

    private func tapGesture(geo: GeometryProxy, img: UIImage) -> some Gesture {
        TapGesture().onEnded { _ in
            // TapGesture has no location; handled via drag below for taps too.
        }
    }

    private func dragGesture(geo: GeometryProxy, img: UIImage) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { v in
                if swipeStart == nil { swipeStart = v.startLocation }
            }
            .onEnded { v in
                guard let s = swipeStart else { return }
                swipeStart = nil
                guard let p1 = mapPoint(s, geo: geo, img: img), let p2 = mapPoint(v.location, geo: geo, img: img) else { return }
                let dist = hypot(p2.x - p1.x, p2.y - p1.y)
                Task {
                    if dist < 8 {
                        _ = await bridge.hid("tap", extra: ["x": Int(p1.x), "y": Int(p1.y)])
                    } else {
                        _ = await bridge.hid("swipe", extra: ["x1": Int(p1.x), "y1": Int(p1.y), "x2": Int(p2.x), "y2": Int(p2.y), "ms": 300])
                    }
                }
            }
    }

    private func loadShot() async {
        loading = true
        defer { loading = false }
        do {
            var req = URLRequest(url: URL(string: bridge.baseURL.trimmingCharacters(in: CharacterSet(charactersIn: "/ ")) + "/x/screenshot")!, timeoutInterval: 10)
            req.setValue(bridge.guardHeader, forHTTPHeaderField: "X-HID-Guard")
            let (data, _) = try await URLSession.shared.data(for: req)
            if let img = UIImage(data: data) { shot = img } else { bridge.log("screenshot: 数据不是 PNG") }
        } catch {
            bridge.log("screenshot: \(error.localizedDescription)")
        }
    }

    private func loadApps() async {
        do {
            let data = try await bridge.request("GET", "/x/apps")
            apps = (try? JSONDecoder().decode([String].self, from: data)) ?? []
        }
        catch { bridge.log("apps: \(error.localizedDescription)") }
    }

    private func loadUI() async {
        do {
            let data = try await bridge.request("GET", "/x/ui")
            let list = (try? JSONDecoder().decode([UIElement].self, from: data)) ?? []
            var out: [UIElement] = []
            for (i, e) in list.enumerated() {
                var m = e
                m.id = i
                out.append(m)
            }
            uiElements = out
        } catch { bridge.log("ui: \(error.localizedDescription)") }
    }
}

// MARK: - Input tab

struct InputTab: View {
    @ObservedObject var bridge: Bridge
    @State var text = ""
    @State var mod1 = "none"
    @State var mod2 = "none"
    @State var keyName = "a"

    let mods = ["none", "ctrl", "alt", "shift", "gui"]
    let keys = ["a", "b", "c", "tab", "esc", "space", "enter", "up", "down", "left", "right", "f1", "f2", "f3", "f4", "f5"]

    var body: some View {
        ScrollView {
            VStack(spacing: 12) {
                TextField("要输入的文本", text: $text)
                    .textFieldStyle(.roundedBorder)
                    .autocapitalization(.none)
                    .disableAutocorrection(true)
                HStack(spacing: 10) {
                    pill("Type") {
                        let t = text
                        Task { _ = await bridge.hid("type", extra: ["text": t]) }
                    }
                    pill("回车") { Task { _ = await bridge.hid("key", extra: ["key": "enter"]) } }
                }
                Divider().background(Color.gray.opacity(0.4))
                HStack {
                    Picker("", selection: $mod1) { ForEach(mods, id: \.self) { Text($0) } }
                    Picker("", selection: $keyName) { ForEach(keys, id: \.self) { Text($0) } }
                    Picker("", selection: $mod2) { ForEach(mods, id: \.self) { Text($0) } }
                }
                .pickerStyle(.menu)
                pill("Combo") {
                    var parts: [String] = []
                    if mod1 != "none" { parts.append(mod1) }
                    if mod2 != "none" { parts.append(mod2) }
                    parts.append(keyName)
                    let combo = parts.joined(separator: "+")
                    Task { _ = await bridge.hid("combo", extra: ["keys": combo]) }
                }
                Divider().background(Color.gray.opacity(0.4))
                HStack(spacing: 10) {
                    pill("音量+") { Task { _ = await bridge.hid("key", extra: ["key": "volume_up"]) } }
                    pill("音量−") { Task { _ = await bridge.hid("key", extra: ["key": "volume_down"]) } }
                    pill("静音") { Task { _ = await bridge.hid("key", extra: ["key": "mute"]) } }
                }
                Divider().background(Color.gray.opacity(0.4))
                HStack(spacing: 16) {
                    Button {
                        Task { _ = await bridge.hid("scroll", extra: ["dy": -3]) }
                    } label: {
                        Text("▲ 上滚").font(.title2).frame(maxWidth: .infinity).padding(14)
                            .background(Color.white).foregroundColor(.black).cornerRadius(12)
                    }
                    Button {
                        Task { _ = await bridge.hid("scroll", extra: ["dy": 3]) }
                    } label: {
                        Text("▼ 下滚").font(.title2).frame(maxWidth: .infinity).padding(14)
                            .background(Color.white).foregroundColor(.black).cornerRadius(12)
                    }
                }
            }
            .padding()
        }
    }

    private func pill(_ label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(.black)
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                .background(Color.white)
                .cornerRadius(16)
        }
    }
}

// MARK: - Tap tab (manual coordinates)

struct TapTab: View {
    @ObservedObject var bridge: Bridge
    @State var x = ""
    @State var y = ""
    @State var x1 = ""
    @State var y1 = ""
    @State var x2 = ""
    @State var y2 = ""

    var body: some View {
        ScrollView {
            VStack(spacing: 12) {
                HStack {
                    Text("X").foregroundColor(.gray)
                    TextField("x", text: $x).keyboardType(.numberPad).textFieldStyle(.roundedBorder)
                    Text("Y").foregroundColor(.gray)
                    TextField("y", text: $y).keyboardType(.numberPad).textFieldStyle(.roundedBorder)
                    pill("Tap") { Task { _ = await bridge.hid("tap", extra: ["x": Int(x) ?? 0, "y": Int(y) ?? 0]) } }
                    pill("长按") { Task { _ = await bridge.hid("tap", extra: ["x": Int(x) ?? 0, "y": Int(y) ?? 0, "ms": 600]) } }
                }
                Divider().background(Color.gray.opacity(0.4))
                HStack {
                    coordField($x1, "x1"); coordField($y1, "y1")
                    coordField($x2, "x2"); coordField($y2, "y2")
                    pill("Swipe") {
                        Task {
                            _ = await bridge.hid("swipe", extra: ["x1": Int(x1) ?? 0, "y1": Int(y1) ?? 0, "x2": Int(x2) ?? 0, "y2": Int(y2) ?? 0, "ms": 300])
                        }
                    }
                }
            }
            .padding()
        }
    }

    private func coordField(_ binding: Binding<String>, _ ph: String) -> some View {
        TextField(ph, text: binding)
            .keyboardType(.numberPad)
            .textFieldStyle(.roundedBorder)
            .frame(width: 58)
    }

    private func pill(_ label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(.black)
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                .background(Color.white)
                .cornerRadius(16)
        }
    }
}
