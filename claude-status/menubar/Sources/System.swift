import AppKit

// Things the app does outside the helper's API: Terminal commands, starting the helper, the log.
enum System {
    static let helperLabel = "com.claude-status.bridge"
    static let defaultLogPath = NSHomeDirectory() + "/Library/Logs/claude-status-bridge.log"

    /// Opens Terminal running `argv`. It goes through a throwaway .command script, which Terminal
    /// runs when opened, so the app needs no AppleScript or Automation permission.
    static func runInTerminal(_ argv: [String], then message: String? = nil) throws {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("claude-monitor-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        let script = dir.appendingPathComponent("Claude Monitor.command")
        try commandScript(argv, then: message).write(to: script, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: script.path)
        guard NSWorkspace.shared.open(script) else {
            throw CocoaError(.fileReadUnknown, userInfo: [NSLocalizedDescriptionKey: "Couldn't open Terminal."])
        }
    }

    static func commandScript(_ argv: [String], then message: String? = nil) -> String {
        var lines = [
            "#!/bin/sh",
            // The script is only needed until Terminal has started it
            "rm -f \"$0\"; rmdir \"${0%/*}\" 2>/dev/null",
            "clear",
            argv.map(shellQuote).joined(separator: " "),
        ]
        if let message {
            lines.append("echo; echo \(shellQuote(message))")
        }
        return lines.joined(separator: "\n") + "\n"
    }

    static func shellQuote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    static let helperPlist = NSHomeDirectory() + "/Library/LaunchAgents/\(helperLabel).plist"

    /// (Re)starts the helper's launchd agent; -k also replaces a copy that's running but not
    /// answering. If the agent isn't loaded but is installed, loads it. Returns an error
    /// message, or nil when launchctl succeeded.
    static func startHelper() async -> String? {
        await Task.detached {
            let domain = "gui/\(getuid())"
            var (status, text) = launchctl(["kickstart", "-k", "\(domain)/\(helperLabel)"])
            // 113: "Could not find service" (booted out, e.g. by an interrupted install)
            if status == 113 || text.contains("Could not find service"),
               FileManager.default.fileExists(atPath: helperPlist) {
                (status, text) = launchctl(["bootstrap", domain, helperPlist])
            }
            if status == 0 { return nil }
            return text.isEmpty ? "launchctl failed (\(status))" : text
        }.value
    }

    /// Runs launchctl; its exit status and what it said on stderr, on one line.
    private static func launchctl(_ arguments: [String]) -> (Int32, String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = FileHandle.nullDevice
        process.standardError = pipe
        do {
            try process.run()
        } catch {
            return (-1, error.localizedDescription)
        }
        let output = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        let text = String(decoding: output, as: UTF8.self)
            .split(whereSeparator: \.isNewline).joined(separator: " ")
            .trimmingCharacters(in: .whitespaces)
        return (process.terminationStatus, text)
    }

    /// Opens the helper's log (in Console). False if there's no log yet.
    @MainActor
    static func openLog(_ path: String?) -> Bool {
        let path = path ?? defaultLogPath
        guard FileManager.default.fileExists(atPath: path) else { return false }
        return NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }
}
