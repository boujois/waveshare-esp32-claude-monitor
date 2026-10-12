import AppKit
import SwiftUI

/// The panel in an ordinary "Claude Monitor" window, for when the menu bar icon can't be found
/// (a full menu bar hides it). Opening the app again while it runs shows it; closing only hides it.
@MainActor
final class OptionsWindow {
    /// Posted by a second copy of the app before it quits.
    static let showNotification = Notification.Name("com.claude-status.menubar.show")

    private let model: MonitorModel
    private var window: NSWindow?
    private var observer: NSObjectProtocol?

    init(model: MonitorModel) {
        self.model = model
        observer = DistributedNotificationCenter.default().addObserver(
            forName: Self.showNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.show() }
        }
    }

    /// Asks the copy that's already running to show its window.
    static func askRunningCopyToShow() {
        // Delivered even while that copy is in the background, where AppKit holds back notifications
        DistributedNotificationCenter.default().postNotificationName(showNotification, object: nil,
                                                                     userInfo: nil, deliverImmediately: true)
    }

    func show() {
        let window = self.window ?? make()
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
        window.orderFrontRegardless()  // in case macOS doesn't let a menu bar app come forward
    }

    private func make() -> NSWindow {
        let model = self.model
        let content = PanelView(model: model)
            .background(PanelVisibility { model.windowOpen = $0 })
        let window = NSWindow(contentViewController: NSHostingController(rootView: content))
        window.title = "Claude Monitor"
        window.styleMask = [.titled, .closable]
        window.isReleasedWhenClosed = false
        window.collectionBehavior = [.moveToActiveSpace]
        window.center()
        self.window = window
        return window
    }
}

/// Finder and Spotlight don't start a second copy: they tell the running one, which lands here.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        MonitorApp.optionsWindow?.show()
        return false
    }
}

/// Tells the model when a window showing the panel is on screen. (onAppear/onDisappear aren't
/// reliable for MenuBarExtra windows, which are only ordered out when they close.)
struct PanelVisibility: NSViewRepresentable {
    let changed: @MainActor (Bool) -> Void

    func makeNSView(context: Context) -> Watcher {
        let view = Watcher()
        view.changed = changed
        return view
    }

    func updateNSView(_ view: Watcher, context: Context) {
        view.changed = changed
    }

    final class Watcher: NSView {
        var changed: (@MainActor (Bool) -> Void)?
        private var observers: [NSObjectProtocol] = []

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            observers.forEach { NotificationCenter.default.removeObserver($0) }
            observers = []
            guard let window else {
                changed?(false)
                return
            }
            for name in [NSWindow.didChangeOcclusionStateNotification, NSWindow.didBecomeKeyNotification,
                         NSWindow.didResignKeyNotification] {
                observers.append(NotificationCenter.default.addObserver(forName: name, object: window, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated { self?.report() }
                })
            }
            report()
        }

        private func report() {
            guard let window else { return }
            changed?(window.isVisible && window.occlusionState.contains(.visible))
        }
    }
}
