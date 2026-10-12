import SwiftUI

/// The menu bar panel.
struct PanelView: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HeaderSection(model: model)
            VStack(alignment: .leading, spacing: 0) {
                SectionDivider()
                UsageSection(model: model)
                SectionDivider()
                NightSection(model: model)
                SectionDivider()
                ToggleSection(model: model, title: "Display", section: .display, rows: [
                    ("Cat", "cat"), ("CI checks", "ci_checks"), ("Anthropic incidents", "status_page"),
                ])
                SectionDivider()
                ToggleSection(model: model, title: "Alerts", section: .alerts, rows: [
                    ("Claude app chat alerts", "chat_alerts"), ("Check replies with Haiku", "check_replies"),
                ])
            }
            .disabled(!model.canEdit)
            .opacity(model.canEdit ? 1 : 0.45)
            SectionDivider()
            FooterSection(model: model)
        }
        .font(.system(size: 13))
        .padding(.horizontal, 14)
        .padding(.top, 12)
        .padding(.bottom, 8)
        .frame(width: 300, alignment: .leading)
    }
}

// MARK: Header

private struct HeaderSection: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(alignment: .firstTextBaseline) {
                Text("Claude Monitor").font(.system(size: 13, weight: .semibold))
                Spacer()
                if let version = model.status?.helperVersion {
                    Text(version).font(.system(size: 11)).foregroundStyle(.secondary)
                }
            }
            switch model.connection {
            case .offline:
                StatusLine(color: .red, text: "The Mac helper isn't running")
                Caption("It sends Claude Code's status to the display.")
                Button(action: model.startHelper) {
                    HStack(spacing: 6) {
                        if model.startingHelper { ProgressView().controlSize(.small).scaleEffect(0.7) }
                        Text("Start helper")
                    }
                }
                .controlSize(.small)
                .padding(.top, 3)
            case .outdated:
                StatusLine(color: .orange, text: "The Mac helper is out of date")
                Caption("Update Claude Monitor to change settings from here.")
            case .unknown:
                StatusLine(color: .gray, text: "Connecting to the Mac helper…")
            case .online:
                DisplayLine(display: model.status?.display)
                SessionsLine(counts: model.status?.counts)
                if let incident = model.status?.incident {
                    Label {
                        Text(incident.name ?? "Claude incident").fixedSize(horizontal: false, vertical: true)
                    } icon: {
                        Image(systemName: "exclamationmark.triangle.fill")
                    }
                    .font(.system(size: 12))
                    .foregroundStyle(.red)
                }
                if let update = model.status?.update, model.status?.helperDir != nil {
                    Button(model.updating ? "Updating in Terminal…" : "Update to \(update)…", action: model.updateHelper)
                        .disabled(model.updating)
                        .controlSize(.small)
                        .padding(.top, 3)
                }
            }
            ErrorText(model.errors[.header])
        }
        .padding(.bottom, 2)
    }
}

private struct DisplayLine: View {
    let display: DisplayStatus?

    var body: some View {
        switch display?.reachable {
        case true?:
            let version = display?.version.map { $0 == "old" ? " · older firmware" : " · \($0)" } ?? ""
            StatusLine(color: .green, text: "Display connected" + version)
        case false?:
            StatusLine(color: .orange, text: "Display not reachable (\(display?.host ?? "claude-status.local"))")
        case nil:
            StatusLine(color: .gray, text: "Looking for the display…")
        }
    }
}

private struct SessionsLine: View {
    let counts: SessionCounts?

    var body: some View {
        StatusLine(color: nil, text: text)
    }

    private var text: String {
        let c = counts
        var parts: [String] = []
        if let n = c?.waiting, n > 0 { parts.append(n == 1 ? "1 needs you" : "\(n) need you") }
        if let n = c?.busy, n > 0 { parts.append("\(n) working") }
        if let n = c?.error, n > 0 { parts.append(n == 1 ? "1 error" : "\(n) errors") }
        if let n = c?.done, n > 0 { parts.append("\(n) done") }
        if let n = c?.idle, n > 0 { parts.append("\(n) idle") }
        return parts.isEmpty ? "No active sessions" : parts.joined(separator: " · ")
    }
}

/// A status dot and a line of text; no dot keeps the text aligned under the ones that have one.
private struct StatusLine: View {
    let color: Color?
    let text: String

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Circle()
                .fill(color ?? .clear)
                .frame(width: 7, height: 7)
                .alignmentGuide(.firstTextBaseline) { $0[.bottom] - 0.5 }
            Text(text)
                .foregroundStyle(color == nil ? .secondary : .primary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.system(size: 12))
    }
}

// MARK: Plan usage

private struct UsageSection: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionTitle("Plan usage")
            let usage = model.status?.usage
            if model.signInDeadline != nil && model.usageState != "ok" {
                Working("Finish signing in in Terminal…")
                Caption("This updates by itself once you're signed in.")
                KeychainHint()
                HStack(spacing: 12) {
                    Spacer()
                    Button("Cancel", action: model.stopWaitingForSignIn)
                        .buttonStyle(.link)
                        .font(.system(size: 11))
                    DisconnectLink(model: model)
                }
            } else if model.connecting && model.usageState != "off" && model.usageState != "ok" {
                // Until Connect has its answer (it may open Terminal itself)
                Working("Connecting…")
                KeychainHint()
            } else {
                switch model.usageState {
                case "off":
                    HStack(alignment: .center, spacing: 10) {
                        Caption("Show your 5-hour and weekly limits on the display.")
                        Spacer(minLength: 0)
                        Button(action: model.connectUsage) {
                            if model.connecting {
                                ProgressView().controlSize(.small).scaleEffect(0.7).frame(width: 52)
                            } else {
                                Text("Connect").frame(minWidth: 52)
                            }
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.small)
                    }
                case "pending":
                    HStack {
                        Working("Connecting…")
                        Spacer()
                        DisconnectLink(model: model)
                    }
                    KeychainHint()
                case "ok":
                    UsageBar(title: "5-hour", percent: usage?.h5, reset: Self.resetsIn(usage?.h5Reset))
                    UsageBar(title: "Weekly", percent: usage?.d7, reset: Self.resetsAt(usage?.d7Reset))
                    HStack {
                        if let at = usage?.at, at > 0 {
                            Caption("Updated \(Self.ago(at))")
                        }
                        Spacer()
                        DisconnectLink(model: model)
                    }
                case "signed_out":
                    // The app says what's missing: the helper's message may be from before the CLI was installed
                    Message(model.status?.claudeCli == nil
                            ? "Install Claude Code's terminal CLI (claude), then try again."
                            : usage?.message?.nonEmpty ?? "Sign in to Claude in Terminal.")
                    HStack(spacing: 8) {
                        if model.status?.claudeCli != nil {
                            Button("Sign in…", action: model.signIn).buttonStyle(.borderedProminent)
                        }
                        TryAgain(model: model)
                        Spacer()
                        DisconnectLink(model: model)
                    }
                    .controlSize(.small)
                default:
                    Message(usage?.message?.nonEmpty ?? "Couldn't get your plan usage.")
                    HStack {
                        TryAgain(model: model)
                        Spacer()
                        DisconnectLink(model: model)
                    }
                    .controlSize(.small)
                    KeychainHint()
                }
            }
            ErrorText(model.errors[.usage])
        }
    }

    static func resetsIn(_ time: Double?) -> String? {
        guard let time, time > 0 else { return nil }
        let minutes = Int((time - Date().timeIntervalSince1970) / 60)
        if minutes < 1 { return "resets in under a minute" }
        let (h, m) = (minutes / 60, minutes % 60)
        return h > 0 ? "resets in \(h)h \(m)m" : "resets in \(m)m"
    }

    static func resetsAt(_ time: Double?) -> String? {
        guard let time, time > 0 else { return nil }
        let formatter = DateFormatter()
        formatter.setLocalizedDateFormatFromTemplate("EEEjmm")
        return "resets " + formatter.string(from: Date(timeIntervalSince1970: time))
    }

    static func ago(_ time: Double) -> String {
        let minutes = Int(Date().timeIntervalSince1970 - time) / 60
        if minutes < 1 { return "just now" }
        if minutes < 60 { return "\(minutes) min ago" }
        return "\(minutes / 60) h ago"
    }
}

private struct UsageBar: View {
    let title: String
    let percent: Double?
    let reset: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(title).font(.system(size: 12, weight: .medium))
                Spacer()
                if let reset {
                    Text(reset).font(.system(size: 11)).foregroundStyle(.secondary)
                }
            }
            HStack(spacing: 8) {
                GeometryReader { geo in
                    ZStack(alignment: .leading) {
                        Capsule().fill(Color.primary.opacity(0.1))
                        Capsule().fill(tint)
                            .frame(width: max(6, geo.size.width * min(fraction, 1)))
                            .opacity(percent == nil ? 0 : 1)
                    }
                }
                .frame(height: 6)
                Text(percent.map { "\(Int($0.rounded()))%" } ?? "–")
                    .font(.system(size: 11, weight: .medium))
                    .monospacedDigit()
                    .frame(width: 34, alignment: .trailing)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(title)
        .accessibilityValue([percent.map { "\(Int($0.rounded())) percent" }, reset].compactMap { $0 }.joined(separator: ", "))
    }

    private var fraction: Double { (percent ?? 0) / 100 }

    private var tint: Color {
        switch percent ?? 0 {
        case 90...: return .red
        case 75...: return .orange
        default: return .accentColor
        }
    }
}

private struct TryAgain: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        Button(action: model.retryUsage) {
            HStack(spacing: 5) {
                if model.retrying { ProgressView().controlSize(.small).scaleEffect(0.6).frame(width: 10, height: 10) }
                Text("Try again")
            }
        }
    }
}

private struct KeychainHint: View {
    var body: some View {
        Caption("If macOS asks about “Claude Code-credentials”, click Always Allow.")
    }
}

/// Turns plan usage off, from any state (the helper otherwise keeps retrying).
private struct DisconnectLink: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        Button("Disconnect", action: model.disconnectUsage)
            .buttonStyle(.link)
            .font(.system(size: 11))
    }
}

// MARK: Night mode

private struct NightSection: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                SectionTitle("Night mode")
                if model.settings.bool("night_mode") && model.status?.nightActive == true && !needsUpdate {
                    Badge(text: "On now", systemImage: "moon.fill")
                }
                Spacer()
                Switch(title: "Night mode", isOn: model.binding("night_mode", in: .night))
            }
            if model.settings.bool("night_mode") {
                HStack(spacing: 6) {
                    Text("From")
                    TimePicker(model: model, key: "night_start", title: "From")
                    Spacer(minLength: 4)
                    Text("To")
                    TimePicker(model: model, key: "night_end", title: "To")
                }
                HStack {
                    Text("Screen")
                    Spacer()
                    Picker("Screen", selection: Binding(
                        get: { model.settings.string("night_screen") },
                        set: { model.set("night_screen", .string($0), in: .night) }
                    )) {
                        Text("Off").tag("off")
                        Text("Dim").tag("dim")
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .controlSize(.small)
                    .fixedSize()
                }
                ToggleRow(title: "Alerts still light it up", isOn: model.binding("night_alerts", in: .night))
                if !needsUpdate {
                    Caption("Double-tap the display to peek.")
                }
            } else {
                Caption("Turn the screen off or dim it overnight.")
            }
            if needsUpdate {
                // Settings still save; the display just ignores them until it's updated
                Notice("Update the display to use night mode")
            }
            ErrorText(model.errors[.night])
        }
    }

    /// The display's firmware is from before night mode (nil = not known yet).
    private var needsUpdate: Bool { model.status?.display?.nightMode == false }
}

private struct TimePicker: View {
    @ObservedObject var model: MonitorModel
    let key: String
    let title: String

    var body: some View {
        DatePicker(title, selection: Binding(
            get: { model.nightTime(key) },
            set: { model.setNightTime(key, to: $0) }
        ), displayedComponents: .hourAndMinute)
        .datePickerStyle(.field)
        .labelsHidden()
        .controlSize(.small)
        .fixedSize()
    }
}

private struct Badge: View {
    let text: String
    let systemImage: String

    var body: some View {
        Label(text, systemImage: systemImage)
            .labelStyle(BadgeLabelStyle())
            .font(.system(size: 10, weight: .semibold))
            .foregroundStyle(Color.indigo)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Capsule().fill(Color.indigo.opacity(0.15)))
    }
}

private struct BadgeLabelStyle: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 3) {
            configuration.icon.imageScale(.small)
            configuration.title
        }
    }
}

// MARK: Toggles and footer

private struct ToggleSection: View {
    @ObservedObject var model: MonitorModel
    let title: String
    let section: MonitorModel.Section
    let rows: [(String, String)]

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            SectionTitle(title)
            ForEach(rows, id: \.1) { row in
                ToggleRow(title: row.0, isOn: model.binding(row.1, in: section))
            }
            ErrorText(model.errors[section])
        }
    }
}

private struct ToggleRow: View {
    let title: String
    let isOn: Binding<Bool>

    var body: some View {
        HStack {
            Text(title)
            Spacer(minLength: 8)
            Switch(title: title, isOn: isOn)
        }
    }
}

private struct Switch: View {
    let title: String
    let isOn: Binding<Bool>
    private let isSnapshot = Environment(\.isSnapshot)

    var body: some View {
        if isSnapshot.wrappedValue {
            SnapshotSwitch(isOn: isOn.wrappedValue)
        } else {
            Toggle(title, isOn: isOn)
                .toggleStyle(.switch)
                .labelsHidden()
                .controlSize(.mini)
        }
    }
}

/// Offscreen, AppKit draws a switch without its coloured track, so snapshots draw their own.
private struct SnapshotSwitch: View {
    let isOn: Bool

    var body: some View {
        Capsule()
            .fill(isOn ? Color.accentColor : Color.primary.opacity(0.12))
            .frame(width: 36, height: 16)
            .overlay(alignment: isOn ? .trailing : .leading) {
                Capsule()
                    .fill(.white)
                    .shadow(color: .black.opacity(0.25), radius: 0.5, y: 0.5)
                    .frame(width: 21, height: 12)
                    .padding(2)
            }
    }
}

extension EnvironmentValues {
    var isSnapshot: Bool {
        get { self[SnapshotKey.self] }
        set { self[SnapshotKey.self] = newValue }
    }

    private struct SnapshotKey: EnvironmentKey {
        static let defaultValue = false
    }
}

private struct FooterSection: View {
    @ObservedObject var model: MonitorModel

    var body: some View {
        HStack {
            Button("Open Log", action: model.openLog)
            Spacer()
            Button("Quit") { NSApplication.shared.terminate(nil) }
                .keyboardShortcut("q")
        }
        .buttonStyle(FooterButtonStyle())
        .padding(.top, -2)
    }
}

/// Plain text buttons that highlight on hover, like menu items.
private struct FooterButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        FooterButton(configuration: configuration)
    }

    private struct FooterButton: View {
        let configuration: ButtonStyleConfiguration
        // Spelled out because the @State macro needs a compiler plugin that only Xcode ships
        private let hovering = State(initialValue: false)

        var body: some View {
            configuration.label
                .font(.system(size: 13))
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 5)
                    .fill(Color.primary.opacity(configuration.isPressed ? 0.15 : hovering.wrappedValue ? 0.08 : 0)))
                .contentShape(Rectangle())
                .onHover { hovering.wrappedValue = $0 }
                .padding(.horizontal, -8)
        }
    }
}

// MARK: Small pieces

private struct SectionDivider: View {
    var body: some View {
        Divider().padding(.vertical, 10)
    }
}

private struct SectionTitle: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text).font(.system(size: 13, weight: .semibold))
    }
}

private struct Caption: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.system(size: 11))
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }
}

private struct Message: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text).font(.system(size: 12)).fixedSize(horizontal: false, vertical: true)
    }
}

/// Something the user may need to do, less urgent than an error.
private struct Notice: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Label {
            Text(text).fixedSize(horizontal: false, vertical: true)
        } icon: {
            Image(systemName: "arrow.down.circle")
        }
        .font(.system(size: 11))
        .foregroundStyle(.orange)
    }
}

private struct Working: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        HStack(spacing: 8) {
            ProgressView().controlSize(.small).scaleEffect(0.75).frame(width: 14, height: 14)
            Text(text).font(.system(size: 12))
        }
    }
}

private struct ErrorText: View {
    let text: String?
    init(_ text: String?) { self.text = text }

    var body: some View {
        if let text {
            Label {
                Text(text).fixedSize(horizontal: false, vertical: true)
            } icon: {
                Image(systemName: "exclamationmark.circle.fill")
            }
            .font(.system(size: 11))
            .foregroundStyle(.red)
        }
    }
}

private extension String {
    var nonEmpty: String? { isEmpty ? nil : self }
}
