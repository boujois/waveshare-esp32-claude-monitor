import Foundation

// The Mac helper's HTTP API (claude_status_bridge.py on 127.0.0.1). The app is only a UI over it:
// every setting lives in the helper's config.json.

/// A user-editable setting. The helper only has booleans and strings; anything else is kept
/// as `.other` so a new key from a newer helper doesn't break decoding.
enum SettingValue: Codable, Equatable, Sendable {
    case bool(Bool)
    case string(String)
    case other

    init(from decoder: any Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let b = try? c.decode(Bool.self) {
            self = .bool(b)
        } else if let s = try? c.decode(String.self) {
            self = .string(s)
        } else {
            self = .other
        }
    }

    func encode(to encoder: any Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .bool(let b): try c.encode(b)
        case .string(let s): try c.encode(s)
        case .other: try c.encodeNil()
        }
    }
}

typealias Settings = [String: SettingValue]

extension Settings {
    /// The helper's defaults, used until it has answered (and while it isn't running).
    static let defaults: Settings = [
        "plan_usage": .bool(false), "cat": .bool(true), "ci_checks": .bool(true),
        "status_page": .bool(true), "chat_alerts": .bool(true), "check_replies": .bool(true),
        "night_mode": .bool(false), "night_start": .string("22:00"), "night_end": .string("07:00"),
        "night_screen": .string("off"), "night_alerts": .bool(true),
    ]

    func bool(_ key: String) -> Bool {
        if case .bool(let b)? = self[key] { return b }
        if case .bool(let b)? = Self.defaults[key] { return b }
        return false
    }

    func string(_ key: String) -> String {
        if case .string(let s)? = self[key] { return s }
        if case .string(let s)? = Self.defaults[key] { return s }
        return ""
    }
}

struct DisplayStatus: Decodable, Sendable {
    var host: String?
    var reachable: Bool?    // nil until the helper has tried to push
    var version: String?    // "old" = firmware from before Wi-Fi updates
    var lastPush: Double?
    var nightMode: Bool?    // false = firmware from before night mode; nil until known
    var card: DisplayCard?  // nil for the status screen, and from firmware before Dismiss
    var link: String?       // "wifi", or "usb" while the helper reaches it over its USB cable

    enum CodingKeys: String, CodingKey {
        case host, reachable, version, card, link
        case lastPush = "last_push", nightMode = "night_mode"
    }
}

/// The card on the display's screen: a needs-input alert, a Done card or CI checks.
struct DisplayCard: Decodable, Sendable {
    var key: String?
    var icon: String?   // the symbol in its circle: exclamation | question | check | cross | spinner | dash
    var title: String?  // its headline, e.g. "Permission"
    var name: String?   // the session's name, or the pull request's
}

struct UsageStatus: Decodable, Sendable {
    var state: String?      // off | pending | ok | signed_out | error
    var message: String?
    var h5: Double?
    var d7: Double?
    var h5Reset: Double?
    var d7Reset: Double?
    var at: Double?

    enum CodingKeys: String, CodingKey {
        case state, message, h5, d7, at
        case h5Reset = "h5_reset", d7Reset = "d7_reset"
    }
}

struct SessionCounts: Decodable, Sendable {
    var waiting: Int?
    var busy: Int?
    var error: Int?
    var done: Int?
    var idle: Int?
}

struct Incident: Decodable, Sendable {
    var name: String?
    var impact: String?
    var status: String?
    var since: Double?
}

struct HelperStatus: Decodable, Sendable {
    var helperVersion: String?
    var helperDir: String?
    var logPath: String?
    var claudeCli: String?
    var display: DisplayStatus?
    var update: String?
    var usage: UsageStatus?
    var nightActive: Bool?
    var counts: SessionCounts?
    var incident: Incident?

    enum CodingKeys: String, CodingKey {
        case display, update, usage, counts, incident
        case helperVersion = "helper_version", helperDir = "helper_dir", logPath = "log_path"
        case claudeCli = "claude_cli", nightActive = "night_active"
    }
}

/// The body of GET /settings, and of every POST (which answer with the same thing).
struct SettingsResponse: Decodable, Sendable {
    var settings: Settings
    var status: HelperStatus
    var signedIn: Bool?     // only from POST /usage/connect

    enum CodingKeys: String, CodingKey {
        case settings, status
        case signedIn = "signed_in"
    }
}

enum HelperError: LocalizedError {
    case unreachable
    case refused(Int, String?)
    case badResponse

    var errorDescription: String? {
        switch self {
        case .unreachable: return "The Mac helper isn't running."
        case .refused(_, let message?): return message
        case .refused(let code, nil): return "The helper refused the change (HTTP \(code))."
        case .badResponse: return "The helper sent something unexpected. Is it up to date?"
        }
    }
}

struct HelperClient: Sendable {
    static let defaultPort = 47823

    let port: Int

    // Ephemeral and proxy-free: this only ever talks to 127.0.0.1
    private static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 30  // the longest any request may take; each sets its own
        config.connectionProxyDictionary = [:]
        config.urlCache = nil
        config.httpCookieStorage = nil
        config.httpShouldSetCookies = false
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        return URLSession(configuration: config)
    }()

    /// GET /settings. A short timeout is for checking whether the helper is up.
    func settings(timeout: TimeInterval = 5) async throws -> SettingsResponse {
        try await send(request("settings", timeout: timeout))
    }

    /// POST /settings with only the keys that changed.
    func update(_ changes: Settings) async throws -> SettingsResponse {
        try await send(request("settings", body: try JSONEncoder().encode(changes)))
    }

    /// Turns plan usage on; the helper checks whether the Claude CLI is signed in (up to 15 s).
    func connectUsage() async throws -> SettingsResponse {
        var req = request("usage/connect", body: Data("{}".utf8))
        req.timeoutInterval = 30
        return try await send(req)
    }

    func refreshUsage() async throws -> SettingsResponse {
        try await send(request("usage/refresh", body: Data("{}".utf8)))
    }

    /// Dismisses the card on the display, if it's still the one with this key (up to 5 s).
    func dismissCard(_ key: String) async throws -> SettingsResponse {
        var req = request("display/dismiss", body: try JSONEncoder().encode(["key": key]))
        req.timeoutInterval = 10
        return try await send(req)
    }

    private func request(_ path: String, body: Data? = nil, timeout: TimeInterval = 5) -> URLRequest {
        var req = URLRequest(url: URL(string: "http://127.0.0.1:\(port)/\(path)")!, timeoutInterval: timeout)
        if let body {
            // The helper rejects POSTs without it (it forces a CORS preflight in browsers)
            req.httpMethod = "POST"
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
            req.httpBody = body
        }
        return req
    }

    private func send(_ req: URLRequest) async throws -> SettingsResponse {
        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await Self.session.data(for: req)
        } catch let error as URLError where error.code != .cancelled {
            throw HelperError.unreachable
        }
        guard let http = response as? HTTPURLResponse else { throw HelperError.badResponse }
        guard http.statusCode == 200 else {
            let message = (try? JSONDecoder().decode([String: String].self, from: data))?["error"]
            throw HelperError.refused(http.statusCode, message)
        }
        do {
            return try JSONDecoder().decode(SettingsResponse.self, from: data)
        } catch {
            throw HelperError.badResponse
        }
    }
}
