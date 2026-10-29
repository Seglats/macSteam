import Foundation

// The product's public identity in one place: name, tagline, project links,
// update feed and release file name. MacFinals is a fork of MacVolvo (itself a fork
// of Recall) for DirectX 12 games on Apple's D3DMetal, starting with THE FINALS. It has
// its own bundle identifier (app.macfinals.launcher) and data folder (MacFinals), so it
// never touches an installed MacVolvo or Recall.
enum Brand {
    static let name="MacFinals"
    static let bundleIdentifier="app.macfinals.launcher"
    static let ownerMarker=".macfinals-owner"
    /// MacVolvo's and MacVolvo GPTK4's folders; never adopted as this app's own.
    static let otherOwnerMarkers=[".macvolvo-owner",".macvolvo-gptk4-owner"]
    /// No MacFinals release feed exists yet; MacVolvo's would offer the wrong app.
    static let allowsUpdates=false
    static let dataFolderName=name
    static let tagline="THE FINALS on Apple Silicon."
    static let repositoryPath="haeli05/MacVolvo"
    static let repository=URL(string:"https://github.com/\(repositoryPath)")!
    static let releases=URL(string:"https://github.com/\(repositoryPath)/releases")!
    static let issues=URL(string:"https://github.com/\(repositoryPath)/issues/new/choose")!
    /// GitHub's latest-release API for the repository. The updater accepts any
    /// release disk image named "<anything>-<version>.dmg".
    static let releaseFeed=URL(string:"https://api.github.com/repos/\(repositoryPath)/releases/latest")!
    static func diskImage(_ version:String)->String { "\(name)-\(version).dmg" }
    static let trademarks="Not affiliated with or endorsed by Embark Studios, Valve Corporation, Blizzard Entertainment or Apple. THE FINALS is a trademark of Embark Studios AB. Steam is a trademark of Valve Corporation. Mac is a trademark of Apple Inc. Based on MacVolvo and Recall by Joshua Asher Nelson."
    /// Who made MacVolvo, which MacFinals is built on: shown in About.
    static let author="@haeli05"
    static let authorPage=URL(string:"https://github.com/haeli05")!
    /// The project MacVolvo is built on.
    static let upstream=URL(string:"https://github.com/AsherJN/recall")!
}
