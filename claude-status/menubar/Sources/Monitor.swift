import AppKit
import SwiftUI

/// Everything the panel shows, kept in step with the helper by polling GET /settings.
/// Changes are shown straight away and sent to the helper; if it refuses one, the control
/// goes back to the helper's value and the error shows under that section.
@MainActor
final class MonitorModel: ObservableObject {
    /// outdated = a helper from before this app, without GET /settings
    enum Connection { case unknown, online, offline, outdated }
    enum Section: Hashable { case header, usage, night, display, music, alerts }

    @Published private(set) var connection = Connection.unknown
    @Published private(set) var status: HelperStatus?
    @Published private(set) var settings = Settings.defaults
    @Published private(set) var errors: [Section: String] = [:]
    @Published private(set) var connecting = false
    @Published private(set) var retrying = false
    @Published private(set) var startingHelper = false
    @Published private(set) var dismissing = false
    @Published private(set) var opening = false
    /// Set for a few minutes after Update opened Terminal, so a second click doesn't start a second update.
    @Published private(set) var updating = false
    /// Set while waiting for the user to finish `claude auth login` in Terminal.
    @Published private(set) var signInDeadline: Date?

    let client: HelperClient

    /// Polls quickly while the panel or the Claude Monitor window is on screen, slowly otherwise
    /// (just for the menu bar icon).
    var panelOpen = false {
        didSet { if panelOpen != oldValue { onScreenChanged() } }
    }
    var windowOpen = false {
        didSet { if windowOpen != oldValue { onScreenChanged() } }
    }

    private var confirmed = Settings.defaults    // the helper's last word
    private var dirty: [String: Int] = [:]       // key -> edit number, until the helper has it
    private var edits = 0
    private var writing = 0
    private var requests = 0
    private var lastWriteDone = 0
    private var pollTask: Task<Void, Never>?
    private var nightTimeTask: Task<Void, Never>?
    private var signInTask: Task<Void, Never>?
    private var updateTask: Task<Void, Never>?
    private var errorTasks: [Section: Task<Void, Never>] = [:]

    init(client: HelperClient) {
        self.client = client
    }

    /// Settings can be changed (assumed so until the first answer, to avoid a flash of grey)
    var canEdit: Bool { connection == .online || connection == .unknown }

    var iconName: String {
        if connection == .offline { return "circle.dashed" }
        if status?.nightActive == true { return "moon.circle" }
        return "circle.circle"
    }

    /// What the plan usage section shows: off | pending | ok | signed_out | error.
    var usageState: String {
        guard settings.bool("plan_usage") else { return "off" }
        let state = status?.usage?.state ?? "pending"
        return state == "off" ? "pending" : state
    }

    // MARK: Polling

    func startPolling() {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            while let self, !Task.isCancelled {
                await self.poll()
                let seconds: UInt64 = self.panelOpen || self.windowOpen ? 2 : 20
                try? await Task.sleep(nanoseconds: seconds * 1_000_000_000)
            }
        }
    }

    private func onScreenChanged() {
        if pollTask != nil { startPolling() }
    }

    func poll(timeout: TimeInterval = 5) async {
        requests += 1
        let started = requests
        do {
            let response = try await client.settings(timeout: timeout)
            // A poll that went out before a change finished may carry the old value
            accept(response, settings: writing == 0 && started > lastWriteDone)
        } catch {
            failed(error, section: nil)
        }
    }

    private func accept(_ response: SettingsResponse, settings applySettings: Bool = true) {
        connection = .online
        status = response.status
        if applySettings {
            confirmed = response.settings
            var shown = response.settings
            for key in dirty.keys { shown[key] = settings[key] }
            settings = shown
        }
        if signInDeadline != nil, usageState == "ok" || !settings.bool("plan_usage") {
            stopWaitingForSignIn()
        }
    }

    private func failed(_ error: any Error, section: Section?) {
        switch error {
        case HelperError.unreachable: connection = .offline
        case HelperError.refused(404, _), HelperError.badResponse: connection = .outdated
        default: break
        }
        if let section {
            show(error.localizedDescription, in: section)
        }
    }

    private func show(_ message: String, in section: Section) {
        errors[section] = message
        errorTasks[section]?.cancel()
        errorTasks[section] = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 10_000_000_000)
            guard !Task.isCancelled else { return }
            self?.errors[section] = nil
        }
    }

    private func clearError(_ section: Section) {
        errorTasks[section]?.cancel()
        errors[section] = nil
    }

    // MARK: Settings

    func set(_ key: String, _ value: SettingValue, in section: Section) {
        guard settings[key] != value else { return }
        markEdited(key, value)
        send([key], in: section)
    }

    func binding(_ key: String, in section: Section) -> Binding<Bool> {
        Binding(get: { self.settings.bool(key) },
                set: { self.set(key, .bool($0), in: section) })
    }

    /// Night mode start/end from the time pickers: sent once the picker has been still for a moment.
    func setNightTime(_ key: String, to date: Date) {
        let parts = Calendar.current.dateComponents([.hour, .minute], from: date)
        let value = String(format: "%02d:%02d", parts.hour ?? 0, parts.minute ?? 0)
        guard settings.string(key) != value else { return }
        markEdited(key, .string(value))
        nightTimeTask?.cancel()
        nightTimeTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 600_000_000)
            guard let self, !Task.isCancelled else { return }
            let keys = ["night_start", "night_end"].filter { self.dirty[$0] != nil }
            if self.settings.string("night_start") == self.settings.string("night_end") {
                for key in keys { self.revert(key) }
                self.show("Night mode needs different From and To times.", in: .night)
                return
            }
            self.send(keys, in: .night)
        }
    }

    /// The picker only shows hours and minutes; the day is a fixed one with no DST change.
    func nightTime(_ key: String) -> Date {
        let parts = settings.string(key).split(separator: ":").compactMap { Int($0) }
        var day = DateComponents(year: 2001, month: 1, day: 1)
        if parts.count == 2 {
            (day.hour, day.minute) = (parts[0], parts[1])
        }
        return Calendar.current.date(from: day) ?? Date()
    }

    private func markEdited(_ key: String, _ value: SettingValue) {
        edits += 1
        dirty[key] = edits
        settings[key] = value
    }

    private func revert(_ key: String) {
        dirty[key] = nil
        settings[key] = confirmed[key]
    }

    private func send(_ keys: [String], in section: Section) {
        guard !keys.isEmpty else { return }
        var changes = Settings()
        var sent: [String: Int] = [:]
        for key in keys {
            changes[key] = settings[key]
            sent[key] = dirty[key]
        }
        writing += 1
        requests += 1
        Task {
            defer {
                writing -= 1
                requests += 1
                lastWriteDone = requests
            }
            do {
                let response = try await client.update(changes)
                for (key, edit) in sent where dirty[key] == edit { dirty[key] = nil }
                clearError(section)
                accept(response)
            } catch {
                for (key, edit) in sent where dirty[key] == edit { revert(key) }
                failed(error, section: section)
            }
        }
    }

    // MARK: Plan usage

    func connectUsage() {
        guard !connecting else { return }
        connecting = true
        clearError(.usage)
        Task {
            defer { connecting = false }
            do {
                let response = try await client.connectUsage()
                accept(response)
                // Without the CLI the section explains what to install. `auth status` can fail when the
                // token works anyway, and a sign-in started from the panel meanwhile already opened Terminal.
                if response.signedIn == false, response.status.claudeCli != nil,
                   usageState != "ok", signInDeadline == nil {
                    signIn()
                }
            } catch {
                failed(error, section: .usage)
            }
        }
    }

    /// Opens Terminal for `claude auth login`, then checks back every 10 s for 3 minutes.
    /// It only asks for a fresh fetch while the helper still says signed_out: each fetch reads
    /// the Keychain, and macOS may ask about that.
    func signIn() {
        guard let cli = status?.claudeCli else {
            show("Install Claude Code's terminal CLI (claude), then try again.", in: .usage)
            return
        }
        do {
            try System.runInTerminal([cli, "auth", "login"],
                                     then: "All set? Claude Monitor will notice by itself. You can close this window.")
        } catch {
            show("Couldn't open Terminal: \(error.localizedDescription)", in: .usage)
            return
        }
        clearError(.usage)
        let deadline = Date().addingTimeInterval(180)
        signInDeadline = deadline
        signInTask?.cancel()
        signInTask = Task { [weak self] in
            while Date() < deadline {
                try? await Task.sleep(nanoseconds: 10_000_000_000)
                guard let self, !Task.isCancelled else { return }
                await self.poll()  // stops the wait once usage works or is turned off
                guard self.signInDeadline != nil, !Task.isCancelled else { return }
                switch self.usageState {
                case "error":
                    // e.g. Keychain access denied: show the message and Try again instead
                    self.stopWaitingForSignIn()
                    return
                case "signed_out":
                    if let response = try? await self.client.refreshUsage(), !Task.isCancelled {
                        self.accept(response)
                    }
                default:
                    break  // pending: a fetch is already under way
                }
                if self.signInDeadline == nil { return }
            }
            self?.signInDeadline = nil
        }
    }

    func stopWaitingForSignIn() {
        signInTask?.cancel()
        signInTask = nil
        signInDeadline = nil
    }

    func retryUsage() {
        guard !retrying else { return }
        retrying = true
        clearError(.usage)
        Task {
            defer { retrying = false }
            do {
                accept(try await client.refreshUsage())
            } catch {
                failed(error, section: .usage)
            }
        }
    }

    func disconnectUsage() {
        stopWaitingForSignIn()
        set("plan_usage", .bool(false), in: .usage)
    }

    // MARK: Display

    /// Dismisses the card on the display's screen, like a double tap. The helper answers with the
    /// card that shows now, if any.
    func dismissCard() {
        guard !dismissing, let key = status?.display?.card?.key else { return }
        dismissing = true
        clearError(.header)
        Task {
            defer { dismissing = false }
            do {
                accept(try await client.dismissCard(key))
            } catch {
                failed(error, section: .header)
            }
        }
    }

    /// Brings the app running the card's session to the front: its terminal tab or editor window.
    func openCard() {
        guard !opening, let key = status?.display?.card?.key else { return }
        opening = true
        clearError(.header)
        Task {
            defer { opening = false }
            do {
                accept(try await client.openCard(key))
            } catch {
                failed(error, section: .header)
            }
        }
    }

    // MARK: Helper

    func startHelper() {
        guard !startingHelper else { return }
        startingHelper = true
        clearError(.header)
        Task {
            defer { startingHelper = false }
            if let problem = await System.startHelper() {
                show("Couldn't start the helper: \(problem). Running install.sh again sets it up.", in: .header)
                return
            }
            // It takes a moment to start listening
            for _ in 0..<12 {
                try? await Task.sleep(nanoseconds: 500_000_000)
                await poll(timeout: 1)
                if connection == .online { return }
            }
            show("The helper didn't start. Open Log shows why.", in: .header)
        }
    }

    func updateHelper() {
        guard !updating, let dir = status?.helperDir else { return }
        do {
            try System.runInTerminal([dir + "/update.sh"])
        } catch {
            show("Couldn't open Terminal: \(error.localizedDescription)", in: .header)
            return
        }
        // The update restarts the helper and this app; the button stays off for a while in case it doesn't
        updating = true
        updateTask?.cancel()
        updateTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 300_000_000_000)
            guard !Task.isCancelled else { return }
            self?.updating = false
        }
    }

    func openLog() {
        if !System.openLog(status?.logPath) {
            show("There's no helper log yet.", in: .header)
        }
    }

    // MARK: Snapshots

    /// Shows a canned helper response (or none, for `offline`) instead of live data.
    func load(_ response: SettingsResponse?, waitingForSignIn: Bool = false, connecting: Bool = false,
              updating: Bool = false) {
        if let response {
            accept(response)
        } else {
            connection = .offline
        }
        if waitingForSignIn {
            signInDeadline = Date().addingTimeInterval(180)
        }
        self.connecting = connecting
        self.updating = updating
    }
}
