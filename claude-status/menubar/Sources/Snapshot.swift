import AppKit
import SwiftUI

// --snapshot: renders the panel offscreen to a PNG (for docs, and to check the layout without
// clicking the menu bar). Live data from the helper, or a canned --fixture.
@MainActor
enum Snapshot {
    static let fixtures = ["connected", "off", "signed_out", "offline", "pending", "error", "signing_in",
                           "connecting", "no_cli", "old_firmware", "updating"]

    static func run(to path: String, options: LaunchOptions) -> Never {
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        Task {
            exit(await render(to: path, options: options))
        }
        app.run()
        exit(0)
    }

    private static func render(to path: String, options: LaunchOptions) async -> Int32 {
        let model = MonitorModel(client: HelperClient(port: options.port))
        if let name = options.fixture {
            guard fixtures.contains(name) else {
                FileHandle.standardError.write(Data("Unknown fixture \(name); try \(fixtures.joined(separator: ", "))\n".utf8))
                return 2
            }
            model.load(fixture(name), waitingForSignIn: name == "signing_in", connecting: name == "connecting",
                       updating: name == "updating")
        } else {
            await model.poll()
        }

        let appearance = NSAppearance(named: options.dark ? .darkAqua : .aqua)
        NSApp.appearance = appearance
        let view = NSHostingView(rootView: PanelView(model: model)
            .background(PanelBackground())
            .environment(\.colorScheme, options.dark ? .dark : .light)
            .environment(\.controlActiveState, .key)  // the real panel is the key window
            .environment(\.isSnapshot, true))
        view.appearance = appearance
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 300, height: 600), styleMask: .borderless,
                              backing: .buffered, defer: false)
        window.isOpaque = false
        window.backgroundColor = .clear
        window.appearance = appearance
        window.contentView = view
        // Size to the content once SwiftUI and the AppKit controls inside it have settled
        var size = NSSize.zero
        for _ in 0..<5 {
            view.layoutSubtreeIfNeeded()
            let fitting = view.fittingSize
            if fitting == size { break }
            size = fitting
            window.setContentSize(size)
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        view.layoutSubtreeIfNeeded()

        // Always 2x, whatever screen this Mac has
        guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * 2),
                                         pixelsHigh: Int(size.height * 2), bitsPerSample: 8, samplesPerPixel: 4,
                                         hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                                         bytesPerRow: 0, bitsPerPixel: 0) else { return 1 }
        rep.size = size
        view.cacheDisplay(in: view.bounds, to: rep)
        guard let png = rep.representation(using: .png, properties: [:]) else { return 1 }
        do {
            try png.write(to: URL(fileURLWithPath: path))
        } catch {
            FileHandle.standardError.write(Data("Couldn't write \(path): \(error.localizedDescription)\n".utf8))
            return 1
        }
        print("\(path) (\(Int(size.width))x\(Int(size.height)) pt, \(model.connection))")
        return 0
    }

    /// The menu bar panel's own background is a material that doesn't draw offscreen.
    private struct PanelBackground: View {
        var body: some View {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(Color(nsColor: .windowBackgroundColor))
                .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(Color.primary.opacity(0.12), lineWidth: 1))
        }
    }

    // MARK: Fixtures

    private static func fixture(_ name: String) -> SettingsResponse? {
        let now = Date().timeIntervalSince1970
        // A Thursday 09:00 weekly reset, like the real plans
        let thursday = Calendar.current.nextDate(after: Date(), matching: DateComponents(hour: 9, minute: 0, weekday: 5),
                                                 matchingPolicy: .nextTime)?.timeIntervalSince1970 ?? now + 3 * 86400
        var settings = Settings.defaults
        var status = HelperStatus(
            helperVersion: "v1.4.0", helperDir: "/Users/you/Claude Monitor/helper",
            logPath: "/Users/you/Library/Logs/claude-status-bridge.log", claudeCli: "/Users/you/.local/bin/claude",
            display: DisplayStatus(host: "claude-status.local", reachable: true, version: "v1.4.0", lastPush: now - 1,
                                   nightMode: true),
            update: nil, usage: UsageStatus(state: "off", message: "", h5: nil, d7: nil, h5Reset: 0, d7Reset: 0, at: 0),
            nightActive: false, counts: SessionCounts(waiting: 0, busy: 0, error: 0, done: 0, idle: 0), incident: nil)

        switch name {
        case "connected":
            settings["plan_usage"] = .bool(true)
            settings["night_mode"] = .bool(true)
            status.usage = UsageStatus(state: "ok", message: "", h5: 42, d7: 17.5, h5Reset: now + 2 * 3600 + 10 * 60 + 30,
                                       d7Reset: thursday, at: now - 120)
            status.nightActive = true
            status.counts = SessionCounts(waiting: 1, busy: 2, error: 0, done: 0, idle: 1)
        case "off":
            break
        case "signed_out", "signing_in":
            settings["plan_usage"] = .bool(true)
            status.usage = UsageStatus(state: "signed_out", message: "Sign in to Claude in Terminal", h5: nil, d7: nil,
                                       h5Reset: 0, d7Reset: 0, at: 0)
            status.counts = SessionCounts(waiting: 0, busy: 1, error: 0, done: 1, idle: 0)
            status.display = DisplayStatus(host: "claude-status.local", reachable: false, version: nil, lastPush: 0)
            status.incident = Incident(name: "Elevated errors on Claude Opus", impact: "minor",
                                       status: "investigating", since: now - 1800)
            status.update = "v1.5.0"
        case "connecting":
            settings["plan_usage"] = .bool(true)
            status.usage = UsageStatus(state: "signed_out", message: "Sign in to Claude in Terminal", h5: nil, d7: nil,
                                       h5Reset: 0, d7Reset: 0, at: 0)
        case "no_cli":
            settings["plan_usage"] = .bool(true)
            status.claudeCli = nil
            status.usage = UsageStatus(state: "signed_out", message: "Install Claude Code's terminal CLI, then sign in",
                                       h5: nil, d7: nil, h5Reset: 0, d7Reset: 0, at: 0)
        case "old_firmware", "updating":
            // A display from before night mode, with an update waiting
            settings["night_mode"] = .bool(true)
            status.display = DisplayStatus(host: "claude-status.local", reachable: true, version: "v1.3.0",
                                           lastPush: now - 1, nightMode: false)
            status.update = "v1.4.0"
            status.nightActive = true
        case "pending":
            settings["plan_usage"] = .bool(true)
            settings["night_mode"] = .bool(true)
            settings["night_screen"] = .string("dim")
            status.usage = UsageStatus(state: "pending", message: "", h5: nil, d7: nil, h5Reset: 0, d7Reset: 0, at: 0)
            status.display = DisplayStatus(host: "claude-status.local", reachable: nil, version: nil, lastPush: 0)
        case "error":
            settings["plan_usage"] = .bool(true)
            status.usage = UsageStatus(state: "error", message: "Anthropic's usage service answered 529 (overloaded).",
                                       h5: 88, d7: 64, h5Reset: now + 1500, d7Reset: thursday, at: now - 900)
        default:
            return nil  // offline
        }
        return SettingsResponse(settings: settings, status: status, signedIn: nil)
    }
}
