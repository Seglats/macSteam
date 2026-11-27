import Foundation

// Checks the app's install-location rules against real mounted disk images.
// Build: swiftc -parse-as-library app/Sources/InstallLocation.swift tests/InstallLocationTests.swift
@main struct InstallLocationTests {
    static func run(_ args:[String]) {
        let p=Process();p.executableURL=URL(fileURLWithPath:"/usr/bin/hdiutil");p.arguments=args
        p.standardOutput=FileHandle.nullDevice;p.standardError=FileHandle.nullDevice
        try! p.run();p.waitUntilExit();precondition(p.terminationStatus == 0,"hdiutil \(args.first!) failed")
    }
    static func main() throws {
        let fm=FileManager.default
        let base=fm.temporaryDirectory.appendingPathComponent("steamdriver-location-tests-\(getpid())")
        try fm.createDirectory(at:base,withIntermediateDirectories:true)
        var mounts=[URL]()
        defer { for m in mounts { run(["detach","-force",m.path]) };try? fm.removeItem(at:base) }
        func volume(_ filesystem:String,_ name:String,readOnly:Bool=false)->URL {
            let image=base.appendingPathComponent(name+".sparseimage"),mount=base.appendingPathComponent(name)
            run(["create","-size","64m","-fs",filesystem,"-type","SPARSE","-volname",name,image.path])
            run(["attach","-nobrowse"]+(readOnly ? ["-readonly"] : [])+["-mountpoint",mount.path,image.path])
            mounts.append(mount);return mount
        }
        let apfs=volume("APFS","TestAPFS")
        let root=InstallLocation.root(forChosen:apfs)
        precondition(root.lastPathComponent == InstallLocation.folderName && root.deletingLastPathComponent().path == apfs.standardizedFileURL.path)
        precondition(InstallLocation.problem(for:root) == nil,"APFS drive refused")
        precondition(InstallLocation.isAvailable(root) && InstallLocation.title(root) == "TestAPFS")
        try fm.createDirectory(at:apfs.appendingPathComponent("Games"),withIntermediateDirectories:true)
        precondition(InstallLocation.title(InstallLocation.root(forChosen:apfs.appendingPathComponent("Games"))) == "TestAPFS › Games")
        // An existing app-made folder reconnects; one with other files is refused.
        try fm.createDirectory(at:root,withIntermediateDirectories:true)
        try "steamdriver-setup-v1\n".write(to:root.appendingPathComponent(Brand.ownerMarker),atomically:true,encoding:.utf8)
        precondition(InstallLocation.root(forChosen:root) == root.standardizedFileURL && InstallLocation.problem(for:root) == nil)
        let foreign=apfs.appendingPathComponent("Other").appendingPathComponent(InstallLocation.folderName)
        try fm.createDirectory(at:foreign,withIntermediateDirectories:true)
        try "keep".write(to:foreign.appendingPathComponent("personal.txt"),atomically:true,encoding:.utf8)
        precondition(InstallLocation.problem(for:foreign)?.contains("already contains") == true)
        let exfat=InstallLocation.root(forChosen:volume("ExFAT","TestEXFAT"))
        precondition(InstallLocation.problem(for:exfat)?.contains("APFS or Mac OS Extended") == true,"ExFAT accepted")
        let sensitive=InstallLocation.root(forChosen:volume("Case-sensitive APFS","TestCASE"))
        precondition(InstallLocation.problem(for:sensitive)?.contains("case-sensitive") == true,"case-sensitive accepted")
        let locked=InstallLocation.root(forChosen:volume("APFS","TestLOCKED",readOnly:true))
        precondition(InstallLocation.problem(for:locked)?.contains("read-only") == true,"read-only accepted")
        // Disconnected: unavailable, still named from its path, never created.
        let unplugged=URL(fileURLWithPath:"/Volumes/Unplugged Test SSD/Games/"+InstallLocation.folderName)
        precondition(!InstallLocation.isAvailable(unplugged) && InstallLocation.driveName(unplugged) == "Unplugged Test SSD")
        precondition(InstallLocation.isStandard(InstallLocation.standard) && InstallLocation.problem(for:InstallLocation.standard) == nil)
        print("PASS: install location accepts APFS and reconnects owned folders; refuses ExFAT, case-sensitive, read-only and foreign folders; detects disconnected drives")
    }
}
