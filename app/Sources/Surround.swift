import Foundation

/// Experimental "Headphone surround" (docs/SURROUND.md). The switch writes
/// SteamDriver/headphone-surround; with it there, a profile's Settings sheet shows Headphone
/// surround (`surround` in d3dmetal.conf, `steamdriver.surround` in dxmt.conf). The SteamDriver
/// tool's run script reads both at launch and sets WINE_HEADPHONE_SURROUND=1, so the runtime's
/// Core Audio driver (wine-coreaudio-headphone-surround.patch, macfinals-192 on) offers a stereo
/// output to the game as 7.1 and renders the speakers for headphones. Turning the switch off
/// leaves the profiles' settings as they are; the run script ignores them until it's on again.
enum Surround {
    static var switchFile:URL { NotProton.ours.appendingPathComponent("headphone-surround") }
    static var enabled:Bool { FileManager.default.fileExists(atPath:switchFile.path) }
    static func setEnabled(_ on:Bool) throws {
        if on {
            try FileManager.default.createDirectory(at:switchFile.deletingLastPathComponent(),withIntermediateDirectories:true)
            try "# macSteam: Experimental › Headphone surround is on; profiles may turn it on.\n".write(to:switchFile,atomically:true,encoding:.utf8)
        } else { try? FileManager.default.removeItem(at:switchFile) }
    }
}
