import Foundation

// The product's public identity in one place: name, tagline, project links,
// update feed and release file name. macSteam is a fork of Recall (Overwatch on
// the Mac) for Valve's Source 2 games; it has its own bundle identifier
// (app.steamdriver.launcher) and data folders (SteamDriver, SteamDriver Steam), so it never
// touches an installed Recall. Names (Rasmus, 2026-11-24): the app is macSteam; the engine it
// uses keeps SteamDriver's name: the folders, the Steam tool ("SteamDriver", notproton-steamdriver), the
// rows in Steam ("SteamDriver driver" …) and the bundle identifier. Not com.macsteam.app or a
// "macsteam" folder: those belong to an unrelated app (macSteam Config). It was called MacVolvo until 2026-11-24 (app.macvolvo.launcher, data in
// MacVolvo, NotProton files in MacVolvo NotProton); Migration.swift moves that over once.
// The owner marker files keep their .macvolvo names: they are hidden, and a folder on a drive
// that isn't connected at the first start still has to be recognised later.
enum Brand {
    #if MACVOLVO_GPTK4
    static let name="macSteam GPTK4"
    static let bundleIdentifier="app.steamdriver.gptk4"
    static let legacyBundleIdentifier="app.macvolvo.gptk4"
    static let legacyDataFolderName="MacVolvo GPTK4"
    static let ownerMarker=".macvolvo-gptk4-owner"
    static let otherOwnerMarker=".macvolvo-owner"
    static let allowsUpdates=false
    #else
    static let name="macSteam"
    static let bundleIdentifier="app.steamdriver.launcher"
    static let legacyBundleIdentifier="app.macvolvo.launcher"
    static let legacyDataFolderName="MacVolvo"
    static let ownerMarker=".macvolvo-owner"
    static let otherOwnerMarker=".macvolvo-gptk4-owner"
    static let allowsUpdates=true
    #endif
    /// The own-Steam data (Windows Steam in a Wine environment of its own), apart from the
    /// NotProton folder (NotProton.ours, "SteamDriver").
    #if MACVOLVO_GPTK4
    static let dataFolderName="SteamDriver GPTK4 Steam"
    #else
    static let dataFolderName="SteamDriver Steam"
    #endif
    static let tagline="Counter-Strike 2 and Deadlock on Apple Silicon."
    static let repositoryPath="seglats/macsteam"  // the GitHub repository, issues and update feed (haeli05/MacVolvo until 2026-11-24)
    static let repository=URL(string:"https://github.com/\(repositoryPath)")!
    static let releases=URL(string:"https://github.com/\(repositoryPath)/releases")!
    /// Report a Problem: macSteam's own issue tracker.
    static let issues=URL(string:"https://github.com/\(repositoryPath)/issues")!
    /// GitHub's latest-release API for the repository. The updater accepts any
    /// release disk image named "<anything>-<version>.dmg".
    static let releaseFeed=URL(string:"https://api.github.com/repos/\(repositoryPath)/releases/latest")!
    static func diskImage(_ version:String)->String { "\(name)-\(version).dmg" }
    static let trademarks="Not affiliated with or endorsed by Valve Corporation, Volvo, Blizzard Entertainment or Apple. Counter-Strike, Deadlock and Steam are trademarks of Valve Corporation. Mac is a trademark of Apple Inc. Based on Recall by Joshua Asher Nelson."
    /// Who makes macSteam: shown in About.
    static let author="@haeli05"
    static let authorPage=URL(string:"https://github.com/haeli05")!
    /// The project macSteam is built on.
    static let upstream=URL(string:"https://github.com/AsherJN/recall")!
}
