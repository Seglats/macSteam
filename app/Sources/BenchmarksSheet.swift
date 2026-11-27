import SwiftUI
import Charts

/// Benchmarks (Benchmarks.swift): which games record, and the saved runs per game with fps,
/// frame times, GPU time and latency, and a frame-time graph.
struct BenchmarksSheet:View {
    @Bindable var model:AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var games:[Profiles.Game]=[]
    @State private var choosing=false
    @State private var open:String?
    @State private var capturing=false
    @State private var recorder=KeyComboRecorder()
    @State private var hotkeyNotice=""
    private var hotkey:String { KeyOverrides.caps(model.benchHotkey).joined() }

    private enum Mode:String,CaseIterable,Identifiable { case off="Off",all="All games",chosen="Chosen games"; var id:String { rawValue } }
    private var mode:Mode {
        switch model.benchRecord { case .off: .off; case .all: .all; case .games: .chosen }
    }
    private var chosen:Set<String> { if case .games(let ids)=model.benchRecord { ids } else { [] } }
    private func name(_ appID:String)->String { games.first { $0.appID==appID }?.name ?? "App \(appID)" }

    var body:some View {
        VStack(spacing:16) {
            LauncherHeading(title:"Benchmarks",subtitle:"",compact:true)
            Text("Measures fps, frame times, GPU time and latency while you play. Turn recording on for a game, start it with SteamDriver, press \(hotkey) where the run should begin and \(hotkey) again where it ends: a sound confirms each press, and the run appears below. Play the same stretch each time to compare settings.")
                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                .frame(maxWidth:.infinity,alignment:.leading)
            LauncherPanel(inset:12) { recordSettings }
            if !model.benchStatus.isEmpty { LauncherNotice(text:model.benchStatus) }
            LauncherPanel(inset:12) {
                ScrollView {
                    VStack(alignment:.leading,spacing:10) {
                        if model.benchRuns.isEmpty {
                            Text("No runs yet.").font(LauncherStyle.font(12)).foregroundStyle(.secondary).padding(.vertical,6)
                        }
                        ForEach(groups,id:\.0) { game,runs in
                            Text(game).font(LauncherStyle.font(13,.semibold)).padding(.top,4)
                            ForEach(runs) { run in runRow(run) }
                        }
                    }.frame(maxWidth:.infinity,alignment:.leading)
                }.frame(maxHeight:380).fixedSize(horizontal:false,vertical:true)
            }
            Text("Recording uses Apple’s Metal performance HUD, hidden, plus the renderer’s own present log; each costs a little per frame, so leave recording off when you aren’t measuring. With SteamDriver on CrossOver, add MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1 MTL_HUD_OPACITY=0 to the game’s launch options (no latency there). How it is measured: docs/BENCHMARKS.md.")
                .font(LauncherStyle.font(11)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                .frame(maxWidth:.infinity,alignment:.leading)
            HStack {
                Spacer()
                Button("Done") { stopCapture();dismiss() }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.large)
                    .keyboardShortcut(capturing ? nil : .cancelAction)
            }
        }.font(LauncherStyle.font(14)).padding(.horizontal,32).padding(.vertical,28).frame(width:620)
            .onDisappear { stopCapture() }
            .tint(.primary).background { LauncherBackground() }
            .onAppear { games=Profiles.steamGames();model.benchRuns=Benchmarks.runs() }
    }

    // MARK: Which games record
    private var recordSettings:some View {
        VStack(alignment:.leading,spacing:8) {
            HStack {
                Text("Record").font(LauncherStyle.font(13,.semibold))
                Spacer()
                Picker("",selection:Binding(get:{ mode },set:{ new in
                    switch new {
                    case .off: model.setBenchRecord(.off)
                    case .all: model.setBenchRecord(.all)
                    case .chosen: if chosen.isEmpty { choosing=true } else { model.setBenchRecord(.games(chosen)) }
                    }
                })) { ForEach(Mode.allCases) { Text($0.rawValue).tag($0) } }
                    .pickerStyle(.segmented).labelsHidden().frame(width:300)
                    .popover(isPresented:$choosing,arrowEdge:.bottom) {
                        BenchmarkGamePicker(games:games,taken:chosen) { game in
                            choosing=false;model.setBenchRecord(.games(chosen.union([game.appID])))
                        }
                    }
            }
            if case .games(let ids)=model.benchRecord {
                ForEach(ids.sorted { name($0) < name($1) },id:\.self) { id in
                    HStack {
                        Image(systemName:"gamecontroller").foregroundStyle(.secondary)
                        Text(name(id)).font(LauncherStyle.font(12,.medium))
                        Spacer()
                        Button { model.setBenchRecord(.games(ids.subtracting([id]))) } label: { Image(systemName:"minus.circle.fill").foregroundStyle(.secondary) }
                            .buttonStyle(.borderless).hoverGrow().help("Stop recording \(name(id))")
                    }
                }
                Button { choosing=true } label: { Label("Add Game…",systemImage:"plus") }
                    .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
            }
            Divider().padding(.vertical,2)
            hotkeyRow
            if !hotkeyNotice.isEmpty { Text(hotkeyNotice).font(LauncherStyle.font(11)).foregroundStyle(.orange).fixedSize(horizontal:false,vertical:true) }
            Text(model.benchRecord == .off ? "\(hotkey) does nothing while recording is off."
                 : "Applies from a game’s next launch. \(hotkey) is \(Brand.name)’s while recording is on, so keep \(Brand.name) open (its window can be closed).")
                .font(LauncherStyle.font(11)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
        }
    }

    // MARK: Hotkey (KeyComboRecorder, as for key combo overrides)
    private var hotkeyRow:some View {
        HStack(spacing:10) {
            Text("Hotkey").font(LauncherStyle.font(13,.semibold))
            Spacer()
            if capturing {
                Image(systemName:"keyboard").foregroundStyle(.secondary)
                Text("Press the new key combo…").font(LauncherStyle.font(12)).foregroundStyle(.secondary)
                Button("Cancel") { stopCapture() }.buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium))
            } else {
                HStack(spacing:4) {
                    ForEach(Array(KeyOverrides.caps(model.benchHotkey).enumerated()),id:\.offset) { _,cap in
                        Text(cap).font(LauncherStyle.font(12,.semibold)).monospaced()
                            .padding(.horizontal,7).padding(.vertical,3)
                            .background(.primary.opacity(0.08),in:RoundedRectangle(cornerRadius:6,style:.continuous))
                    }
                }
                Button("Change…") { startCapture() }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
                    .help("Record a new key combo for starting and stopping a run")
                if model.benchHotkey != Benchmarks.defaultHotkey {
                    Button("Reset") { hotkeyNotice="";model.setBenchHotkey(nil) }.buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium))
                        .help("Back to \(KeyOverrides.caps(Benchmarks.defaultHotkey).joined())")
                }
            }
        }
    }
    private func startCapture() {
        hotkeyNotice="";capturing=true
        BenchmarkRecorder.shared.suspended=true
        recorder.start { combo in
            stopCapture()
            if let problem=Benchmarks.hotkeyProblem(combo) { hotkeyNotice=problem;return }
            model.setBenchHotkey(combo)
        }
    }
    private func stopCapture() {
        recorder.stop();capturing=false
        BenchmarkRecorder.shared.suspended=false
    }

    // MARK: Runs
    private var groups:[(String,[Benchmarks.Run])] {
        var order:[String]=[],byGame:[String:[Benchmarks.Run]]=[:]
        for run in model.benchRuns { if byGame[run.game]==nil { order.append(run.game) };byGame[run.game,default:[]].append(run) }
        return order.map { ($0,byGame[$0]!) }
    }
    private func runRow(_ run:Benchmarks.Run)->some View {
        let s=run.summary
        return VStack(alignment:.leading,spacing:6) {
            Button { open = open==run.id ? nil : run.id } label: {
                HStack(spacing:10) {
                    Image(systemName:"chevron.right").font(.system(size:10,weight:.semibold)).rotationEffect(.degrees(open==run.id ? 90 : 0)).foregroundStyle(.secondary)
                    VStack(alignment:.leading,spacing:1) {
                        Text(run.label.isEmpty ? Date(timeIntervalSince1970:run.startedUnix).formatted(date:.abbreviated,time:.shortened) : run.label)
                            .font(LauncherStyle.font(12,.semibold)).lineLimit(1)
                        Text(Benchmarks.settingsLine(run)).font(LauncherStyle.font(11)).foregroundStyle(.secondary).lineLimit(1)
                    }
                    Spacer(minLength:8)
                    stat(fmt(s.avgFPS,0),"fps")
                    stat(fmt(s.low1FPS,0),"1% low")
                    stat(fmt(s.p99,1),"p99 ms")
                    stat(s.latencyP50.map { fmt($0,1) } ?? "–","latency")
                }.contentShape(Rectangle())
            }.buttonStyle(.plain).hoverGrow()
            if open==run.id { RunDetail(model:model,run:run).padding(.leading,20) }
        }
    }
    private func stat(_ value:String,_ label:String)->some View {
        VStack(alignment:.trailing,spacing:0) {
            Text(value).font(LauncherStyle.font(13,.semibold)).monospacedDigit()
            Text(label).font(LauncherStyle.font(10)).foregroundStyle(.secondary)
        }.frame(minWidth:52,alignment:.trailing)
    }
}

func fmt(_ value:Double,_ digits:Int)->String { value.formatted(.number.precision(.fractionLength(digits))) }

/// One run's numbers and frame-time graph, with its name and Delete.
private struct RunDetail:View {
    @Bindable var model:AppModel
    let run:Benchmarks.Run
    @State private var label=""
    var body:some View {
        let s=run.summary
        VStack(alignment:.leading,spacing:8) {
            Grid(alignment:.leading,horizontalSpacing:16,verticalSpacing:3) {
                row("Average",fmt(s.avgFPS,1)+" fps","Frames",
                    "\(s.frames) in \(fmt(s.seconds,1)) s")
                row("1% low",fmt(s.low1FPS,1)+" fps","0.1% low",fmt(s.low01FPS,1)+" fps")
                row("Frame time p50",fmt(s.p50,2)+" ms","p95 / p99",fmt(s.p95,2)+" / "+fmt(s.p99,2)+" ms")
                row("Longest frame",fmt(s.maxMs,1)+" ms","Stutters",
                    "\(s.stutters) (over 2× the median)")
                row("GPU time p50",fmt(s.gpuP50,2)+" ms","p95",fmt(s.gpuP95,2)+" ms")
                row("GPU-bound frames",fmt(s.gpuBound*100,0)+" %","Present → display",
                    s.latencyP50.map { "\(fmt($0,1)) ms p50, \(fmt(s.latencyP95 ?? 0,1)) p95" } ?? "not recorded")
            }.font(LauncherStyle.font(11))
            chart.frame(height:110)
            HStack(spacing:8) {
                TextField("Name this run (e.g. upscaling on)",text:$label).textFieldStyle(.roundedBorder).font(LauncherStyle.font(12))
                    .onSubmit { model.renameRun(run,label) }
                Button("Save Name") { model.renameRun(run,label) }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
                    .disabled(label.trimmingCharacters(in:.whitespaces)==run.label)
                Button("Delete",role:.destructive) { model.deleteRun(run) }.buttonStyle(.borderless).hoverGrow().foregroundStyle(.red).font(LauncherStyle.font(12,.medium))
            }
            Text("Process \(run.process)").font(LauncherStyle.font(10)).foregroundStyle(.tertiary)
        }.onAppear { label=run.label }
    }
    private func row(_ a:String,_ av:String,_ b:String,_ bv:String)->some View {
        GridRow {
            Text(a).foregroundStyle(.secondary);Text(av).monospacedDigit()
            Text(b).foregroundStyle(.secondary);Text(bv).monospacedDigit()
        }
    }
    /// Frame time per frame (at most ~1200 points: the longest frame of each bucket, so spikes stay).
    private var chart:some View {
        let step=max(1,run.intervals.count/1200)
        let points=stride(from:0,to:run.intervals.count,by:step).map { i in
            (i,Double(run.intervals[i..<min(i+step,run.intervals.count)].max() ?? 0))
        }
        return Chart {
            ForEach(points,id:\.0) { i,ms in LineMark(x:.value("Frame",i),y:.value("ms",ms)).interpolationMethod(.linear) }
            RuleMark(y:.value("p50",run.summary.p50)).foregroundStyle(.secondary.opacity(0.5)).lineStyle(StrokeStyle(lineWidth:1,dash:[3,3]))
        }.chartYAxisLabel("ms").chartXAxis(.hidden)
    }
}

/// Picks a Steam game to record.
private struct BenchmarkGamePicker:View {
    let games:[Profiles.Game]
    let taken:Set<String>
    let pick:(Profiles.Game)->Void
    @State private var search=""
    private var matches:[Profiles.Game] {
        let query=search.trimmingCharacters(in:.whitespaces),open=games.filter { !taken.contains($0.appID) }
        return query.isEmpty ? open : open.filter { $0.name.localizedCaseInsensitiveContains(query) || $0.appID.hasPrefix(query) }
    }
    var body:some View {
        VStack(alignment:.leading,spacing:8) {
            TextField("Search games or type an app id",text:$search).textFieldStyle(.roundedBorder)
                .onSubmit { if let first=matches.first { pick(first) } else if Profiles.valid(search) { pick(Profiles.Game(appID:search,name:"App \(search)")) } }
            ScrollView {
                VStack(alignment:.leading,spacing:2) {
                    ForEach(matches) { game in
                        Button { pick(game) } label: {
                            VStack(alignment:.leading,spacing:1) {
                                Text(game.name).font(LauncherStyle.font(13,.medium)).lineLimit(1)
                                Text("App \(game.appID)").font(LauncherStyle.font(11)).foregroundStyle(.secondary)
                            }.frame(maxWidth:.infinity,alignment:.leading).padding(.horizontal,6).padding(.vertical,5).contentShape(Rectangle())
                        }.buttonStyle(.plain).hoverGrow()
                    }
                    if matches.isEmpty {
                        Text(Profiles.valid(search) ? "Press Return to use app \(search)." : "No game matches.").font(LauncherStyle.font(11)).foregroundStyle(.secondary).padding(6)
                    }
                }.frame(maxWidth:.infinity,alignment:.leading)
            }.frame(maxHeight:260).fixedSize(horizontal:false,vertical:true)
        }.padding(12).frame(width:320)
    }
}
