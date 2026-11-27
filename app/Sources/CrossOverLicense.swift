import Foundation

/// A valid CrossOver licence, checked before the SteamDriver tool is installed: NotProton's author
/// asked for it (2026-11-27). SteamDriver is built from CodeWeavers' CrossOver Wine sources and
/// reaches Steam through NotProton's bridge, and NotProton itself refuses to install its CrossOver
/// tool without a licence ("The trial version is not supported"). This is the same check
/// NotProton makes:
/// - the licence file com.codeweavers.CrossOver.license, in /Library/Preferences (where CrossOver
///   writes it when it's activated) or ~/Library/Preferences;
/// - its signature beside it (.sha256, or .sig), verified with macOS's own /usr/bin/openssl against
///   CodeWeavers' public key in an installed CrossOver (share/crossover/data/tie.pub);
/// - its "expires" date not yet passed.
/// No licence file means CrossOver isn't activated (the trial). The licence's customer, e-mail and
/// id are never read or kept.
enum CrossOverLicense {
    enum Status:Equatable {
        case valid
        case noCrossOver
        case notActivated
        case invalid
        case expired(String)

        var message:String {
            switch self {
            case .valid: return "CrossOver is activated."
            case .noCrossOver: return "SteamDriver needs a licensed copy of CrossOver: install CrossOver and activate it, then come back here."
            case .notActivated: return "SteamDriver needs a licensed copy of CrossOver, and CrossOver isn’t activated. The trial version isn’t supported."
            case .invalid: return "SteamDriver needs a licensed copy of CrossOver, and the CrossOver licence on this Mac couldn’t be verified. Open CrossOver and activate it again."
            case .expired(let date): return "SteamDriver needs a licensed copy of CrossOver, and the CrossOver licence on this Mac expired on \(date)."
            }
        }
    }

    /// Tests: the folder standing in for "/" (system-wide /Applications and /Library/Preferences).
    nonisolated(unsafe) static var systemRoot=URL(fileURLWithPath:"/")
    static let name="com.codeweavers.CrossOver"
    static let apps=["CrossOver.app","CrossOver Preview.app"]
    static let keyPath="Contents/SharedSupport/CrossOver/share/crossover/data/tie.pub"

    /// CodeWeavers' public keys, from each installed CrossOver.
    static func keys()->[URL] {
        let fm=FileManager.default
        let folders=[systemRoot.appendingPathComponent("Applications"),NotProton.home.appendingPathComponent("Applications")]
        return folders.flatMap { folder in apps.map { folder.appendingPathComponent($0).appendingPathComponent(keyPath) } }
            .filter { fm.fileExists(atPath:$0.path) }
    }
    /// The licence files, system-wide first.
    static func licenses()->[URL] {
        [systemRoot.appendingPathComponent("Library/Preferences"),NotProton.home.appendingPathComponent("Library/Preferences")]
            .map { $0.appendingPathComponent(name+".license") }
            .filter { FileManager.default.fileExists(atPath:$0.path) }
    }

    /// Tests that install the tool without a CrossOver: the answer check() gives instead of looking.
    nonisolated(unsafe) static var answerForTests:Status?

    static func check(today:Date=Date())->Status {
        if let answerForTests { return answerForTests }
        let keys=keys()
        guard !keys.isEmpty else { return .noCrossOver }
        let licenses=licenses()
        guard !licenses.isEmpty else { return .notActivated }
        var expired:String?
        for license in licenses {
            let folder=license.deletingLastPathComponent()
            let signatures=["sha256","sig"].map { folder.appendingPathComponent(name+"."+$0) }
                .filter { FileManager.default.fileExists(atPath:$0.path) }
            guard signatures.contains(where:{ signature in keys.contains { verify(license,signature,$0) } }) else { continue }
            guard let date=expires(license) else { continue }
            if date.passed(today) { expired=date.text;continue }
            return .valid
        }
        if let expired { return .expired(expired) }
        return .invalid
    }

    /// The licence's signature, checked the way NotProton does (macOS's LibreSSL).
    static func verify(_ license:URL,_ signature:URL,_ key:URL)->Bool {
        let task=Process()
        task.executableURL=URL(fileURLWithPath:"/usr/bin/openssl")
        task.arguments=["dgst","-sha256","-verify",key.path,"-signature",signature.path,license.path]
        task.standardOutput=FileHandle.nullDevice;task.standardError=FileHandle.nullDevice
        guard (try? task.run()) != nil else { return false }
        task.waitUntilExit()
        return task.terminationStatus == 0
    }

    struct Expiry { let text:String; let year,month,day:Int
        func passed(_ today:Date)->Bool {
            let c=Calendar(identifier:.gregorian).dateComponents([.year,.month,.day],from:today)
            return (year,month,day) < (c.year ?? 0,c.month ?? 0,c.day ?? 0)
        }
    }
    /// The "expires=YYYY/MM/DD" line; nothing else of the licence is read.
    static func expires(_ license:URL)->Expiry? {
        guard let text=try? String(contentsOf:license,encoding:.utf8),
              let line=text.split(whereSeparator:\.isNewline).first(where:{ $0.hasPrefix("expires=") }) else { return nil }
        let value=String(line.dropFirst("expires=".count)).trimmingCharacters(in:.whitespaces)
        let parts=value.split(separator:"/").compactMap { Int($0) }
        guard parts.count == 3 else { return nil }
        return Expiry(text:value,year:parts[0],month:parts[1],day:parts[2])
    }
}
