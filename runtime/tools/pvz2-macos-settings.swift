// The Mac app's settings window (Cmd-,): a sidebar and grouped sections, the
// way macOS 26 draws its own settings. The values live in the app's own
// UserDefaults under the keys the host reads (pvz2-macos.m), and the host
// watches them, so a change acts at once -- there is no "save".
//
//   General   the window: Fixed (one shape) or Adapt, the shape, and what closing it does
//   Keyboard  the key for each seed packet, the shovel and collecting (click a field, press a key)
//   About     the version, updates, and where the saves and the log are
//
// The host opens it with [NSClassFromString(@"PVZ2Settings") show].

import AppKit
import SwiftUI

// MARK: - The keys (the host's own names; see pvz2-macos.m)

private enum Key {
    static let resizeMode = "resizeMode"
    static let fixedAspect = "fixedAspect"
    static let slots = (1...8).map { "seedSlot\($0)" }
    static let shovel = "shovelKey"
    static let defaultSlots = (1...8).map { "\($0)" }
    static let defaultShovel = "S"
    static let collect = "collectSunKey"
    static let defaultCollect = "Space"
    /// Every shortcut and its default, for the one-key-one-shortcut rule.
    static let all = zip(slots + [shovel, collect], defaultSlots + [defaultShovel, defaultCollect]).map { ($0, $1) }
    static let closeAction = "closeAction"
    static let confirmQuit = "confirmQuit"
    static let renderSharp = "renderSharp"
    /// Sparkle's own key; the host hands a change to the updater
    static let automaticChecks = "SUEnableAutomaticChecks"
}

/// A key's name, the way the host matches it (pvz2-macos.m, key_name): "Space",
/// "Tab", the arrows, "F1".."F12", or the key's own character without Shift, in
/// capitals. nil for a key that cannot be a shortcut.
enum KeyName {
    static func of(_ e: NSEvent) -> String? {
        switch e.keyCode {
        case 49: return "Space"
        case 48: return "Tab"
        case 123: return "Left"
        case 124: return "Right"
        case 125: return "Down"
        case 126: return "Up"
        case 122: return "F1"
        case 120: return "F2"
        case 99: return "F3"
        case 118: return "F4"
        case 96: return "F5"
        case 97: return "F6"
        case 98: return "F7"
        case 100: return "F8"
        case 101: return "F9"
        case 109: return "F10"
        case 103: return "F11"
        case 111: return "F12"
        case 36, 76, 53, 51, 117: return nil       // Return, Enter, Escape, Delete
        default: break
        }
        let c = e.modifierFlags.contains(.shift) ? e.characters(byApplyingModifiers: []) : e.charactersIgnoringModifiers
        guard let c, c.count == 1,
              let u = c.unicodeScalars.first?.value,
              u > 0x20, u != 0x7f, !(0xF700...0xF8FF).contains(u) else { return nil }
        return c.uppercased()
    }

    /// How a stored name is shown.
    static func display(_ name: String) -> String {
        switch name {
        case "": "None"
        case "Left": "←"
        case "Right": "→"
        case "Up": "↑"
        case "Down": "↓"
        case "Tab": "⇥ Tab"
        default: name
        }
    }
}

// MARK: - The window

@objc(PVZ2Settings)
@MainActor
final class PVZ2Settings: NSObject {
    @objc static func show() { PVZ2SettingsWindowController.show() }
    /// One pane by name ("general", "keyboard", "about"): a test's way in.
    @objc static func showTab(_ name: String) {
        PVZ2SettingsWindowController.show(tab: PVZ2SettingsTab(rawValue: name))
    }
}

enum PVZ2SettingsTab: String, CaseIterable, Identifiable {
    case general
    case keyboard
    case about

    var id: Self { self }
    var title: String {
        switch self {
        case .general: "General"
        case .keyboard: "Keyboard"
        case .about: "About"
        }
    }
    var systemImage: String {
        switch self {
        case .general: "macwindow"
        case .keyboard: "keyboard"
        case .about: "info.circle"
        }
    }
}

@MainActor
@Observable
final class PVZ2SettingsNavigation {
    static let shared = PVZ2SettingsNavigation()
    var selectedTab: PVZ2SettingsTab? = .general
    private init() {}
}

@MainActor
final class PVZ2SettingsWindowController: NSWindowController, NSWindowDelegate {
    private static var shared: PVZ2SettingsWindowController?

    static func show(tab: PVZ2SettingsTab? = nil) {
        if let tab { PVZ2SettingsNavigation.shared.selectedTab = tab }
        if shared == nil { shared = PVZ2SettingsWindowController() }
        shared?.showWindow(nil)
    }

    private init() {
        // .fullSizeContentView is what gives the window macOS 26's glass chrome;
        // it has to be there when the window is made.
        let window = NSWindow(
            contentRect: NSRect(origin: .zero, size: CGSize(width: 680, height: 520)),
            styleMask: [.titled, .closable, .resizable, .fullSizeContentView],   // a settings window is not minimized (macOS's own are not)
            backing: .buffered, defer: false)
        super.init(window: window)
        window.title = "Settings"
        window.titleVisibility = .visible
        window.toolbarStyle = .automatic
        window.isMovableByWindowBackground = true
        window.setFrameAutosaveName("PVZ2SettingsWindow")
        window.center()
        window.delegate = self
        let host = NSHostingController(rootView: PVZ2SettingsView())
        // By default the hosting controller makes the window's limits the view's
        // (no maximum at all) and overrides any set here; the window's own are used.
        host.sizingOptions = []
        window.contentViewController = host
        // The panes are forms: a wider window only stretches them.
        window.minSize = Self.minSize
        window.maxSize = Self.maxSize
        if window.frame.width > Self.maxSize.width || window.frame.height > Self.maxSize.height {
            window.setContentSize(NSSize(width: min(window.frame.width, Self.maxSize.width),
                                         height: min(window.frame.height, Self.maxSize.height)))
        }
    }

    private static let minSize = NSSize(width: 620, height: 460)
    private static let maxSize = NSSize(width: 860, height: 760)

    /// The limits again: SwiftUI's split view rewrites the window's own maxSize.
    func windowWillResize(_ sender: NSWindow, to frameSize: NSSize) -> NSSize {
        NSSize(width: min(max(frameSize.width, Self.minSize.width), Self.maxSize.width),
               height: min(max(frameSize.height, Self.minSize.height), Self.maxSize.height))
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func showWindow(_ sender: Any?) {
        super.showWindow(sender)
        window?.makeKeyAndOrderFront(nil)
        // A test that must not take the person's focus runs the app in the
        // background; the window is shown without activating it.
        let env = ProcessInfo.processInfo.environment
        let quiet = ["TL_PVZ2_QUIET", "TL_KEEP_RUNNING_IN_BACKGROUND", "TL_PVZ2_BACKGROUND"]
            .contains { (env[$0] ?? "0") != "0" && !(env[$0] ?? "").isEmpty }
        if !quiet { NSApp.activate(ignoringOtherApps: true) }
    }

    func windowWillClose(_ notification: Notification) { Self.shared = nil }
}

// MARK: - The root view

struct PVZ2SettingsView: View {
    @State private var navigation = PVZ2SettingsNavigation.shared

    var body: some View {
        NavigationSplitView(columnVisibility: .constant(.all)) {
            List(selection: $navigation.selectedTab) {
                ForEach(PVZ2SettingsTab.allCases) { tab in
                    Label(tab.title, systemImage: tab.systemImage).tag(tab)
                }
            }
            .listStyle(.sidebar)
            .scrollEdgeEffectStyleSoftIfAvailable()
            .navigationSplitViewColumnWidth(min: 180, ideal: 180, max: 180)
            .toolbar(removing: .sidebarToggle)
        } detail: {
            Group {
                switch navigation.selectedTab ?? .general {
                case .general: GeneralPane()
                case .keyboard: KeyboardPane()
                case .about: AboutPane()
                }
            }
            .navigationTitle((navigation.selectedTab ?? .general).title)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .navigationTitle("Settings")
        .navigationSplitViewStyle(.balanced)
        .frame(minWidth: 620, minHeight: 460)
    }
}

// MARK: - Shared pieces

/// A grouped form with a transparent background (the glass shows through).
private struct Pane<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        Form { content }
            .formStyle(.grouped)
            .scrollContentBackground(.hidden)
            .contentMargins(.top, 8, for: .scrollContent)
    }
}

/// A setting's name with a reset arrow beside it, shown while the pointer is
/// over the setting and its value is not the default.
private struct ResettableRow<Content: View>: View {
    let title: String
    let isDefault: Bool
    let reset: () -> Void
    @ViewBuilder var content: Content
    @State private var hovering = false
    var body: some View {
        LabeledContent {
            content
        } label: {
            HStack(spacing: 6) {
                Text(title)
                if hovering && !isDefault {
                    Button(action: reset) { Image(systemName: "arrow.counterclockwise") }
                        .buttonStyle(.borderless)
                        .foregroundStyle(.secondary)
                        .help("Reset to default")
                        .transition(.opacity)
                }
            }
        }
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.12), value: hovering)
    }
}

// MARK: - General

private struct GeneralPane: View {
    @AppStorage(Key.resizeMode) private var storedMode = "Fixed"
    @AppStorage(Key.fixedAspect) private var fixedAspect = "display"
    @AppStorage(Key.closeAction) private var closeAction = "quit"
    @AppStorage(Key.confirmQuit) private var confirmQuit = true
    @AppStorage(Key.renderSharp) private var renderSharp = false

    /// "Letterbox" is the old name of the fixed mode.
    private var mode: Binding<String> {
        Binding(get: { storedMode == "Adapt" ? "Adapt" : "Fixed" }, set: { storedMode = $0 })
    }

    var body: some View {
        Pane {
            Section {
                ResettableRow(title: "Mode", isDefault: mode.wrappedValue == "Fixed",
                              reset: { storedMode = "Fixed" }) {
                    Picker("", selection: mode) {
                        Text("Fixed").tag("Fixed")
                        Text("Adapt").tag("Adapt")
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .fixedSize()
                }
                if mode.wrappedValue == "Fixed" {
                    ResettableRow(title: "Aspect ratio", isDefault: fixedAspect == "display",
                                  reset: { fixedAspect = "display" }) {
                        Picker("", selection: $fixedAspect) {
                            Text("Current display (\(AspectName.of(Self.displayAspect())))").tag("display")
                            Text("The game's own (2.17:1)").tag("game")
                            Divider()
                            ForEach(AspectName.choices, id: \.tag) { Text($0.name).tag($0.tag) }
                        }
                        .pickerStyle(.menu)
                        .labelsHidden()
                        .fixedSize()
                    }
                }
            } header: {
                Text("Window")
            } footer: {
                Text(mode.wrappedValue == "Adapt"
                     ? "The window takes any shape from 4:3 to 2.4:1, and the game starts in the window's shape. The game lays a screen out once, so after you change the shape the picture keeps its old one, with bars: ↻ restarts the game in the new shape (it saves first), ✕ puts the window back. Experimental."
                     : "The window keeps one shape and the game fills it, at any size. A new shape takes effect when the game restarts: ↻ in the corner does that now (it saves first), ✕ keeps the current shape until the next start. “Current display” is the shape of the display the game is on, so full screen has no bars.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section {
                ResettableRow(title: "Closing the window", isDefault: closeAction == "quit",
                              reset: { closeAction = "quit" }) {
                    Picker("", selection: $closeAction) {
                        Text("Quits the game").tag("quit")
                        Text("Keeps the game paused").tag("pause")
                    }
                    .pickerStyle(.menu)
                    .labelsHidden()
                    .fixedSize()
                }
                ResettableRow(title: "Ask before quitting", isDefault: confirmQuit,
                              reset: { confirmQuit = true }) {
                    Toggle("", isOn: $confirmQuit)
                        .toggleStyle(.switch)
                        .labelsHidden()
                }
            } footer: {
                (Text(closeAction == "pause"
                     ? "The red button and ⌘W hide the window and pause the game, the way minimizing does. Click the Dock icon (or Window › PvZ2 Reflourished, ⌘0) to go on where you were. The paused game still uses its memory; ⌘Q quits."
                     : "The red button and ⌘W save and quit, like ⌘Q. To pause and keep your place instead, minimize (⌘M) or hide (⌘H).")
                    + Text(confirmQuit ? " Quitting asks first." : ""))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section {
                ResettableRow(title: "Sharper on standard displays", isDefault: !renderSharp,
                              reset: { renderSharp = false }) {
                    Toggle("", isOn: $renderSharp)
                        .toggleStyle(.switch)
                        .labelsHidden()
                }
                LabeledContent("Frame rate") {
                    Text("The display's own")
                        .foregroundStyle(.secondary)
                }
            } footer: {
                Text("The game draws at the sharpness of the display its window is on, and follows the window to another display. “Sharper on standard displays” draws at twice the size on a display that isn't Retina and scales it down: smoother edges, more work for the graphics card. With the game's “Target 60 FPS” on, the game runs at the refresh rate of the display its window is on (120 Hz, 175 Hz…); off, at 60.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Keyboard

private struct KeyboardPane: View {
    var body: some View {
        Pane {
            Section {
                ForEach(0..<8, id: \.self) { i in
                    KeyRow(label: "Seed packet \(i + 1)", key: Key.slots[i], fallback: Key.defaultSlots[i])
                }
            } header: {
                Text("Seed packets")
            } footer: {
                Text("A key takes that seed packet in hand; click the lawn to plant it. The same key again puts it back.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section {
                KeyRow(label: "Shovel", key: Key.shovel, fallback: Key.defaultShovel)
            } header: {
                Text("Shovel")
            } footer: {
                Text("The key picks up the shovel; click a plant to dig it up. The same key again puts it back.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section {
                KeyRow(label: "Collect sun and coins", key: Key.collect, fallback: Key.defaultCollect)
            } header: {
                Text("Collecting")
            } footer: {
                Text("Every sun and coin on the lawn flies to its counter, as if you had clicked each one.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section {
                Button("Restore the default keys") {
                    let d = UserDefaults.standard
                    Key.all.forEach { d.removeObject(forKey: $0.0) }
                }
            } footer: {
                Text("Click a key field and press the key you want: letters, digits, punctuation, Space, Tab, the arrows or F1–F12. Esc cancels, Delete clears. A key can only belong to one shortcut: giving it to another one takes it from the first.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

/// One shortcut's key. A missing setting is the default key; "" is None (the
/// shortcut is off) -- the host reads them the same way.
private struct KeyRow: View {
    let label: String
    let key: String
    let fallback: String
    @AppStorage private var value: String

    init(label: String, key: String, fallback: String) {
        self.label = label
        self.key = key
        self.fallback = fallback
        _value = AppStorage(wrappedValue: fallback, key)
    }

    var body: some View {
        ResettableRow(title: label, isDefault: value == fallback, reset: { assign(fallback) }) {
            KeyRecorderField(value: value, id: key, assign: assign)
        }
    }

    /// One key, one shortcut: the first match wins in the host, so a key that two
    /// shortcuts had would silently belong to only one of them.
    private func assign(_ new: String) {
        if !new.isEmpty {
            let d = UserDefaults.standard
            for (other, fallback) in Key.all where other != key {
                let current = d.object(forKey: other) as? String ?? fallback
                if current.caseInsensitiveCompare(new) == .orderedSame { d.set("", forKey: other) }
            }
        }
        value = new
    }
}

/// The one key field that is listening, if any: one at a time.
@MainActor
@Observable
final class KeyRecording {
    static let shared = KeyRecording()
    var active: String?
    private var monitor: Any?
    private var resign: Any?
    private init() {}

    func start(_ id: String, assign: @escaping (String) -> Void) {
        stop()
        active = id
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] e in
            guard let self, self.active == id else { return e }
            // ⌘, ⌃ and ⌥ keys belong to the menus: they stop the listening and do their job.
            if !e.modifierFlags.intersection([.command, .control, .option]).isEmpty {
                self.stop()
                return e
            }
            switch e.keyCode {
            case 53: self.stop()                                   // Escape: cancel
            case 51, 117: assign(""); self.stop()                  // Delete: None
            default:
                if let name = KeyName.of(e) { assign(name); self.stop() } else { NSSound.beep() }
            }
            return nil
        }
        resign = NotificationCenter.default.addObserver(
            forName: NSWindow.didResignKeyNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.stop() }
        }
    }

    func stop() {
        if let m = monitor { NSEvent.removeMonitor(m) }
        if let r = resign { NotificationCenter.default.removeObserver(r) }
        monitor = nil
        resign = nil
        active = nil
    }
}

/// A key field: click it, press a key.
private struct KeyRecorderField: View {
    let value: String
    let id: String
    let assign: (String) -> Void
    private var recording: Bool { KeyRecording.shared.active == id }

    var body: some View {
        Button {
            if recording { KeyRecording.shared.stop() } else { KeyRecording.shared.start(id, assign: assign) }
        } label: {
            Text(recording ? "Press a key…" : KeyName.display(value))
                .foregroundStyle(recording ? Color.accentColor : (value.isEmpty ? .secondary : .primary))
                .frame(minWidth: 96)
        }
        .buttonStyle(.bordered)
        .help(recording ? "Press the key, or Esc to cancel" : "Click, then press a key")
        .onDisappear { if recording { KeyRecording.shared.stop() } }
    }
}

// MARK: - About

private struct AboutPane: View {
    @AppStorage(Key.automaticChecks) private var automaticChecks = true

    /// Where the game keeps its saves, settings and log: the app's data folder,
    /// or the one a test set (TL_DATA) -- the host's own rule.
    private var dataFolder: URL {
        if let d = ProcessInfo.processInfo.environment["TL_DATA"], !d.isEmpty {
            return URL(fileURLWithPath: d)
        }
        let name = Bundle.main.infoDictionary?["PVZ2DataFolder"] as? String ?? ""
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(name.isEmpty ? "PvZ2 Reflourished" : name)
    }

    /// A build with Sparkle's feed and key (the host's updates_configured)
    private var hasUpdates: Bool {
        let info = Bundle.main.infoDictionary ?? [:]
        return !(info["SUFeedURL"] as? String ?? "").isEmpty && !(info["SUPublicEDKey"] as? String ?? "").isEmpty
    }

    var body: some View {
        Pane {
            Section {
                LabeledContent("Reflourished-Darwin version",
                               value: Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?")
                LabeledContent("Game version",
                               value: Bundle.main.infoDictionary?["PVZ2GameVersion"] as? String ?? "?")
                Text("Plants vs. Zombies 2: Reflourished, running natively on this Mac: the game's own Android code, converted ahead of time, drawn through Metal.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            if hasUpdates {
                Section {
                    LabeledContent("Check for updates automatically") {
                        Toggle("", isOn: $automaticChecks)
                            .toggleStyle(.switch)
                            .labelsHidden()
                    }
                    LabeledContent("New releases") {
                        Button("Check Now") {
                            NSApp.sendAction(Selector(("checkForUpdates:")), to: nil, from: nil)
                        }
                        .controlSize(.small)
                    }
                } header: {
                    Text("Updates")
                } footer: {
                    Text("Once a day the app looks for a new release on GitHub, and asks before it installs one. Your saves and settings stay as they are.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Section("Files") {
                LabeledContent {
                    Button("Show in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([dataFolder])
                    }
                    .controlSize(.small)
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Saves and settings")
                        Text((dataFolder.path as NSString).abbreviatingWithTildeInPath)
                            .font(.caption).foregroundStyle(.secondary)
                            .lineLimit(1).truncationMode(.middle)
                    }
                }
                LabeledContent {
                    Button("Show in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([dataFolder.appendingPathComponent("pvz2.log")])
                    }
                    .controlSize(.small)
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Log")
                        Text("pvz2.log, for a bug report (after a crash: pvz2.previous.log)").font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}

// MARK: - macOS 26 only

private extension View {
    @ViewBuilder
    func scrollEdgeEffectStyleSoftIfAvailable() -> some View {
        if #available(macOS 26.0, *) {
            scrollEdgeEffectStyle(.soft, for: .all)
        } else {
            self
        }
    }
}

// MARK: - Aspect ratios

private enum AspectName {
    /// The shapes on offer, besides the display's own and the game's own. The game
    /// fills 4:3 to 2.4:1; a display outside that is kept to it.
    static let choices: [(tag: String, name: String)] = [
        ("16:9", "16:9"), ("16:10", "16:10"), ("3:2", "3:2"), ("4:3", "4:3"), ("21:9", "21:9"),
    ]
    private static let known: [(String, Double)] = [
        ("16:9", 16.0 / 9), ("16:10", 1.6), ("3:2", 1.5), ("4:3", 4.0 / 3), ("21:9", 21.0 / 9),
    ]
    /// A shape's usual name, or its ratio to one.
    static func of(_ a: Double) -> String {
        if let k = known.first(where: { abs($0.1 / a - 1) < 0.015 }) { return k.0 }
        return String(format: "%.2f:1", a)
    }
}

extension GeneralPane {
    /// The display's own shape, the way the host reads it: below the camera housing.
    @MainActor static func displayAspect() -> Double {
        let s = NSApp.windows.first(where: { $0.title == "PvZ2 Reflourished" })?.screen ?? NSScreen.main
        guard let s else { return 1.6 }
        let h = s.frame.height - s.safeAreaInsets.top
        return h > 0 ? s.frame.width / h : 1.6
    }
}
