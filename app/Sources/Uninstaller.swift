import Foundation

/// What Uninstall removes outside macSteam's own folders (AppModel.uninstallEverything does the
/// NotProton files, the own Steam, the lock folder, the settings and the app itself). Everything
/// here is something macSteam or its game tools made, recognised by a marker or a name only they
/// use; anything else is left alone. Files go to the Trash, like the rest of Uninstall.
///
/// - macOS shortcuts a game still has switched off (a game tool's shortcuts-taken records, left
///   when the game was killed and its run script never ran again) are switched back on;
/// - game tools in Steam's compatibilitytools.d that carry GameTools.marker;
/// - in every game's compatdata: the tool's logs, perf and capture folders, shortcut records and
///   the GPU-warning marker, and (unless the player keeps them) the Wine prefix (pfx) of every game
///   a tool ran, which the tool's run script made; Steam's then-empty folder for the game goes too;
/// - NotProton's runner copies built from macSteam's variants ("+macvolvo-" in the name), which
///   nothing can use once the variants are gone;
/// - Deadlock's renderer setting back to Vulkan where macSteam had switched it, and the backups;
/// - what the app kept under its old name (MacVolvo): settings, caches, folders with its marker,
///   and NotProton files with its marker;
/// - macOS permissions given to the app (Input Monitoring, microphone): tccutil reset.
enum Uninstaller {
    private static let fm=FileManager.default
    /// Tests: delete instead of moving to the Trash, and leave the real preferences alone.
    nonisolated(unsafe) static var useTrash=true
    nonisolated(unsafe) static var touchDefaults=true
    private static var library:URL { NotProton.home.appendingPathComponent("Library") }

    /// A game running from a game tool or a NotProton runner copy of ours; nil when none is.
    static func runningGame()->String? {
        let p=Process(),pipe=Pipe()
        p.executableURL=URL(fileURLWithPath:"/bin/ps");p.arguments=["-axo","command="];p.standardOutput=pipe
        guard (try? p.run()) != nil else { return nil }
        let out=String(data:pipe.fileHandleForReading.readDataToEndOfFile(),encoding:.utf8) ?? ""
        p.waitUntilExit()
        return out.split(separator:"\n").first { line in
            line.contains("compatibilitytools.d/notproton-steamdriver") || line.contains("+macvolvo-")
        }.map(String.init)
    }

    /// Runs every step; returns one line per thing removed, for the log. With removePrefixes,
    /// the Wine prefix of every game a game tool ran goes too: the tool's run script made it
    /// (wineboot when there was none), and it holds that game's local settings.
    @discardableResult static func run(removePrefixes:Bool=true)->[String] {
        var notes:[String]=[]
        restoreShortcuts(&notes)
        removeGameTools(&notes)
        removeCompatdataFiles(removePrefixes:removePrefixes,&notes)
        removeRunnerCopies(&notes)
        restoreDeadlock(&notes)
        removeLegacy(&notes)
        removeControllerSupport(&notes)
        if touchDefaults { resetPermissions(&notes) }
        return notes
    }

    private static func trash(_ url:URL,_ what:String,_ notes:inout [String]) {
        guard fm.fileExists(atPath:url.path) else { return }
        if (useTrash && (try? fm.trashItem(at:url,resultingItemURL:nil)) != nil) || (try? fm.removeItem(at:url)) != nil {
            notes.append("removed \(what): \(url.path)")
        } else { notes.append("could not remove \(what): \(url.path)") }
    }

    /// More controller support (ControllerSupport.swift): its login item, and Steam's environment
    /// setting, so Steam takes those controllers again from its next start.
    private static func removeControllerSupport(_ notes:inout [String]) {
        trash(ControllerSupport.agent,"controller login item",&notes)
        if touchDefaults,NotProton.tool("/bin/launchctl",["unsetenv",ControllerSupport.variable]) {
            notes.append("cleared \(ControllerSupport.variable) for apps started from now on")
        }
    }

    /// Steam's libraries (its own folder and every path in libraryfolders.vdf).
    static func libraries()->[URL] { NotProton.steamLibraries() }
    private static func compatdataFolders()->[URL] {
        libraries().flatMap { library->[URL] in
            let root=library.appendingPathComponent("steamapps/compatdata")
            return ((try? fm.contentsOfDirectory(at:root,includingPropertiesForKeys:nil)) ?? []).filter { $0.lastPathComponent.allSatisfy(\.isNumber) }
        }
    }

    private static func restoreShortcuts(_ notes:inout [String]) {
        var ids:[Int32]=[]
        for folder in compatdataFolders() {
            let records=folder.appendingPathComponent("shortcuts-taken")
            for file in (try? fm.contentsOfDirectory(at:records,includingPropertiesForKeys:nil)) ?? [] {
                guard let pid=Int32(file.lastPathComponent),kill(pid,0) != 0 || errno == ESRCH else { continue }
                let text=(try? String(contentsOf:file,encoding:.utf8)) ?? ""
                ids+=text.split(whereSeparator:\.isNewline).compactMap { Int32($0) }
            }
        }
        guard !ids.isEmpty else { return }
        KeyOverrides.enableSystemShortcuts(ids)
        notes.append("handed macOS shortcuts \(ids) back to macOS")
    }

    private static func removeGameTools(_ notes:inout [String]) {
        for folder in (try? fm.contentsOfDirectory(at:NotProton.steamTools,includingPropertiesForKeys:nil)) ?? []
        where fm.fileExists(atPath:folder.appendingPathComponent(GameTools.marker).path) {
            trash(folder,"game tool",&notes)
        }
    }

    /// Names only the game tools' run script writes; perf and captures only where its log shows
    /// the tool used that game's folder.
    private static let toolFiles=["steamdriver-run.log","steamdriver-wine.log","steamdriver-wine.log.old",
                                  "steamdriver-gpu-warning-seen","shortcuts-taken","hid-status.json"]
    private static func removeCompatdataFiles(removePrefixes:Bool,_ notes:inout [String]) {
        for folder in compatdataFolders() {
            let usedByTool=fm.fileExists(atPath:folder.appendingPathComponent("steamdriver-run.log").path)
            for name in toolFiles+(usedByTool ? ["perf","captures"]+(removePrefixes ? ["pfx"] : []) : []) {
                trash(folder.appendingPathComponent(name),name=="pfx" ? "game prefix" : "game tool file",&notes)
            }
            // Steam's folder for the game, once nothing is left in it.
            if usedByTool,removePrefixes,
               ((try? fm.contentsOfDirectory(atPath:folder.path)) ?? ["?"]).allSatisfy({ $0 == ".DS_Store" }) {
                trash(folder,"empty game folder",&notes)
            }
        }
    }

    private static func removeRunnerCopies(_ notes:inout [String]) {
        let runners=library.appendingPathComponent("Application Support/notproton/runners")
        for folder in (try? fm.contentsOfDirectory(at:runners,includingPropertiesForKeys:nil)) ?? []
        where folder.lastPathComponent.contains("+macvolvo-") {
            trash(folder,"NotProton runner copy with macSteam's variants",&notes)
        }
    }

    /// Undoes NotProtonExtras.keepDeadlockOnDirectX11: where macSteam switched Deadlock's saved
    /// renderer from Vulkan to DirectX 11 (it kept the original beside it), the renderer line goes
    /// back to Vulkan if it still says DirectX 11; everything else changed since stays. Then the
    /// backup goes.
    private static func restoreDeadlock(_ notes:inout [String]) {
        guard let dx11=try? NSRegularExpression(pattern:#"("DefaultRenderSystemOption"\s+")-dx11(")"#) else { return }
        for library in libraries() {
            let file=library.appendingPathComponent("steamapps/common/Deadlock/game/citadel/cfg/boot.vcfg")
            let backup=file.appendingPathExtension("macvolvo-backup")
            guard fm.fileExists(atPath:backup.path) else { continue }
            if let original=try? String(contentsOf:backup,encoding:.utf8),original.contains("-vulkan"),
               let text=try? String(contentsOf:file,encoding:.utf8) {
                let whole=NSRange(text.startIndex...,in:text)
                if dx11.numberOfMatches(in:text,range:whole)==1,
                   (try? dx11.stringByReplacingMatches(in:text,range:whole,withTemplate:"$1-vulkan$2").write(to:file,atomically:true,encoding:.utf8)) != nil {
                    notes.append("put Deadlock's renderer back to Vulkan: \(file.path)")
                }
            }
            trash(backup,"Deadlock settings backup",&notes)
        }
    }

    private static func removeLegacy(_ notes:inout [String]) {
        let support=library.appendingPathComponent("Application Support")
        for id in [Brand.legacyBundleIdentifier] {
            if touchDefaults,UserDefaults.standard.persistentDomain(forName:id) != nil {
                UserDefaults.standard.removePersistentDomain(forName:id);notes.append("removed the settings of \(id)")
            }
            trash(library.appendingPathComponent("Caches/\(id)"),"old cache",&notes)
            trash(library.appendingPathComponent("Preferences/\(id).plist"),"old settings file",&notes)
        }
        trash(support.appendingPathComponent("MacVolvo NotProton"),"old NotProton folder",&notes)
        let oldData=support.appendingPathComponent(Brand.legacyDataFolderName)
        if fm.fileExists(atPath:oldData.appendingPathComponent(Brand.ownerMarker).path) { trash(oldData,"old own-Steam folder",&notes) }
        for (old,_) in NotProton.legacyFiles where NotProton.ownsFile(old) { trash(old,"old NotProton file",&notes) }
    }

    private static func resetPermissions(_ notes:inout [String]) {
        guard let id=Bundle.main.bundleIdentifier else { return }
        let p=Process()
        p.executableURL=URL(fileURLWithPath:"/usr/bin/tccutil");p.arguments=["reset","All",id]
        p.standardOutput=FileHandle.nullDevice;p.standardError=FileHandle.nullDevice
        if (try? p.run()) != nil { p.waitUntilExit();if p.terminationStatus==0 { notes.append("reset macOS permissions for \(id)") } }
    }
}
