import Foundation

/// The games in macSteam's own Steam (troubleshooting), and, when it's removed with Keep games,
/// moving them into the Steam app's library so the Steam app (with NotProton) uses them instead
/// of downloading them again. Both Steams write the same app manifest (appmanifest_<id>.acf, the
/// Windows depots), so the Steam app takes a moved game as installed; it may check the files
/// the first time. A game is moved with its manifest and its Workshop items. A game the Steam app
/// already has (same folder or manifest) stays behind and goes to the Trash with the rest.
/// Moving within one volume is a rename; from another volume it's a copy, which takes a while.
/// Libraries the own Steam keeps outside its folder are never touched: removing it leaves them.
enum OwnSteamGames {
    struct Game:Identifiable,Equatable {
        let appID:String,name:String,installdir:String,size:Int64
        var id:String { appID }
    }
    private static let fm=FileManager.default

    static func steamapps(_ root:URL)->URL { root.appendingPathComponent("environment/drive_c/Program Files (x86)/Steam/steamapps") }
    /// The Steam app's own library.
    static var steamApp:URL { NotProton.home.appendingPathComponent("Library/Application Support/Steam") }
    static var target:URL { steamApp.appendingPathComponent("steamapps") }
    static var steamAppInstalled:Bool { fm.fileExists(atPath:steamApp.path) }

    /// Installed games: a manifest whose game folder is there.
    static func list(_ root:URL)->[Game] {
        let apps=steamapps(root)
        let files=((try? fm.contentsOfDirectory(atPath:apps.path)) ?? []).filter { $0.hasPrefix("appmanifest_") && $0.hasSuffix(".acf") }.sorted()
        return files.compactMap { file in
            guard let text=try? String(contentsOf:apps.appendingPathComponent(file),encoding:.utf8),
                  let id=value("appid",text),let dir=value("installdir",text),!dir.isEmpty,!dir.contains("/"),dir != "..",
                  fm.fileExists(atPath:apps.appendingPathComponent("common/\(dir)").path) else { return nil }
            return Game(appID:id,name:value("name",text) ?? dir,installdir:dir,size:Int64(value("SizeOnDisk",text) ?? "") ?? 0)
        }
    }
    private static func value(_ key:String,_ text:String)->String? {
        guard let rx=try? NSRegularExpression(pattern:"\"\(key)\"\\s+\"([^\"]*)\""),
              let m=rx.firstMatch(in:text,range:NSRange(text.startIndex...,in:text)),let r=Range(m.range(at:1),in:text) else { return nil }
        return String(text[r])
    }

    struct Result { var moved:[String]=[],skipped:[String]=[] }
    enum MoveError:LocalizedError {
        case failed(String,String)
        var errorDescription:String? { if case .failed(let name,let why)=self { return "\(name) couldn’t be moved to the Steam app: \(why)" };return nil }
    }

    /// Moves every game into the Steam app's library. Stops at the first game that fails to
    /// move (what's moved stays moved), so the caller can leave the own Steam in place.
    static func move(_ root:URL) throws->Result {
        let from=steamapps(root),to=target
        var result=Result()
        try fm.createDirectory(at:to.appendingPathComponent("common"),withIntermediateDirectories:true)
        for game in list(root) {
            let manifest="appmanifest_\(game.appID).acf"
            let folder=to.appendingPathComponent("common/\(game.installdir)")
            if fm.fileExists(atPath:folder.path) || fm.fileExists(atPath:to.appendingPathComponent(manifest).path) {
                result.skipped.append(game.name);continue
            }
            do {
                try fm.moveItem(at:from.appendingPathComponent("common/\(game.installdir)"),to:folder)
                try fm.moveItem(at:from.appendingPathComponent(manifest),to:to.appendingPathComponent(manifest))
            } catch { throw MoveError.failed(game.name,error.localizedDescription) }
            // Workshop items, when it has any and the Steam app has none of its own.
            let items=from.appendingPathComponent("workshop/content/\(game.appID)"),itemsTo=to.appendingPathComponent("workshop/content/\(game.appID)")
            let record=from.appendingPathComponent("workshop/appworkshop_\(game.appID).acf"),recordTo=to.appendingPathComponent("workshop/appworkshop_\(game.appID).acf")
            if fm.fileExists(atPath:items.path),!fm.fileExists(atPath:itemsTo.path),!fm.fileExists(atPath:recordTo.path) {
                try? fm.createDirectory(at:itemsTo.deletingLastPathComponent(),withIntermediateDirectories:true)
                if (try? fm.moveItem(at:items,to:itemsTo)) != nil { try? fm.moveItem(at:record,to:recordTo) }
            }
            result.moved.append(game.name)
        }
        return result
    }
}
