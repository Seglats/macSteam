import Foundation

/// Shared and own runtime copies (docs/RUNTIMES.md). Every new SteamDriver prefix is an APFS
/// clone of one template per runtime (SteamDriver/prefix-templates/<runtime>/, made by the run
/// script from the first prefix wineboot builds). With Advanced › Custom runtimes on
/// (SteamDriver/custom-runtimes), a game profile can ask for its own copy instead
/// (profiles/app-<id>/own-runtime): that game's prefix is built from scratch and never shared.
/// The run script reads both files when it makes a prefix. Loading other runtimes is to do.
enum Runtimes {
    private static let fm=FileManager.default
    static var switchFile:URL { NotProton.ours.appendingPathComponent("custom-runtimes") }
    static var templates:URL { NotProton.ours.appendingPathComponent("prefix-templates") }
    static func ownFile(_ profile:String)->URL { Profiles.dir(profile).appendingPathComponent("own-runtime") }

    static var customOn:Bool { fm.fileExists(atPath:switchFile.path) }
    static func setCustom(_ on:Bool) throws {
        if on {
            try fm.createDirectory(at:switchFile.deletingLastPathComponent(),withIntermediateDirectories:true)
            try "# macSteam: Advanced › Custom runtimes is on; game profiles may have own-runtime.\n".write(to:switchFile,atomically:true,encoding:.utf8)
        } else { try? fm.removeItem(at:switchFile) }
    }
    static func ownCopy(_ profile:String)->Bool { fm.fileExists(atPath:ownFile(profile).path) }
    static func setOwnCopy(_ profile:String,_ on:Bool) throws {
        if on { try "# macSteam: this game's prefix is its own copy, not a clone of the shared template.\n".write(to:ownFile(profile),atomically:true,encoding:.utf8) }
        else { try? fm.removeItem(at:ownFile(profile)) }
    }
    /// The runtimes that have a shared template, for the Advanced panel.
    static func sharedTemplates()->[String] {
        ((try? fm.contentsOfDirectory(atPath:templates.path)) ?? []).filter { fm.fileExists(atPath:templates.appendingPathComponent($0).appendingPathComponent(".complete").path) }.sorted()
    }
}
