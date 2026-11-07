import Foundation

/// MacVolvo with NotProton (https://github.com/NotProtonNot/NotProton), which turns on Steam Play
/// in the macOS Steam app and runs Windows games in a copy of CrossOver ("runner"). MacVolvo
/// doesn't replace any of it. It adds itself through NotProton's own extension points:
///
/// - a runner variant (notproton/variants/macvolvo.conf) that lays MacVolvo's Mac driver
///   (winemac.so: raw mouse input, High-precision, fullscreen and display fixes) over the
///   runner. NotProton builds that runner copy on first use. The driver is built from the same
///   CrossOver sources as the runner, one per build (Resources/NotProton/<build>/winemac.so),
///   and the variant names its build, so NotProton never applies it to another one;
/// - a Steam compatibility tool, "MacVolvo (CrossOver 26.3)", next to NotProton's own in
///   compatibilitytools.d. Its run script (Resources/NotProton/run.sh) adds the variant and the
///   mouse settings and then runs NotProton's tool unchanged. Chosen as Steam's default tool
///   (Steam › Settings › Compatibility) it applies to every game; per game in its Properties.
///
/// The folder is named notproton-… because NotProton's hooks in Steam only handle tools named
/// that way. NotProton removes such folders when they hold a `flavor` file or its run script,
/// so this one has neither, as the FINALS compatibility package does. Nothing NotProton wrote
/// is changed, and Remove deletes only what carries MacVolvo's marker.
enum NotProton {
    static let projectURL=URL(string:"https://github.com/NotProtonNot/NotProton")!
    private static let fm=FileManager.default
    /// The player's home folder; tests point it at a throwaway one.
    nonisolated(unsafe) static var home=FileManager.default.homeDirectoryForCurrentUser
    private static var library:URL { home.appendingPathComponent("Library/Application Support") }
    static var support:URL { library.appendingPathComponent("notproton") }
    static var steamTools:URL { library.appendingPathComponent("Steam/compatibilitytools.d") }
    /// MacVolvo's own folder for this, apart from the worker's data folder.
    static var ours:URL { library.appendingPathComponent("MacVolvo NotProton") }
    static var settingsFile:URL { ours.appendingPathComponent("settings") }
    private static var variantsFile:URL { support.appendingPathComponent("variants/macvolvo.conf") }
    private static let owner="Written by MacVolvo.app, which also removes it."
    private static let ownerFile=".macvolvo-owner"

    /// One of NotProton's Steam tools, from its registry (notproton/tools: name, build, flavor,
    /// display name, tab-separated).
    struct Tool:Equatable {
        let name, build, flavor, display:String
        var ourName:String { "notproton-macvolvo" + name.dropFirst("notproton".count) }
        var ourDisplay:String { "MacVolvo (\(display))" }
        var variant:String { "macvolvo-" + build }
    }
    enum Status:Equatable {
        case notInstalled
        /// NotProton's runners are builds MacVolvo has no driver for.
        case unsupported(builds:[String])
        case available([Tool])
        case added([Tool])
    }

    static var installed:Bool { fm.fileExists(atPath:support.appendingPathComponent("tools").path) }
    private static func valid(_ s:Substring)->Bool {
        !s.isEmpty && s.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "._-".contains($0)) }
    }
    static func tools()->[Tool] {
        guard let text=try? String(contentsOf:support.appendingPathComponent("tools"),encoding:.utf8) else { return [] }
        return text.split(separator:"\n").compactMap { line in
            let f=line.split(separator:"\t",omittingEmptySubsequences:false)
            guard f.count >= 4, f[0].hasPrefix("notproton"), valid(f[0]), valid(f[1]), valid(f[2]) else { return nil }
            guard fm.isExecutableFile(atPath:steamTools.appendingPathComponent(String(f[0])+"/run").path) else { return nil }
            return Tool(name:String(f[0]),build:String(f[1]),flavor:String(f[2]),
                        display:String(f[3]).filter { $0 != "\"" && $0 != "\\" && !$0.isNewline })
        }
    }
    /// The builds this app has a driver for.
    static func drivers(_ resources:URL)->Set<String> {
        let folder=resources.appendingPathComponent("NotProton")
        let names=(try? fm.contentsOfDirectory(atPath:folder.path)) ?? []
        return Set(names.filter { fm.fileExists(atPath:folder.appendingPathComponent($0+"/winemac.so").path) })
    }
    /// The driver is x86_64 (Rosetta); NotProton's FEX runners run arm64 Wine.
    static func supported(_ resources:URL)->[Tool] {
        let builds=drivers(resources)
        return tools().filter { $0.flavor == "rosetta" && builds.contains($0.build) }
    }
    static func isAdded(_ tool:Tool)->Bool {
        fm.fileExists(atPath:steamTools.appendingPathComponent(tool.ourName+"/"+ownerFile).path)
    }
    static func status(_ resources:URL)->Status {
        guard installed else { return .notInstalled }
        let tools=supported(resources)
        if tools.isEmpty { return .unsupported(builds:Self.tools().map(\.build)) }
        return tools.contains(where:isAdded) ? .added(tools) : .available(tools)
    }
    static var steamRunning:Bool {
        let task=Process();task.executableURL=URL(fileURLWithPath:"/usr/bin/pgrep");task.arguments=["-x","steam_osx"]
        task.standardOutput=FileHandle.nullDevice;task.standardError=FileHandle.nullDevice
        guard (try? task.run()) != nil else { return false }
        task.waitUntilExit();return task.terminationStatus == 0
    }

    /// The mouse settings the run script reads (only these names; see run.sh).
    static func settings(mouse:AppModel.MouseInput, rate:AppModel.MouseRate)->String {
        // Every program but Steam's own and Wine's desktop, as portable_steam.h mouselookList().
        var lines=["WINEMAC_MOUSELOOK=*;!steam.exe;!steamwebhelper.exe;!steamwebhelper_real.exe;!steamservice.exe;"
                   + "!steamerrorreporter.exe;!steamerrorreporter64.exe;!gameoverlayui.exe;!gameoverlayui64.exe;!explorer.exe"]
        if mouse == .classic { lines.append("WINEMAC_INPUT_VARIANT=classic") }
        else {
            lines.append("WINEMAC_INPUT_VARIANT=precise")
            if mouse == .highPrecision { lines.append("WINEMAC_RAW_SOURCE=hid") }
            lines.append("WINEMAC_MOUSELOOK_RAW_INTERVAL_US=" + (rate.perSecond.map { String(1_000_000/$0) } ?? "0"))
        }
        return lines.joined(separator:"\n")+"\n"
    }

    /// Adds (or brings up to date) MacVolvo's driver, variant and tool for every supported runner.
    static func add(resources:URL, settings:String) throws {
        let tools=supported(resources)
        guard !tools.isEmpty else { throw failure("NotProton has no CrossOver build this version of MacVolvo has a driver for.") }
        guard let template=try? String(contentsOf:resources.appendingPathComponent("NotProton/run.sh"),encoding:.utf8) else {
            throw failure("This copy of MacVolvo is missing its NotProton files. Download it again.")
        }
        // Refuse before writing anything when a file in MacVolvo's place isn't MacVolvo's.
        if fm.fileExists(atPath:variantsFile.path) && !ownsVariants() { throw failure("\(variantsFile.path) wasn't written by MacVolvo, so nothing was changed.") }
        for tool in tools {
            let folder=steamTools.appendingPathComponent(tool.ourName)
            if fm.fileExists(atPath:folder.path) && !isAdded(tool) { throw failure("\(folder.path) wasn't made by MacVolvo, so nothing was changed.") }
        }
        try fm.createDirectory(at:ours,withIntermediateDirectories:true)
        var variants="# \(owner)\n# MacVolvo's Mac driver over NotProton's CrossOver runner (MacVolvo › Settings › General).\n"
        for tool in tools {
            let driver=ours.appendingPathComponent(tool.build)
            try fm.createDirectory(at:driver,withIntermediateDirectories:true)
            try replace(driver.appendingPathComponent("winemac.so")) { try fm.copyItem(at:resources.appendingPathComponent("NotProton/\(tool.build)/winemac.so"),to:$0) }
            variants += """

            [\(tool.variant)]
            label = MacVolvo mouse and display driver
            variant = MacVolvo
            src = \(driver.path)
            dst = lib/wine/x86_64-unix
            build = \(tool.build)

            """
        }
        try write(settings,to:settingsFile)
        try fm.createDirectory(at:variantsFile.deletingLastPathComponent(),withIntermediateDirectories:true)
        try write(variants,to:variantsFile)
        for tool in tools {
            let folder=steamTools.appendingPathComponent(tool.ourName)
            let stage=steamTools.appendingPathComponent(".\(tool.ourName).\(UUID().uuidString)")
            try fm.createDirectory(at:stage,withIntermediateDirectories:true)
            do {
                try write(owner+"\n",to:stage.appendingPathComponent(ownerFile))
                try write("\"manifest\"\n{\n  \"version\" \"2\"\n  \"commandline\" \"/run %verb%\"\n}\n",to:stage.appendingPathComponent("toolmanifest.vdf"))
                try write("""
                "compatibilitytools"
                {
                  "compat_tools"
                  {
                    "\(tool.ourName)"
                    {
                      "install_path" "."
                      "display_name" "\(tool.ourDisplay)"
                      "from_oslist" "windows"
                      "to_oslist" "macos"
                    }
                  }
                }

                """,to:stage.appendingPathComponent("compatibilitytool.vdf"))
                let run=stage.appendingPathComponent("run")
                try write(template.replacingOccurrences(of:"@NP_TOOL@",with:tool.name).replacingOccurrences(of:"@VARIANT@",with:tool.variant),to:run)
                try fm.setAttributes([.posixPermissions:0o755],ofItemAtPath:run.path)
                if fm.fileExists(atPath:folder.path) { try fm.removeItem(at:folder) }
                try fm.moveItem(at:stage,to:folder)
            } catch { try? fm.removeItem(at:stage);throw error }
        }
    }
    /// The mouse settings changed: the next launch reads them; Steam needn't restart.
    static func updateSettings(_ settings:String) {
        guard fm.fileExists(atPath:ours.path) else { return }
        try? write(settings,to:settingsFile)
    }
    /// Removes only what carries MacVolvo's marker: its tools, its variant file and its folder.
    static func remove() throws {
        for name in (try? fm.contentsOfDirectory(atPath:steamTools.path)) ?? [] where name.hasPrefix("notproton-macvolvo") {
            let folder=steamTools.appendingPathComponent(name)
            if fm.fileExists(atPath:folder.appendingPathComponent(ownerFile).path) { try fm.removeItem(at:folder) }
        }
        if ownsVariants() { try fm.removeItem(at:variantsFile) }
        if fm.fileExists(atPath:ours.path) { try fm.removeItem(at:ours) }
    }
    /// After an update of MacVolvo: puts its current driver and run script in place, if added.
    static func refresh(resources:URL, settings:String) {
        guard case .added=status(resources) else { return }
        try? add(resources:resources,settings:settings)
    }

    private static func ownsVariants()->Bool {
        (try? String(contentsOf:variantsFile,encoding:.utf8))?.hasPrefix("# "+owner) == true
    }
    private static func write(_ text:String, to url:URL) throws {
        try replace(url) { try Data(text.utf8).write(to:$0) }
    }
    /// Writes beside the target, then swaps it in, so a game starting now never reads half a file.
    private static func replace(_ url:URL, _ make:(URL) throws -> Void) throws {
        let temp=url.deletingLastPathComponent().appendingPathComponent(".\(url.lastPathComponent).\(UUID().uuidString)")
        do {
            try make(temp)
            if fm.fileExists(atPath:url.path) { _ = try fm.replaceItemAt(url,withItemAt:temp) } else { try fm.moveItem(at:temp,to:url) }
        } catch { try? fm.removeItem(at:temp);throw error }
    }
    private static func failure(_ message:String)->NSError {
        NSError(domain:"MacVolvo.NotProton",code:1,userInfo:[NSLocalizedDescriptionKey:message])
    }
}
