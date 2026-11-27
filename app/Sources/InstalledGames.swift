import Foundation

/// The games macSteam installed (GameInstaller): one per SteamDriver/installers/<Name>/ folder,
/// and deleting one again. docs/GOG.md, "Deleting a game".
///
/// Deleting removes, with Steam closed (it rewrites its files when it quits): the shortcut in
/// shortcuts.vdf, its CompatToolMapping entry in config.vdf, its artwork in grid/; then the
/// game's prefix (steamapps/compatdata/<id>: the installed game), the installer copy and
/// macSteam's record. "Keep saves and settings" first copies the prefix's user folder
/// (drive_c/users: AppData, Documents, Saved Games) and its user registry (user.reg: settings
/// such as Unity's) to SteamDriver/saved-games/<Name>/; the run script puts them back when the
/// same game is installed again from macSteam.
enum InstalledGames {
    private static let fm=FileManager.default
    static var savedGames:URL { NotProton.ours.appendingPathComponent("saved-games") }

    struct Game:Identifiable,Equatable {
        var id:String { folder.lastPathComponent }
        let folder:URL           // SteamDriver/installers/<Name>
        let name:String
        let shortcutID:UInt32?
        let inSteam:Bool
        var prefix:URL? { shortcutID.map { GameInstaller.steam.appendingPathComponent("steamapps/compatdata/\($0)") } }
        /// Where the prefix really is: elsewhere when it was installed with "Install to" (then
        /// compatdata/<id> is a link to it), nil if it's in Steam's folder.
        var elsewhere:URL? {
            guard let prefix,let target=try? FileManager.default.destinationOfSymbolicLink(atPath:prefix.path) else { return nil }
            return URL(fileURLWithPath:target,relativeTo:prefix.deletingLastPathComponent()).standardizedFileURL
        }
        var cover:URL? {
            guard let shortcutID,let user=GameInstaller.userFolder() else { return nil }
            return user.appendingPathComponent("config/grid/\(shortcutID)p.jpg")
        }
    }

    /// Every installer folder macSteam made, with its shortcut: the id it noted at install
    /// ("shortcut"), else a shortcut in Steam whose target is in that folder, else Steam's id
    /// formula for its installer and name (a shortcut already removed in Steam).
    static func list()->[Game] {
        let shortcuts=steamShortcuts()
        let folders=((try? fm.contentsOfDirectory(at:GameInstaller.installers,includingPropertiesForKeys:nil)) ?? []).filter(\.hasDirectoryPath)
        return folders.map { folder in
            let name=((try? String(contentsOf:folder.appendingPathComponent("name"),encoding:.utf8)) ?? folder.lastPathComponent).trimmingCharacters(in:.whitespacesAndNewlines)
            var id=UInt32(((try? String(contentsOf:folder.appendingPathComponent("shortcut"),encoding:.utf8)) ?? "").trimmingCharacters(in:.whitespacesAndNewlines))
            // Compared resolved, so a path through a symlink (~/Library/Application Support/Steam
            // is one here) still matches.
            let prefix=folder.resolvingSymlinksInPath().path+"/"
            if id==nil { id=shortcuts.first { resolved($0.exe).hasPrefix(prefix) }?.id }
            if id==nil,let exe=((try? fm.contentsOfDirectory(atPath:folder.path)) ?? []).first(where:{ $0.lowercased().hasSuffix(".exe") }) {
                id=GameInstaller.shortcutID(exe:"\""+folder.appendingPathComponent(exe).path+"\"",name:name)
            }
            return Game(folder:folder,name:name,shortcutID:id,inSteam:shortcuts.contains { $0.id==id })
        }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }
    private static func resolved(_ quotedExe:String)->String {
        URL(fileURLWithPath:quotedExe.trimmingCharacters(in:CharacterSet(charactersIn:"\""))).resolvingSymlinksInPath().path
    }
    private static func steamShortcuts()->[(id:UInt32,exe:String)] {
        guard let user=GameInstaller.userFolder(),let data=try? Data(contentsOf:user.appendingPathComponent("config/shortcuts.vdf")),
              let root=BinaryVDF.parse(data),case .map(let entries)?=root.first(where:{ $0.0=="shortcuts" })?.1 else { return [] }
        return entries.compactMap { entry in
            guard case .map(let f)=entry.1,case .int(let id)?=f.first(where:{ $0.0.lowercased()=="appid" })?.1,
                  case .string(let exe)?=f.first(where:{ $0.0.lowercased()=="exe" })?.1 else { return nil }
            return (id,exe)
        }
    }
    /// Disk space the installed game takes (its prefix), for the list. Walks the folder: call it
    /// off the main thread.
    static func size(_ game:Game)->UInt64 {
        guard let prefix=game.elsewhere ?? game.prefix,let walk=fm.enumerator(at:prefix,includingPropertiesForKeys:[.totalFileAllocatedSizeKey,.isRegularFileKey]) else { return 0 }
        var total:UInt64=0
        for case let url as URL in walk {
            if let v=try? url.resourceValues(forKeys:[.totalFileAllocatedSizeKey,.isRegularFileKey]),v.isRegularFile==true { total+=UInt64(v.totalFileAllocatedSize ?? 0) }
        }
        return total
    }
    static func hasSaves(_ game:Game)->Bool { fm.fileExists(atPath:savedGames.appendingPathComponent(game.id).path) }

    // MARK: Steam's side (Steam must not be running)
    /// The shortcut, its compatibility tool entry and its artwork. Other shortcuts stay as they are.
    static func removeFromSteam(_ id:UInt32) throws {
        if let user=GameInstaller.userFolder() {
            let file=user.appendingPathComponent("config/shortcuts.vdf")
            if let data=try? Data(contentsOf:file) {
                guard var root=BinaryVDF.parse(data) else { throw GameInstaller.Failure.message("Steam’s shortcuts.vdf couldn’t be read, so nothing was changed.") }
                if let r=root.firstIndex(where:{ $0.0=="shortcuts" }),case .map(let entries)=root[r].1 {
                    let kept=entries.filter { entry in
                        guard case .map(let f)=entry.1,case .int(let v)?=f.first(where:{ $0.0.lowercased()=="appid" })?.1 else { return true }
                        return v != id
                    }
                    if kept.count != entries.count {
                        // Steam numbers its shortcuts 0, 1, 2, … in order.
                        root[r].1 = .map(kept.enumerated().map { (String($0.offset),$0.element.1) })
                        try BinaryVDF.encode(root).write(to:file,options:.atomic)
                    }
                }
            }
            let grid=user.appendingPathComponent("config/grid")
            for name in (try? fm.contentsOfDirectory(atPath:grid.path)) ?? [] where name.hasPrefix("\(id)") {
                let rest=name.dropFirst("\(id)".count)
                if rest.hasPrefix(".") || rest.hasPrefix("p.") || rest.hasPrefix("_") { try? fm.removeItem(at:grid.appendingPathComponent(name)) }
            }
        }
        let config=GameInstaller.steam.appendingPathComponent("config/config.vdf")
        if var text=try? String(contentsOf:config,encoding:.utf8),
           let entry=text.range(of:#"\n[ \t]*""#+String(id)+#""[ \t]*\n[ \t]*\{[^{}]*\}[ \t]*(?=\n)"#,options:.regularExpression) {
            text.removeSubrange(entry)
            try text.write(to:config,atomically:true,encoding:.utf8)
        }
    }

    // MARK: Our side
    /// The prefix's user folder and user registry, for putting back after a reinstall.
    static func keepSaves(_ game:Game) throws {
        guard let prefix=game.prefix?.appendingPathComponent("pfx"),fm.fileExists(atPath:prefix.appendingPathComponent("drive_c/users").path) else { return }
        let target=savedGames.appendingPathComponent(game.id)
        let staging=savedGames.appendingPathComponent(".\(game.id)-new")
        try? fm.removeItem(at:staging)
        try fm.createDirectory(at:staging,withIntermediateDirectories:true)
        try fm.copyItem(at:prefix.appendingPathComponent("drive_c/users"),to:staging.appendingPathComponent("users"))
        if fm.fileExists(atPath:prefix.appendingPathComponent("user.reg").path) {
            try fm.copyItem(at:prefix.appendingPathComponent("user.reg"),to:staging.appendingPathComponent("user.reg"))
        }
        // Temporary files and caches aren't saves.
        if let users=try? fm.contentsOfDirectory(at:staging.appendingPathComponent("users"),includingPropertiesForKeys:nil) {
            for user in users { try? fm.removeItem(at:user.appendingPathComponent("AppData/Local/Temp")) }
        }
        try? (game.name+"\n").write(to:staging.appendingPathComponent("name"),atomically:true,encoding:.utf8)
        try? fm.removeItem(at:target)
        try fm.moveItem(at:staging,to:target)
    }
    /// Deletes the game everywhere. `keep`: saves and settings go to saved-games first.
    static func delete(_ game:Game,keep:Bool,progress:@escaping @Sendable (String)->Void) async throws {
        if keep { progress("Keeping \(game.name)’s saves and settings…");try keepSaves(game) }
        let steamWasRunning=NotProton.steamRunning
        if let id=game.shortcutID {
            if steamWasRunning {
                progress("Closing Steam to remove the game…")
                _ = NotProton.tool("/usr/bin/open",["-g","steam://exit"])
                for _ in 0..<60 where NotProton.steamRunning { try await Task.sleep(for:.milliseconds(500)) }
                guard !NotProton.steamRunning else { throw GameInstaller.Failure.message("Steam didn’t quit, perhaps because a game or download is running. Quit Steam, then try again.\(keep ? " The saves are already kept." : "")") }
                try await Task.sleep(for:.seconds(1))
            }
            progress("Removing \(game.name) from Steam…")
            try removeFromSteam(id)
        }
        progress("Deleting \(game.name)’s files…")
        // Installed elsewhere: the folder there, then the link in Steam's compatdata. Only a folder
        // macSteam made for this game (GameInstaller.placePrefix: <Name>-<id>); a link someone
        // pointed at another folder (a whole games library, say) loses only the link.
        if let real=game.elsewhere,let id=game.shortcutID,real.lastPathComponent.hasSuffix("-\(id)") { try? fm.removeItem(at:real) }
        if let prefix=game.prefix { try? fm.removeItem(at:prefix) }
        if let id=game.shortcutID { try? fm.removeItem(at:NotProton.ours.appendingPathComponent("games/\(id)")) }
        try fm.removeItem(at:game.folder)
        if steamWasRunning { _ = NotProton.tool("/usr/bin/open",[NotProton.steamApp.path]) }
    }
}
