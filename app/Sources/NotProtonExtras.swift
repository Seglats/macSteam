import Foundation

/// The renderers and game profiles MacVolvo adds to NotProton (NotProton.swift).
///
/// Renderers, one "MacVolvo renderer" dropdown in a game's Compatibility page, combined with the
/// "MacVolvo driver" one:
/// - MacVolvo DXMT: MacVolvo's DXMT (Recall's, which draws through the driver's fullscreen canvas),
///   bundled with the app per CrossOver build (Resources/NotProton/<build>/dxmt), over the runner's
///   lib/dxmt and its lib/wine copy of winemetal.dll. The game uses it with DXMT as NotProton's
///   graphics backend;
/// - Apple D3DMetal: from the player's own Game Porting Toolkit image, which Apple doesn't let
///   MacVolvo ship. Checked (Apple's signature, D3DMetal's identity and modules) and copied
///   unchanged into MacVolvo's folder, over the runner's lib64/apple_gptk. Used with D3DMetal as the
///   graphics backend.
///
/// Game profiles, chosen in MacVolvo: each adds a row for its game that points MacVolvo DXMT at the
/// game's renderer profile (Resources/NotProton/profiles). Deadlock's also keeps its saved renderer
/// on DirectX 11 in every Steam library: a saved Vulkan choice would bypass DXMT and D3DMetal alike.
extension NotProton {
    enum Profile:String, CaseIterable, Identifiable {
        case cs2, deadlock
        var id:String { rawValue }
        var title:String { self == .cs2 ? "Counter-Strike 2" : "Deadlock" }
        var exe:String { rawValue + ".exe" }
        var key:String { "NOTPROTON_MACVOLVO_PROFILE_" + rawValue.uppercased() }
        var detail:String {
            switch self {
            case .cs2: return "MacVolvo DXMT’s renderer settings for CS2: low frame latency, input sampled later each frame, four shader compiler threads."
            case .deadlock: return "MacVolvo DXMT’s renderer settings for Deadlock, and Deadlock kept on DirectX 11: a saved Vulkan choice would bypass DXMT and D3DMetal."
            }
        }
    }

    // MARK: MacVolvo DXMT
    static func dxmtVariant(_ tool:Tool)->String { "macvolvo-dxmt-" + tool.build }
    static func dxmtTree(_ tool:Tool)->URL { ours.appendingPathComponent(tool.build+"/dxmt") }
    static func bundledDXMT(_ resources:URL, _ tool:Tool)->URL { resources.appendingPathComponent("NotProton/\(tool.build)/dxmt") }

    // MARK: Apple D3DMetal
    static let d3dmetalVariant="macvolvo-d3dmetal"
    static var d3dmetalFolder:URL { ours.appendingPathComponent("d3dmetal") }
    /// What lands in the runner's lib64/apple_gptk: Apple's redist/lib, unchanged.
    static var d3dmetalTree:URL { d3dmetalFolder.appendingPathComponent("apple_gptk") }
    static var d3dmetalVersion:String? {
        (try? String(contentsOf:d3dmetalFolder.appendingPathComponent("version"),encoding:.utf8))?
            .trimmingCharacters(in:.whitespacesAndNewlines)
    }
    /// Imports D3DMetal from the player's "Evaluation environment for Windows games" image (Apple's
    /// Game Porting Toolkit). Nothing Apple made is changed; a bad image changes nothing.
    static func importGPTK(_ image:URL) throws -> String {
        let fm=FileManager.default
        let mount=fm.temporaryDirectory.appendingPathComponent("macvolvo-gptk-\(UUID().uuidString)")
        try fm.createDirectory(at:mount,withIntermediateDirectories:true)
        defer { try? fm.removeItem(at:mount) }
        guard tool("/usr/bin/hdiutil",["attach","-readonly","-nobrowse","-noautoopen","-mountpoint",mount.path,image.path]) else {
            throw failure("That file couldn’t be opened as a disk image.")
        }
        defer { _ = tool("/usr/bin/hdiutil",["detach",mount.path,"-force"]) }
        let redist=mount.appendingPathComponent("redist/lib")
        let framework=redist.appendingPathComponent("external/D3DMetal.framework")
        guard let info=NSDictionary(contentsOf:framework.appendingPathComponent("Resources/Info.plist")),
              info["CFBundleIdentifier"] as? String == "com.apple.D3DMetal",
              let version=info["CFBundleShortVersionString"] as? String else {
            throw failure("That image has no D3DMetal. Choose Apple’s \u{201C}Evaluation environment for Windows games\u{201D} image from the Game Porting Toolkit.")
        }
        guard let major=Int(version.prefix { $0.isNumber }),major >= 3 else { throw failure("D3DMetal \(version) is too old. Use Game Porting Toolkit 3 or later.") }
        for name in ["external/libd3dshared.dylib","wine/x86_64-windows/d3d11.dll","wine/x86_64-windows/dxgi.dll",
                     "wine/x86_64-unix/d3d11.so","wine/x86_64-unix/dxgi.so"] where !fm.fileExists(atPath:redist.appendingPathComponent(name).path) {
            throw failure("That image is missing Apple’s \(name).")
        }
        // Apple's signature on every native file, and no link out of the package.
        var native=[framework.path]
        guard let walk=fm.enumerator(atPath:redist.path) else { throw failure("That image’s D3DMetal folder couldn’t be read.") }
        for case let path as String in walk {
            let url=redist.appendingPathComponent(path)
            if let target=try? fm.destinationOfSymbolicLink(atPath:url.path) {
                guard url.deletingLastPathComponent().appendingPathComponent(target).standardizedFileURL.path.hasPrefix(redist.standardizedFileURL.path) else {
                    throw failure("That image has a link out of Apple’s package, so it wasn’t used.")
                }
                continue
            }
            if !path.contains(".framework/"),let handle=FileHandle(forReadingAtPath:url.path) {
                let magic=handle.readData(ofLength:4);try? handle.close()
                if magic == Data([0xcf,0xfa,0xed,0xfe]) || magic == Data([0xca,0xfe,0xba,0xbe]) { native.append(url.path) }
            }
        }
        for path in native where !tool("/usr/bin/codesign",["--verify","--strict","-R=anchor apple",path]) {
            throw failure("\((path as NSString).lastPathComponent) isn’t signed by Apple, so it wasn’t used.")
        }
        // Copied whole beside the old copy, then swapped in.
        try fm.createDirectory(at:ours,withIntermediateDirectories:true)
        let stage=ours.appendingPathComponent(".d3dmetal-\(UUID().uuidString)")
        do {
            guard tool("/usr/bin/ditto",[redist.path,stage.appendingPathComponent("apple_gptk").path]) else { throw failure("D3DMetal couldn’t be copied.") }
            let licenses=stage.appendingPathComponent("licenses")
            try fm.createDirectory(at:licenses,withIntermediateDirectories:true)
            for name in ["Read Me.rtf","License.rtf","Acknowledgements.rtf"] where fm.fileExists(atPath:mount.appendingPathComponent(name).path) {
                try fm.copyItem(at:mount.appendingPathComponent(name),to:licenses.appendingPathComponent(name))
            }
            try Data((version+"\n").utf8).write(to:stage.appendingPathComponent("version"))
            if fm.fileExists(atPath:d3dmetalFolder.path) { try fm.removeItem(at:d3dmetalFolder) }
            try fm.moveItem(at:stage,to:d3dmetalFolder)
        } catch { try? fm.removeItem(at:stage);throw error }
        return version
    }
    static func removeGPTK() throws {
        if FileManager.default.fileExists(atPath:d3dmetalFolder.path) { try FileManager.default.removeItem(at:d3dmetalFolder) }
    }

    // MARK: Game profiles
    static var profilesFolder:URL { ours.appendingPathComponent("profiles") }
    static func profileFile(_ profile:Profile)->URL { profilesFolder.appendingPathComponent(profile.rawValue+".conf") }
    static var installedProfiles:[Profile] {
        Profile.allCases.filter { FileManager.default.fileExists(atPath:profileFile($0).path) }
    }
    static func setProfile(_ profile:Profile, installed on:Bool, resources:URL) throws {
        let fm=FileManager.default
        if on {
            try fm.createDirectory(at:profilesFolder,withIntermediateDirectories:true)
            if fm.fileExists(atPath:profileFile(profile).path) { try fm.removeItem(at:profileFile(profile)) }
            try fm.copyItem(at:resources.appendingPathComponent("NotProton/profiles/\(profile.rawValue).conf"),to:profileFile(profile))
            if profile == .deadlock { _ = keepDeadlockOnDirectX11() }
        } else if fm.fileExists(atPath:profileFile(profile).path) {
            try fm.removeItem(at:profileFile(profile))
        }
    }
    /// The Windows path DXMT reads its profile from (DXMT_CONFIG_FILE).
    static func windowsPath(_ url:URL)->String { "Z:" + url.path.replacingOccurrences(of:"/",with:"\\") }

    /// Deadlock saves the renderer chosen in its Video settings in boot.vcfg, and a saved Vulkan
    /// choice bypasses DXMT and D3DMetal. Switches it back to DirectX 11 in every Steam library,
    /// keeping the first original beside it, as portable_steam.h configureDeadlockRenderer does
    /// in MacVolvo's own Steam. Returns how many it changed.
    @discardableResult static func keepDeadlockOnDirectX11()->Int {
        let fm=FileManager.default
        let steam=home.appendingPathComponent("Library/Application Support/Steam")
        var libraries=[steam]
        if let vdf=try? String(contentsOf:steam.appendingPathComponent("steamapps/libraryfolders.vdf"),encoding:.utf8),
           let paths=try? NSRegularExpression(pattern:#""path"\s+"([^"]+)""#) {
            for match in paths.matches(in:vdf,range:NSRange(vdf.startIndex...,in:vdf)) {
                if let range=Range(match.range(at:1),in:vdf) { libraries.append(URL(fileURLWithPath:String(vdf[range]).replacingOccurrences(of:"\\\\",with:"\\"))) }
            }
        }
        guard let vulkan=try? NSRegularExpression(pattern:#"("DefaultRenderSystemOption"\s+")-vulkan(")"#) else { return 0 }
        var changed=0
        for library in Set(libraries.map(\.standardizedFileURL)) {
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
            label = MacVolvo DXMT
            variant = MacVolvo renderer
            src = \(dxmtTree(tool).path)
            dst = lib
            build = \(tool.build)

            """
        }
        if let version=d3dmetalVersion {
            text += """

            [\(d3dmetalVariant)]
            label = Apple D3DMetal \(version) (your Game Porting Toolkit)
            variant = MacVolvo renderer
            src = \(d3dmetalTree.path)
            dst = lib64/apple_gptk

            """
        }
        return text
    }
    static func profileRows(_ tools:[Tool])->String {
        let toolLines=tools.map { "tools = \($0.name)" }.joined(separator:"\n")
        return installedProfiles.map { profile in
            """

            [\(profile.key)]
            label = MacVolvo \(profile.title) profile
            description = \(profile.detail) Needs the MacVolvo DXMT renderer and DXMT as the graphics backend.
            requires = \(profileFile(profile).path)
            \(toolLines)
            env = DXMT_CONFIG_FILE=\(windowsPath(profileFile(profile)))

            """
        }.joined()
    }

    static func tool(_ path:String,_ arguments:[String])->Bool {
        let task=Process();task.executableURL=URL(fileURLWithPath:path);task.arguments=arguments
        task.standardOutput=FileHandle.nullDevice;task.standardError=FileHandle.nullDevice
        guard (try? task.run()) != nil else { return false }
        task.waitUntilExit();return task.terminationStatus == 0
    }

    // MARK: NotProton's own D3DMetal
    /// The D3DMetal in NotProton's CrossOver for that build, if it has one: offered by NotProton itself
    /// with D3DMetal as the graphics backend, no MacVolvo renderer needed.
    static func runnerD3DMetalVersion(_ tool:Tool)->String? {
        let framework=support.appendingPathComponent("runners/crossover-\(tool.build)/CrossOver/lib64/apple_gptk/external/D3DMetal.framework")
        for plist in ["Resources/Info.plist","Versions/A/Resources/Info.plist"] {
            if let info=NSDictionary(contentsOf:framework.appendingPathComponent(plist)),
               info["CFBundleIdentifier"] as? String == "com.apple.D3DMetal",let version=info["CFBundleShortVersionString"] as? String { return version }
        }
        return nil
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
