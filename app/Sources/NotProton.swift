import Foundation

/// MacVolvo with NotProton (https://github.com/NotProtonNot/NotProton), which turns on Steam Play
/// in the macOS Steam app and runs Windows games in a copy of CrossOver ("runner"). MacVolvo
/// replaces nothing of it: no tool, no runner, no CrossOver. It adds options to each game's
/// Compatibility page in Steam, through NotProton's own extension points (its conf-format.md):
///
/// - runner variants (notproton/variants/macvolvo.conf), laid over NotProton's runner in a copy
///   NotProton builds on first use. "MacVolvo driver": MacVolvo's Mac driver (winemac.so: raw mouse
///   input, High-precision, fullscreen and display fixes), compiled from the same CrossOver sources
///   as the runner, one per build (Resources/NotProton/<build>/winemac.so), naming its build so
///   NotProton never applies it to another. "MacVolvo renderer": MacVolvo DXMT, or Apple's D3DMetal
///   from the player's own Game Porting Toolkit (NotProtonExtras.swift);
/// - plugin rows (notproton/plugins/macvolvo.conf): "MacVolvo mouse", with High-precision,
///   Classic timing and Mouse updates under it, and the game profiles chosen in MacVolvo. Each sets
///   variables for that game only, and only for the CrossOver tools the driver is built for.
///
/// Both files say MacVolvo wrote them; NotProton never writes to those folders. Remove deletes
/// only what carries MacVolvo's marker.
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
    private static var variantsFile:URL { support.appendingPathComponent("variants/macvolvo.conf") }
    private static var pluginsFile:URL { support.appendingPathComponent("plugins/macvolvo.conf") }
    private static let owner="Written by MacVolvo.app, which also removes it."
    /// The variant groups, each a dropdown in the Compatibility page; choices from both combine.
    static let driverGroup="MacVolvo driver"
    static let rendererGroup="MacVolvo renderer"

    /// One of NotProton's Steam tools, from its registry (notproton/tools: name, build, flavor,
    /// display name, tab-separated).
    struct Tool:Equatable {
        let name, build, flavor, display:String
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
    static var isAdded:Bool { owns(variantsFile) || owns(pluginsFile) }
    static func status(_ resources:URL)->Status {
        guard installed else { return .notInstalled }
        let tools=supported(resources)
        if tools.isEmpty { return .unsupported(builds:Self.tools().map(\.build)) }
        return isAdded ? .added(tools) : .available(tools)
    }
    static var steamRunning:Bool {
        let task=Process();task.executableURL=URL(fileURLWithPath:"/usr/bin/pgrep");task.arguments=["-x","steam_osx"]
        task.standardOutput=FileHandle.nullDevice;task.standardError=FileHandle.nullDevice
        guard (try? task.run()) != nil else { return false }
        task.waitUntilExit();return task.terminationStatus == 0
    }

    // MARK: The two files
    private static func driver(_ tool:Tool)->URL { ours.appendingPathComponent(tool.build+"/winemac.so") }
    static func variantsText(_ tools:[Tool])->String {
        var text="# \(owner)\n# MacVolvo's driver and renderers over NotProton's CrossOver runner, chosen in a game's\n"
            + "# Properties › Compatibility (Developer): \(driverGroup) and \(rendererGroup).\n"
        for tool in tools {
            text += """

            [\(tool.variant)]
            label = MacVolvo driver
            variant = \(driverGroup)
            src = \(driver(tool).deletingLastPathComponent().path)
            dst = lib/wine/x86_64-unix
            build = \(tool.build)

            """
        }
        return text + extraVariants(tools)
    }
    /// Every program but Steam's own and Wine's desktop, as portable_steam.h mouselookList().
    static let mouselook="*;!steam.exe;!steamwebhelper.exe;!steamwebhelper_real.exe;!steamservice.exe;"
        + "!steamerrorreporter.exe;!steamerrorreporter64.exe;!gameoverlayui.exe;!gameoverlayui64.exe;!explorer.exe"
    static func pluginsText(_ tools:[Tool])->String {
        // Plugin env lines win over NOTPROTON_ENV_ launch options (compat_run.sh), so the update
        // rate, a slider, is left to its NOTPROTON_ENV_ option and no row sets it. Its default is
        // the driver's own: at most one delivery a millisecond.
        let toolLines=tools.map { "tools = \($0.name)" }.joined(separator:"\n")
        let requires=tools.map { "requires = \(driver($0).path)" }.joined(separator:"\n")
        return """
        # \(owner)
        # MacVolvo's mouse and game profiles in a game's Properties › Compatibility (Developer).
        # The mouse needs the \(driverGroup) variant; a profile needs MacVolvo DXMT.

        [NOTPROTON_MACVOLVO_MOUSE]
        label = MacVolvo mouse
        description = Raw mouse input from MacVolvo’s driver, each click after the movement before it. Choose MacVolvo driver as a runner variant too.
        \(requires)
        \(toolLines)
        env = WINEMAC_MOUSELOOK=\(mouselook)
        env = WINEMAC_INPUT_VARIANT=precise
        env = WINEMAC_RAW_STATUS=$HOME/Library/Application Support/MacVolvo NotProton/hid-status.json

        [NOTPROTON_MACVOLVO_HIGH_PRECISION]
        label = High-precision
        description = Reads your mouse directly, skipping macOS’s mouse layer. macOS asks to allow Input Monitoring for Steam.
        depends = NOTPROTON_MACVOLVO_MOUSE
        \(toolLines)
        env = WINEMAC_RAW_SOURCE=hid
        env = WINEMAC_RAW_REQUEST=1

        [NOTPROTON_MACVOLVO_CLASSIC]
        label = Classic timing
        description = The older mouse timing. Try it if MacVolvo mouse feels different to you.
        depends = NOTPROTON_MACVOLVO_MOUSE
        \(toolLines)
        env = WINEMAC_INPUT_VARIANT=classic

        [NOTPROTON_ENV_WINEMAC_MOUSELOOK_RAW_INTERVAL_US]
        label = Mouse updates
        description = How many times a second the game gets your mouse’s movement. Movement in between is added to the next one.
        type = slider
        depends = NOTPROTON_MACVOLVO_MOUSE
        \(toolLines)
        choice = 4000 = 250
        choice = 2000 = 500
        choice = 1000 = 1,000
        choice = 500 = 2,000
        choice = 0 = Every update
        default = 1000

        """ + profileRows(tools)
    }

    // MARK: Add, update, remove
    /// Adds (or brings up to date) MacVolvo's driver, variant and rows for every supported runner.
    static func add(resources:URL) throws {
        let tools=supported(resources)
        guard !tools.isEmpty else { throw failure("NotProton has no CrossOver build this version of MacVolvo has a driver for.") }
        // Refuse before writing anything when a file in MacVolvo's place isn't MacVolvo's.
        for file in [variantsFile,pluginsFile] where fm.fileExists(atPath:file.path) && !owns(file) {
            throw failure("\(file.path) wasn't written by MacVolvo, so nothing was changed.")
        }
        for tool in tools {
            try fm.createDirectory(at:driver(tool).deletingLastPathComponent(),withIntermediateDirectories:true)
            try replace(driver(tool)) { try fm.copyItem(at:resources.appendingPathComponent("NotProton/\(tool.build)/winemac.so"),to:$0) }
            try replace(dxmtTree(tool)) { try fm.copyItem(at:bundledDXMT(resources,tool),to:$0) }
        }
        for file in [variantsFile,pluginsFile] { try fm.createDirectory(at:file.deletingLastPathComponent(),withIntermediateDirectories:true) }
        try write(variantsText(tools),to:variantsFile)
        try write(pluginsText(tools),to:pluginsFile)
        removeEarlierTool()
    }
    /// Removes only what carries MacVolvo's marker: the two files and MacVolvo's folder.
    static func remove() throws {
        for file in [variantsFile,pluginsFile] where owns(file) { try fm.removeItem(at:file) }
        removeEarlierTool()
        if fm.fileExists(atPath:ours.path) { try fm.removeItem(at:ours) }
    }
    /// After an update of MacVolvo: puts its current driver and rows in place, if added.
    /// Brings what's in Steam up to this copy of MacVolvo, in place. True when it changed anything,
    /// which Steam only sees after a restart.
    @discardableResult static func refresh(resources:URL)->Bool {
        // Deadlock can save Vulkan again from its Video settings; its profile keeps it on DirectX 11.
        if installedProfiles.contains(.deadlock) { keepDeadlockOnDirectX11() }
        guard case .added=status(resources),!isCurrent(resources:resources) else { return false }
        return (try? add(resources:resources)) != nil
    }
    /// Whether what's in place is what this copy of MacVolvo brings.
    static func isCurrent(resources:URL)->Bool {
        let tools=supported(resources)
        return !tools.isEmpty && (try? String(contentsOf:variantsFile,encoding:.utf8)) == variantsText(tools)
            && (try? String(contentsOf:pluginsFile,encoding:.utf8)) == pluginsText(tools)
            && tools.allSatisfy { fm.contentsEqual(atPath:driver($0).path,andPath:resources.appendingPathComponent("NotProton/\($0.build)/winemac.so").path)
                && fm.contentsEqual(atPath:dxmtTree($0).path,andPath:bundledDXMT(resources,$0).path) }
    }
    /// The first version made a Steam tool of its own (notproton-macvolvo-*) and a settings file.
    private static func removeEarlierTool() {
        for name in (try? fm.contentsOfDirectory(atPath:steamTools.path)) ?? [] where name.hasPrefix("notproton-macvolvo") {
            let folder=steamTools.appendingPathComponent(name)
            if fm.fileExists(atPath:folder.appendingPathComponent(".macvolvo-owner").path) { try? fm.removeItem(at:folder) }
        }
        try? fm.removeItem(at:ours.appendingPathComponent("settings"))
    }

    // MARK: The mouse report
    /// What MacVolvo's driver said about the High-precision mouse in the latest game that used it
    /// (WINEMAC_RAW_STATUS; cocoa_app.m hid_write_status). macOS gives Input Monitoring to the app
    /// that started the game, Steam here, and only the game can ask.
    struct MouseStatus:Decodable, Equatable {
        let access:String       // granted, denied, unknown
        let open:Int            // IOHIDManagerOpen's result, 0 when the mice opened
        let mice:Int            // mice open (trackpads aren't counted)
        let reporting:Bool      // one has sent movement: the game reads the mouse directly
        let process:String
        let time:TimeInterval
        var date:Date { Date(timeIntervalSince1970:time) }
    }
    static var mouseStatusFile:URL { ours.appendingPathComponent("hid-status.json") }
    static func mouseStatus()->MouseStatus? {
        guard let data=try? Data(contentsOf:mouseStatusFile),data.count < 4096 else { return nil }
        return try? JSONDecoder().decode(MouseStatus.self,from:data)
    }

    // MARK: Files
    private static func owns(_ file:URL)->Bool {
        (try? String(contentsOf:file,encoding:.utf8))?.hasPrefix("# "+owner) == true
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
    static func failure(_ message:String)->NSError {
        NSError(domain:"MacVolvo.NotProton",code:1,userInfo:[NSLocalizedDescriptionKey:message])
    }
}
