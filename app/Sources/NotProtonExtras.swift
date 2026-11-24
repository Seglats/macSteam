import Foundation

/// The renderers and game profiles macSteam adds to NotProton (NotProton.swift).
///
/// Renderers, one "SteamDriver renderer" dropdown in a game's Compatibility page, combined with the
/// "SteamDriver driver" one:
/// - SteamDriver DXMT: macSteam's DXMT (Recall's, which draws through the driver's fullscreen canvas),
///   bundled with the app per CrossOver build (Resources/NotProton/<build>/dxmt), over the runner's
///   lib/dxmt and its lib/wine copy of winemetal.dll. The game uses it with DXMT as NotProton's
///   graphics backend;
/// - Apple D3DMetal: from the player's own Game Porting Toolkit in SteamDriver/GPTK (macSteam
///   ships no D3DMetal), checked (Apple's signature, D3DMetal 4, modules) and laid
///   unchanged over the runner's lib64/apple_gptk. Used with D3DMetal as the graphics backend.
///
/// Profiles (Profiles.swift): Global DX11 and each DX11 game profile add a row that points
/// SteamDriver DXMT at the profile's settings; a game's row only shows in that game (`apps`).
/// Deadlock's profile also keeps its saved renderer on DirectX 11 in every Steam library: a saved
/// Vulkan choice would bypass DXMT and D3DMetal alike.
extension NotProton {
    // MARK: SteamDriver DXMT
    static func dxmtVariant(_ tool:Tool)->String { "macvolvo-dxmt-" + tool.build }
    static func dxmtTree(_ tool:Tool)->URL { ours.appendingPathComponent(tool.build+"/dxmt") }
    static func bundledDXMT(_ resources:URL, _ tool:Tool)->URL { resources.appendingPathComponent("NotProton/\(tool.build)/dxmt") }

    // MARK: Apple's Game Porting Toolkit (the player's own)
    /// The player's own copy of Apple's Game Porting Toolkit 4, copied by hand into SteamDriver/GPTK
    /// (Show Folder): the redist folder from Apple's "Evaluation environment for Windows games"
    /// image (or its lib folder, or lib's contents; gptkLib finds Apple's files in any of these).
    /// macSteam ships no D3DMetal and installs nothing of Apple's itself: the player gets it from
    /// Apple, whose image shows Apple's license when they open it. gptkStatus checks the folder
    /// before anything uses it. Used unchanged by the "Apple D3DMetal" variant (over the runner's
    /// lib64/apple_gptk) and by the SteamDriver tool, whose run script links its runtime's
    /// lib/apple-gptk to gptkLib.
    static var gptkFolder:URL { ours.appendingPathComponent("GPTK") }
    /// Where Apple's redist/lib contents are inside the folder.
    static var gptkLib:URL {
        for nested in ["redist/lib","lib"] where FileManager.default.fileExists(atPath:gptkFolder.appendingPathComponent(nested+"/external").path) {
            return gptkFolder.appendingPathComponent(nested)
        }
        return gptkFolder
    }
    static let d3dmetalVariant="macvolvo-d3dmetal"
    /// Apple's modules the SteamDriver tool and the variant load; a folder without them isn't used.
    static let gptkRequired=["external/D3DMetal.framework/Versions/A/D3DMetal","external/libd3dshared.dylib",
        "wine/x86_64-windows/d3d11.dll","wine/x86_64-windows/dxgi.dll","wine/x86_64-windows/d3d12.dll",
        "wine/x86_64-windows/d3d12core.dll","wine/x86_64-unix/d3d11.so","wine/x86_64-unix/dxgi.so",
        "wine/x86_64-unix/d3d12.so","wine/x86_64-unix/d3d12core.so"]
    enum GPTKStatus:Equatable {
        case empty
        case missing([String])
        case notApple(String)
        case tooOld(String)
        case ready(String)
        var version:String? { if case .ready(let v)=self { return v };return nil }
    }
    /// The D3DMetal version in a redist/lib-shaped folder, when it is Apple's D3DMetal.
    static func d3dmetalVersion(in lib:URL)->String? {
        let framework=lib.appendingPathComponent("external/D3DMetal.framework")
        for plist in ["Resources/Info.plist","Versions/A/Resources/Info.plist"] {
            if let info=NSDictionary(contentsOf:framework.appendingPathComponent(plist)),
               info["CFBundleIdentifier"] as? String == "com.apple.D3DMetal",
               let version=info["CFBundleShortVersionString"] as? String { return version }
        }
        return nil
    }
    nonisolated(unsafe) private static var gptkCache:(stamp:String,status:GPTKStatus)?
    /// Checks the folder: every required module, D3DMetal 4 or later, and Apple's signature on
    /// every native file (cached until the folder changes, as codesign takes a moment).
    static func gptkStatus()->GPTKStatus {
        let fm=FileManager.default
        let names=(try? fm.contentsOfDirectory(atPath:gptkFolder.path))?.filter { $0 != ".DS_Store" && !$0.hasSuffix(".txt") } ?? []
        if names.isEmpty { return .empty }
        let lib=gptkLib
        let missing=gptkRequired.filter { !fm.fileExists(atPath:lib.appendingPathComponent($0).path) }
        if !missing.isEmpty { return .missing(missing) }
        guard let version=d3dmetalVersion(in:lib) else { return .notApple("D3DMetal.framework") }
        guard let major=Int(version.prefix { $0.isNumber }),major >= 4 else { return .tooOld(version) }
        let stamp=lib.path+gptkRequired.map { path in
            let a=try? fm.attributesOfItem(atPath:lib.appendingPathComponent(path).path)
            return "\(path)\((a?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0)\((a?[.size] as? Int) ?? 0)"
        }.joined()
        if let cache=gptkCache,cache.stamp==stamp { return cache.status }
        var status=GPTKStatus.ready(version)
        if let bad=nativeFiles(lib).first(where: { !tool("/usr/bin/codesign",["--verify","--strict","-R=anchor apple",$0]) }) {
            status = .notApple((bad as NSString).lastPathComponent)
        }
        gptkCache=(stamp,status)
        return status
    }
    /// The framework and every Mach-O file outside it (Apple signs both).
    private static func nativeFiles(_ lib:URL)->[String] {
        var native=[lib.appendingPathComponent("external/D3DMetal.framework").path]
        guard let walk=FileManager.default.enumerator(atPath:lib.path) else { return native }
        for case let path as String in walk where !path.contains(".framework/") {
            let url=lib.appendingPathComponent(path)
            guard (try? FileManager.default.destinationOfSymbolicLink(atPath:url.path)) == nil,
                  let handle=FileHandle(forReadingAtPath:url.path) else { continue }
            let magic=handle.readData(ofLength:4);try? handle.close()
            if magic == Data([0xcf,0xfa,0xed,0xfe]) || magic == Data([0xca,0xfe,0xba,0xbe]) { native.append(url.path) }
        }
        return native
    }
    /// Makes the folder (with a note of the steps) and returns it, for Show Folder.
    static func prepareGPTKFolder() throws -> URL {
        try FileManager.default.createDirectory(at:gptkFolder,withIntermediateDirectories:true)
        let readme=gptkFolder.appendingPathComponent("How to add Apple’s Game Porting Toolkit.txt")
        if !FileManager.default.fileExists(atPath:readme.path) {
            try Data("""
            Apple's Game Porting Toolkit for macSteam

            1. Download Game Porting Toolkit 4 from Apple:
               https://developer.apple.com/games/game-porting-toolkit/
            2. Open the Game Porting Toolkit disk image, then open
               "Evaluation environment for Windows games" in it.
            3. Copy its redist folder into this folder.

            macSteam checks the files (Apple's signature, D3DMetal 4) and shows a check mark when
            they're ready. You can delete this note.

            """.utf8).write(to:readme)
        }
        return gptkFolder
    }

    // MARK: Profiles (Profiles.swift)
    /// The Windows path DXMT reads its profile from (DXMT_CONFIG_FILE).
    static func windowsPath(_ url:URL)->String { "Z:" + url.path.replacingOccurrences(of:"/",with:"\\") }
    /// Steam's libraries: its own folder and every path in libraryfolders.vdf.
    static func steamLibraries()->[URL] {
        let steam=home.appendingPathComponent("Library/Application Support/Steam")
        var found=[steam]
        if let vdf=try? String(contentsOf:steam.appendingPathComponent("steamapps/libraryfolders.vdf"),encoding:.utf8),
           let paths=try? NSRegularExpression(pattern:#""path"\s+"([^"]+)""#) {
            for match in paths.matches(in:vdf,range:NSRange(vdf.startIndex...,in:vdf)) {
                if let range=Range(match.range(at:1),in:vdf) { found.append(URL(fileURLWithPath:String(vdf[range]).replacingOccurrences(of:"\\\\",with:"\\"))) }
            }
        }
        var seen=Set<String>()
        return found.map(\.standardizedFileURL).filter { seen.insert($0.path).inserted }
    }

    /// Deadlock saves the renderer chosen in its Video settings in boot.vcfg, and a saved Vulkan
    /// choice bypasses DXMT and D3DMetal. Switches it back to DirectX 11 in every Steam library,
    /// keeping the first original beside it, as portable_steam.h configureDeadlockRenderer does
    /// in macSteam's own Steam. Returns how many it changed.
    @discardableResult static func keepDeadlockOnDirectX11()->Int {
        let fm=FileManager.default
        guard let vulkan=try? NSRegularExpression(pattern:#"("DefaultRenderSystemOption"\s+")-vulkan(")"#) else { return 0 }
        var changed=0
        for library in steamLibraries() {
            let file=library.appendingPathComponent("steamapps/common/Deadlock/game/citadel/cfg/boot.vcfg")
            guard let text=try? String(contentsOf:file,encoding:.utf8) else { continue }
            let whole=NSRange(text.startIndex...,in:text)
            guard vulkan.numberOfMatches(in:text,range:whole) == 1 else { continue }
            let backup=file.appendingPathExtension("macvolvo-backup")
            if !fm.fileExists(atPath:backup.path) { guard (try? fm.copyItem(at:file,to:backup)) != nil else { continue } }
            let fixed=vulkan.stringByReplacingMatches(in:text,range:whole,withTemplate:"$1-dx11$2")
            if (try? fixed.write(to:file,atomically:true,encoding:.utf8)) != nil { changed += 1 }
        }
        return changed
    }

    // MARK: The rows and variants they add
    static func extraVariants(_ tools:[Tool])->String {
        var text=""
        for tool in tools {
            text += """

            [\(dxmtVariant(tool))]
            label = SteamDriver DXMT
            variant = SteamDriver renderer
            src = \(dxmtTree(tool).path)
            dst = lib
            build = \(tool.build)

            """
        }
        if let version=gptkStatus().version {
            text += """

            [\(d3dmetalVariant)]
            label = Apple D3DMetal \(version) (your Game Porting Toolkit)
            variant = SteamDriver renderer
            src = \(gptkLib.path)
            dst = lib64/apple_gptk

            """
        }
        return text
    }
    /// NotProton shows at most 32 rows across every plugin file; the mouse takes 4, so profile
    /// rows stop well short of that, leaving room for other plugins.
    static let maxProfileRows=16
    /// The row id a profile keeps in Steam: CS2's and Deadlock's from before profiles, so games
    /// keep it switched on.
    static func rowKey(_ p:Profiles.Profile)->String {
        switch p.appID {
        case nil: return "NOTPROTON_MACVOLVO_PROFILE_" + p.kind.rawValue.uppercased()
        case "730": return "NOTPROTON_MACVOLVO_PROFILE_CS2"
        case Profiles.deadlockAppID: return "NOTPROTON_MACVOLVO_PROFILE_DEADLOCK"
        case let app?: return "NOTPROTON_MACVOLVO_PROFILE_APP_" + app
        }
    }
    /// The profiles that become rows: those with renderer settings (DX11; DX12 has none to set
    /// yet), Global DX11 first and game profiles after it, so a game's DXMT_CONFIG_FILE wins
    /// where both are on (NotProton passes plugin env lines in file order, the last one wins).
    static func rowProfiles()->[Profiles.Profile] {
        let dx11=Profiles.list().filter { $0.kind == .dx11 && FileManager.default.fileExists(atPath:Profiles.dxmtFile($0.id).path) }
        return Array((dx11.filter(\.isGlobal)+dx11.filter { !$0.isGlobal }).prefix(maxProfileRows))
    }
    static func profileRows(_ tools:[Tool])->String {
        let toolLines=tools.map { "tools = \($0.name)" }.joined(separator:"\n")
        return rowProfiles().map { p in
            let file=Profiles.dxmtFile(p.id)
            let label=p.isGlobal ? "SteamDriver DX11 profile" : "SteamDriver profile: \(p.name)"
            let what=p.isGlobal ? "macSteam’s Global DX11 profile: SteamDriver DXMT’s renderer settings for every DX11 game. A game’s own profile wins over it."
                : "macSteam’s profile for \(p.name): SteamDriver DXMT’s renderer settings for this game, over Global DX11."
            return """

            [\(rowKey(p))]
            label = \(label)
            description = \(what) Needs the SteamDriver DXMT renderer and DXMT as the graphics backend.
            requires = \(file.path)
            \(toolLines)\(p.appID.map { "\napps = \($0)" } ?? "")
            env = DXMT_CONFIG_FILE=\(windowsPath(file))

            """
        }.joined()
    }

    static func tool(_ path:String,_ arguments:[String])->Bool {
        let task=Process();task.executableURL=URL(fileURLWithPath:path);task.arguments=arguments
        task.standardOutput=FileHandle.nullDevice;task.standardError=FileHandle.nullDevice
        guard (try? task.run()) != nil else { return false }
        task.waitUntilExit();return task.terminationStatus == 0
    }

    // MARK: Restarting Steam
    /// The Steam app NotProton patched, which loads NotProton when opened: NotProton's own setting,
    /// else /Applications/Steam.app (SupportPaths.Steam.app in NotProton).
    static var steamApp:URL {
        let path=CFPreferencesCopyAppValue("steamAppPath" as CFString,"com.notproton.NotProton" as CFString) as? String
        return URL(fileURLWithPath:path ?? "/Applications/Steam.app")
    }
    /// Asks Steam to quit the way NotProton does (steam://exit), waits up to 30 seconds, then opens
    /// it again. Never forces it: a game or download in progress keeps Steam open, and false comes back.
    static func restartSteam()->Bool {
        if steamRunning {
            _ = tool("/usr/bin/open",["-g","steam://exit"])
            for _ in 0..<60 where steamRunning { Thread.sleep(forTimeInterval:0.5) }
            guard !steamRunning else { return false }
            Thread.sleep(forTimeInterval:1)
        }
        return tool("/usr/bin/open",[steamApp.path])
    }
}
