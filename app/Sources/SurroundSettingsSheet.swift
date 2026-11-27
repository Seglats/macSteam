import SwiftUI
import UniformTypeIdentifiers

/// Experimental › Headphone surround › Settings…: the live preview with every setting
/// (SurroundSettings.swift). Changes are heard at once; Save makes games use them.
struct SurroundSettingsSheet:View {
    @Bindable var model:AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var scrub:Double?
    @State private var saving=false
    private var s:SurroundSettings { model.surroundSettings }

    var body:some View {
        VStack(spacing:16) {
            LauncherHeading(title:"Headphone surround",subtitle:"",compact:true)
            player
            ScrollView {
                VStack(alignment:.leading,spacing:14) {
                    ForEach(SurroundKnobs.groups,id:\.self) { group in
                        VStack(alignment:.leading,spacing:6) {
                            Text(group).font(LauncherStyle.font(13,.semibold))
                            ForEach(SurroundKnobs.all.filter { $0.group == group }) { knob in slider(knob) }
                        }
                    }
                }.padding(.horizontal,2)
            }.scrollIndicators(.hidden).frame(maxHeight:340)
            Text(s.notice.isEmpty ? "Changes are heard at once. Save to use them in games, from each game’s next launch." : s.notice)
                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                .frame(maxWidth:.infinity,alignment:.leading)
            HStack {
                Button("Reset to Fitted") { s.resetToFitted() }
                    .buttonStyle(.borderless).hoverGrow().disabled(s.isFitted)
                    .help("The settings fitted to Dolby Atmos for Headphones (docs/SURROUND.md)")
                Spacer()
                Button("Close") { s.discardChanges();s.stop();dismiss() }
                    .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).keyboardShortcut(.cancelAction)
                    .help(s.changed ? "Close without saving the changes" : "Close")
                Button(saving ? "Saving…" : "Save") {
                    saving=true
                    s.save { _ in saving=false }
                }
                .buttonStyle(.borderedProminent).hoverGrow().buttonBorderShape(.capsule).disabled(!s.changed || saving)
                .help("Games use these settings from their next launch")
            }
        }
        .font(LauncherStyle.font(14)).padding(.horizontal,28).padding(.vertical,24).frame(width:560)
        .tint(.primary).background { LauncherBackground() }
        .onAppear {
            s.notice=""
            s.start(resources:Bundle.main.resourceURL ?? Bundle.main.bundleURL,helpers:Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers"))
        }
        .onDisappear { s.stop() }
    }

    private var player:some View {
        HStack(alignment:.center,spacing:14) {
            SurroundSpeakers(active:nil,angle:s.isPlaying ? s.angle : nil)
            VStack(alignment:.leading,spacing:8) {
                HStack(spacing:8) {
                    Button("Play a File…") { chooseFile() }
                        .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
                        .help("A 5.1 or 7.1 WAV file of your own, heard with these settings")
                    if s.fileName != nil {
                        Button("Back to the Test") { s.backToTest() }.buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium))
                    }
                    Spacer(minLength:8)
                    Picker("",selection:Binding(get:{ s.surround },set:{ s.surround=$0 })) {
                        Text("Surround On").tag(true);Text("Off").tag(false)
                    }.pickerStyle(.segmented).labelsHidden().fixedSize()
                    .help("Off is a plain stereo mix of the same sound, what a game plays on headphones without headphone surround")
                }
                HStack(spacing:10) {
                    Button { scrub=nil;s.playPause() } label:{ Image(systemName:s.isPlaying ? "pause.fill" : "play.fill").frame(width:14) }
                        .buttonStyle(.borderless).hoverGrow().disabled(s.phase != .ready && s.phase != .playing)
                        .help(s.isPlaying ? "Pause" : "Play")
                    Button { scrub=nil;s.replay() } label:{ Image(systemName:"arrow.counterclockwise") }
                        .buttonStyle(.borderless).hoverGrow().disabled(s.phase != .ready && s.phase != .playing)
                        .help("From the start")
                    Slider(value:Binding(get:{ scrub ?? s.time },set:{ scrub=$0 }),in:0...max(s.length,0.1)) { editing in
                        if !editing,let to=scrub { s.seek(to:to);scrub=nil }
                    }.controlSize(.small).disabled(s.length == 0)
                    Text(clock(scrub ?? s.time)+" / "+clock(s.length)).font(LauncherStyle.font(11).monospacedDigit()).foregroundStyle(.secondary).fixedSize()
                }
                HStack(spacing:8) {
                    Image(systemName:"speaker.fill").font(.system(size:10)).foregroundStyle(.secondary)
                    Slider(value:Binding(get:{ s.volume },set:{ s.volume=$0 }),in:0...1).controlSize(.small).frame(maxWidth:180)
                    Image(systemName:"speaker.wave.3.fill").font(.system(size:10)).foregroundStyle(.secondary)
                    Text("\(Int((s.volume*100).rounded())) %").font(LauncherStyle.font(11).monospacedDigit()).foregroundStyle(.secondary)
                }
                Text(status).font(LauncherStyle.font(12)).foregroundStyle(s.phase.isFailure ? .orange : .secondary).fixedSize(horizontal:false,vertical:true)
            }
        }
    }

    private var status:String {
        if case .failed(let why)=s.phase { return why }
        if s.phase == .starting { return "Starting…" }
        if let name=s.fileName {
            if let ch=s.fileChannels,ch <= 2 { return "\(name) is \(ch == 1 ? "mono" : "stereo"): surround works on 5.1 and 7.1 sound." }
            return name
        }
        if s.isPlaying,let p=s.pattern { return "Pattern \(p) of 5, \(SurroundTest.patternNames[max(0,min(4,p-1))])." }
        return "Use headphones. The ball bounces around you; move a setting and hear it change."
    }

    private func slider(_ knob:SurroundKnobs.Knob)->some View {
        let value=s.values[knob.key] ?? knob.fitted
        return HStack(spacing:10) {
            Text(knob.title).font(LauncherStyle.font(12)).frame(width:78,alignment:.leading)
            Slider(value:Binding(get:{ value },set:{ s.set(knob.key,$0) }),in:knob.range).controlSize(.small)
            Text(format(value,knob)).font(LauncherStyle.font(11).monospacedDigit()).foregroundStyle(.secondary).frame(width:58,alignment:.trailing)
            Button { s.set(knob.key,knob.fitted) } label:{ Image(systemName:"arrow.uturn.backward").font(.system(size:10)) }
                .buttonStyle(.borderless).hoverGrow().opacity(abs(value-knob.fitted) < 1e-6 ? 0 : 1)
                .help("Back to the fitted \(format(knob.fitted,knob))")
        }.help(knob.help)
    }
    private func format(_ v:Double,_ knob:SurroundKnobs.Knob)->String {
        switch knob.unit {
        case "×": return String(format:"%.2f×",v)
        case " s": return String(format:"%.2f s",v)
        case " dB": return String(format:"%+.1f dB",v)
        default: return String(format:"%.0f%@",v,knob.unit)
        }
    }
    private func clock(_ seconds:Double)->String { let t=max(0,Int(seconds));return String(format:"%d:%02d",t/60,t%60) }
    private func chooseFile() {
        let panel=NSOpenPanel()
        panel.allowedContentTypes=[.wav];panel.canChooseDirectories=false
        panel.message="Choose a 5.1 or 7.1 WAV file to hear with these settings"
        panel.prompt="Play"
        if panel.runModal() == .OK,let url=panel.url { s.playFile(url) }
    }
}
