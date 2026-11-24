import Foundation

/// macSteam was called MacVolvo until 2026-11-24 (Brand.swift; briefly SteamDriver, whose folder
/// and file names it keeps). At the first start after the
/// rename, before anything reads them, this moves what the old name kept over to the new one, once
/// and without overwriting anything already in a new place:
///
/// - settings: the old bundle identifier's preferences (app.macvolvo.launcher) are copied into
///   the new one's, unless it already has its own; a saved install location that was the old
///   standard folder follows the folder;
/// - the NotProton folder: ~/Library/Application Support/MacVolvo NotProton (drivers, DXMT,
///   renderer profiles, mouse status, key combo overrides) becomes …/SteamDriver;
/// - the own-Steam data: …/MacVolvo becomes …/SteamDriver Steam (moved within the same volume,
///   so nothing is copied); a custom location elsewhere stays where it is and is still recognised
///   by its owner marker;
/// - NotProton's files: notproton/variants|plugins/macvolvo.conf become steamdriver.conf; the
///   next refresh rewrites them with the current names. Their ids stay (macvolvo-<build>,
///   NOTPROTON_MACVOLVO_*), so every game keeps the options chosen for it in Steam;
/// - before profiles (2026-11-24): the CS2 and Deadlock renderer profiles and the key combo lists
///   become profiles (Profiles.migrate);
/// - a D3DMetal imported before the GPTK folder (d3dmetal/apple_gptk) becomes SteamDriver/GPTK.
///
/// Every step is skipped when its source is missing or its target exists, so running it again is
/// harmless. What happened is written to the app's log through `notes`.
enum Migration {
    nonisolated(unsafe) static var notes:[String]=[]
    private static let fm=FileManager.default

    /// Every path comes from NotProton.home (the player's home; tests point it at a throwaway one).
    static func run(defaults:Bool=true) {
        let library=NotProton.home.appendingPathComponent("Library/Application Support")
        if defaults { migrateDefaults(library:library) }
        move(library.appendingPathComponent("MacVolvo NotProton"),to:NotProton.ours,what:"NotProton folder")
        move(library.appendingPathComponent(Brand.legacyDataFolderName),to:library.appendingPathComponent(Brand.dataFolderName),what:"own-Steam data")
        for (old,new) in NotProton.legacyFiles { move(old,to:new,what:"NotProton file \(old.lastPathComponent)") }
        Profiles.migrate(&notes)
        // D3DMetal imported before the GPTK folder: d3dmetal/apple_gptk (redist/lib) and its notices.
        let old=NotProton.ours.appendingPathComponent("d3dmetal")
        move(old.appendingPathComponent("apple_gptk"),to:NotProton.gptkFolder,what:"imported Game Porting Toolkit")
        move(old.appendingPathComponent("licenses"),to:NotProton.gptkFolder.appendingPathComponent("licenses"),what:"Game Porting Toolkit notices")
        if fm.fileExists(atPath:NotProton.gptkFolder.path) { try? fm.removeItem(at:old) }
    }

    private static func move(_ old:URL,to new:URL,what:String) {
        guard fm.fileExists(atPath:old.path),!fm.fileExists(atPath:new.path) else { return }
        do {
            try fm.createDirectory(at:new.deletingLastPathComponent(),withIntermediateDirectories:true)
            try fm.moveItem(at:old,to:new)
            notes.append("moved the \(what) from \(old.path) to \(new.path)")
        } catch { notes.append("could not move the \(what) from \(old.path): \(error.localizedDescription)") }
    }

    private static func migrateDefaults(library:URL) {
        let defaults=UserDefaults.standard
        guard defaults.persistentDomain(forName:Brand.bundleIdentifier)?.isEmpty ?? true,
              var old=defaults.persistentDomain(forName:Brand.legacyBundleIdentifier),!old.isEmpty else { return }
        // The old standard folder moves (above), so a saved location naming it follows.
        let oldStandard=library.appendingPathComponent(Brand.legacyDataFolderName).standardizedFileURL.path
        if let saved=old[InstallLocation.savedKey] as? String,URL(fileURLWithPath:saved).standardizedFileURL.path==oldStandard {
            old[InstallLocation.savedKey]=library.appendingPathComponent(Brand.dataFolderName).path
        }
        defaults.setPersistentDomain(old,forName:Brand.bundleIdentifier)
        notes.append("copied \(old.count) setting(s) from \(Brand.legacyBundleIdentifier)")
    }
}
