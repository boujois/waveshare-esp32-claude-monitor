import AppKit
import SwiftUI

// Claude Monitor's menu bar app: a small panel over the Mac helper's settings API.
//
//   Claude Monitor [--port N]                          the menu bar app (opened again while it runs,
//                                                      it shows the panel in a window)
//   Claude Monitor --snapshot out.png [--fixture NAME] [--dark] [--port N]
//                                                      render the panel to a PNG and exit

struct LaunchOptions: Sendable {
    var port = HelperClient.defaultPort
    var snapshot: String?
    var fixture: String?
    var dark = false

    static func parse(_ args: [String]) -> LaunchOptions {
        var options = LaunchOptions()
        let saved = UserDefaults.standard.integer(forKey: "helperPort")
        if (1...65535).contains(saved) { options.port = saved }
        var rest = args.dropFirst()[...]
        func value(_ flag: String) -> String {
            guard let next = rest.popFirst() else { fail("\(flag) needs a value") }
            return next
        }
        while let arg = rest.popFirst() {
            switch arg {
            case "--port":
                guard let port = Int(value(arg)), (1...65535).contains(port) else { fail("--port needs a port number") }
                options.port = port
            case "--snapshot": options.snapshot = value(arg)
            case "--fixture": options.fixture = value(arg)
            case "--dark": options.dark = true
            default: break  // e.g. -NSSomething YES from AppKit
            }
        }
        return options
    }

    private static func fail(_ message: String) -> Never {
        FileHandle.standardError.write(Data("Claude Monitor: \(message)\n".utf8))
        exit(2)
    }
}

@main
enum Main {
    static let bundleID = "com.claude-status.menubar"

    @MainActor
    static func main() {
        let options = LaunchOptions.parse(ProcessInfo.processInfo.arguments)
        if let out = options.snapshot {
            Snapshot.run(to: out, options: options)
        }
        // One icon is enough. A second copy opened by hand shows the running one's window, in
        // case its icon is hidden, and leaves; one started by launchd at login just leaves.
        let others = NSRunningApplication.runningApplications(withBundleIdentifier: bundleID)
            .filter { $0.processIdentifier != getpid() && !$0.isTerminated }
        if !others.isEmpty {
            if ProcessInfo.processInfo.environment["XPC_SERVICE_NAME"] != bundleID {
                OptionsWindow.askRunningCopyToShow()
            }
            exit(0)
        }
        let model = MonitorModel(client: HelperClient(port: options.port))
        model.startPolling()
        MonitorApp.model = model
        MonitorApp.optionsWindow = OptionsWindow(model: model)
        MonitorApp.main()
    }
}

@MainActor
struct MonitorApp: App {
    // Made once in main(), so polling starts exactly once however often SwiftUI makes the App
    @MainActor static var model: MonitorModel!
    @MainActor static var optionsWindow: OptionsWindow?

    @StateObject private var model = MonitorApp.model!
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        MenuBarExtra {
            PanelView(model: model)
                .background(PanelVisibility { model.panelOpen = $0 })
        } label: {
            Image(systemName: model.iconName)
                .accessibilityLabel("Claude Monitor")
        }
        .menuBarExtraStyle(.window)
    }
}
