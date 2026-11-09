import Foundation

/// Steam Play compatibility tools for games that need a Wine of their own (environments/<game> in
/// the source tree; the first is THE FINALS, environments/finals/compat-tool). Each is bundled in
/// Resources/GameTools/<id>/: the tool's files (run, compatibilitytool.vdf, toolmanifest.vdf), its
/// Wine runtime (runtime.tar.gz) and tool.json. Install lays them into Steam's
/// compatibilitytools.d/<id>, where the macOS Steam app (Steam Play switched on by NotProton) offers
/// the tool in a game's Properties › Compatibility. The tool uses NotProton's Steam bridge, so
/// NotProton has to be installed; nothing of NotProton's is changed. Not downloaded yet: the
/// runtime comes with the app.
///
/// The installed folder carries a marker (.steamdriver-tool.json, also naming the runtime), so a
/// folder MacVolvo didn't make is never replaced or removed. Steam's prefix for the game
/// (steamapps/compatdata/<app id>) belongs to Steam and stays.
enum GameTools {
    struct Tool:Identifiable,Equatable,Sendable {
        let id:String        // folder in compatibilitytools.d and Steam's internal name
        let game:String      // the game, as the window shows it
        let display:String   // the tool's name in Steam's Compatibility dropdown
        let runtime:String   // the runtime version bundled with this app
        let bundle:URL       // Resources/GameTools/<id>
    }
    enum State:Equatable { case notInstalled, installed, outdated(String), foreign }
    struct Failure:LocalizedError { let errorDescription:String? }

    private static let fm=FileManager.default
    static let marker=".steamdriver-tool.json"
    private static let toolFiles=["run","compatibilitytool.vdf","toolmanifest.vdf"]

    /// The tools this app carries (Resources/GameTools/*/tool.json).
    static func bundled(_ resources:URL)->[Tool] {
        let root=resources.appendingPathComponent("GameTools")
        let folders=(try? fm.contentsOfDirectory(at:root,includingPropertiesForKeys:nil)) ?? []
        return folders.compactMap { folder in
            guard let data=try? Data(contentsOf:folder.appendingPathComponent("tool.json")),
                  let json=try? JSONSerialization.jsonObject(with:data) as? [String:Any],
                  let id=json["id"] as? String,id==folder.lastPathComponent,
                  let game=json["game"] as? String,let display=json["display"] as? String,
                  let runtime=json["runtime"] as? String else { return nil }
            return Tool(id:id,game:game,display:display,runtime:runtime,bundle:folder)
        }.sorted { $0.game < $1.game }
    }
    static func folder(_ tool:Tool)->URL { NotProton.steamTools.appendingPathComponent(tool.id) }
    /// The runtime an installed tool has, from its marker; nil when there is none.
    static func installedRuntime(_ tool:Tool)->String? {
        guard let data=try? Data(contentsOf:folder(tool).appendingPathComponent(marker)),
              let json=try? JSONSerialization.jsonObject(with:data) as? [String:Any] else { return nil }
        return json["runtime"] as? String
    }
    static func state(_ tool:Tool)->State {
        let dest=folder(tool)
        guard fm.fileExists(atPath:dest.path) else { return .notInstalled }
        guard let runtime=installedRuntime(tool) else { return .foreign }
        return runtime==tool.runtime ? .installed : .outdated(runtime)
    }

    /// Lays the tool and its runtime into compatibilitytools.d/<id>: built beside it, then swapped
    /// in, so Steam never sees half a tool. Runs off the main thread (the runtime is ~1 GB unpacked).
    static func install(_ tool:Tool) throws {
        let dest=folder(tool),parent=dest.deletingLastPathComponent()
        if case .foreign=state(tool) {
            throw Failure(errorDescription:"Steam already has a \u{201C}\(tool.id)\u{201D} tool that \(Brand.name) didn’t install, so it was left as it is.")
        }
        try fm.createDirectory(at:parent,withIntermediateDirectories:true)
        let staging=parent.appendingPathComponent(".\(tool.id).installing"),old=parent.appendingPathComponent(".\(tool.id).old")
        try? fm.removeItem(at:staging);try? fm.removeItem(at:old)
        let runtime=staging.appendingPathComponent("runtime")
        try fm.createDirectory(at:runtime,withIntermediateDirectories:true)
        do {
            let tar=Process()
            tar.executableURL=URL(fileURLWithPath:"/usr/bin/tar")
            tar.arguments=["-xzf",tool.bundle.appendingPathComponent("runtime.tar.gz").path,"-C",runtime.path]
            try tar.run();tar.waitUntilExit()
            guard tar.terminationStatus==0 else { throw Failure(errorDescription:"The \(tool.game) runtime couldn’t be unpacked.") }
            for name in toolFiles {
                try fm.copyItem(at:tool.bundle.appendingPathComponent(name),to:staging.appendingPathComponent(name))
            }
            try fm.setAttributes([.posixPermissions:0o755],ofItemAtPath:staging.appendingPathComponent("run").path)
            let record:[String:Any]=["tool":tool.id,"runtime":tool.runtime,"written_by":"\(Brand.name).app"]
            try JSONSerialization.data(withJSONObject:record,options:[.sortedKeys]).write(to:staging.appendingPathComponent(marker))
            if fm.fileExists(atPath:dest.path) { try fm.moveItem(at:dest,to:old) }
            try fm.moveItem(at:staging,to:dest)
            try? fm.removeItem(at:old)
        } catch {
            try? fm.removeItem(at:staging)
            if !fm.fileExists(atPath:dest.path),fm.fileExists(atPath:old.path) { try? fm.moveItem(at:old,to:dest) }
            throw error
        }
    }
    /// Removes the tool, only when it carries MacVolvo's marker. The game's prefix stays (Steam's).
    static func remove(_ tool:Tool) throws {
        switch state(tool) {
        case .notInstalled: return
        case .foreign: throw Failure(errorDescription:"That \u{201C}\(tool.id)\u{201D} tool wasn’t installed by \(Brand.name), so it was left as it is.")
        case .installed,.outdated: try fm.removeItem(at:folder(tool))
        }
    }
}
