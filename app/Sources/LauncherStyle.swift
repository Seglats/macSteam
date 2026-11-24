import SwiftUI
import AppKit

// Shared visual vocabulary. Headings, buttons and labels (semibold and up, or 17 pt
// and up) are set in Chakra Petch, bundled under SIL OFL 1.1 (Resources/Fonts); smaller
// regular text stays in the system font, which reads best at body sizes.
enum LauncherStyle {
    static let accent=Color.primary
    static let ink=Color(nsColor:NSColor(name:nil) { appearance in
        appearance.bestMatch(from:[.darkAqua,.aqua]) == .darkAqua ? .black : .white
    })
    static let contentWidth:CGFloat=440
    static let radius:CGFloat=12
    private static let displayFonts:Bool={
        guard let folder=Bundle.main.resourceURL?.appendingPathComponent("Fonts"),
              let files=try? FileManager.default.contentsOfDirectory(at:folder,includingPropertiesForKeys:nil) else { return false }
        for file in files where file.pathExtension == "ttf" { CTFontManagerRegisterFontsForURL(file as CFURL,.process,nil) }
        return NSFont(name:"ChakraPetch-SemiBold",size:12) != nil
    }()
    static func font(_ size:CGFloat, _ weight:Font.Weight = .regular) -> Font {
        let display=weight == .semibold || weight == .bold || weight == .heavy || weight == .black || size >= 17
        guard display && displayFonts else { return .system(size:size,weight:weight) }
        let face=weight == .bold || weight == .heavy || weight == .black ? "ChakraPetch-Bold"
            : weight == .semibold ? "ChakraPetch-SemiBold" : "ChakraPetch-Medium"
        // Chakra Petch runs small next to SF Pro at the same point size.
        return .custom(face,fixedSize:size*1.08)
    }
    /// macSteam is always dark (Main.swift sets the dark appearance; the system theme doesn't
    /// apply, Rasmus 2026-11-24): a blue ground like Steam's logo, dark navy at the top to a
    /// lighter blue at the bottom, a little darker than Steam's own gradient.
    static let groundTop=Color(red:0.020,green:0.035,blue:0.094)      // #05091a
    static let groundMiddle=Color(red:0.035,green:0.098,blue:0.212)   // #091936
    static let groundBottom=Color(red:0.063,green:0.220,blue:0.384)   // #103862
    static func ground(_ scheme:ColorScheme) -> Color { groundMiddle }
    /// Panels: translucent dark navy, so the gradient shows through.
    static func surface(_ scheme:ColorScheme) -> Color {
        Color(red:0.02,green:0.05,blue:0.12).opacity(0.55)
    }
}
struct LauncherBackground: View {
    /// The gradient's axis leans π/20 (9°) off vertical, so its bands slope down to the left;
    /// worked out from the real size so the angle holds in any window or sheet.
    static let slope=Double.pi/20
    var body:some View {
        GeometryReader { geometry in
            let w=max(geometry.size.width,1),h=max(geometry.size.height,1)
            let dx=tan(Self.slope)*h/w/2
            LinearGradient(stops:[.init(color:LauncherStyle.groundTop,location:0),
                                  .init(color:LauncherStyle.groundMiddle,location:0.55),
                                  .init(color:LauncherStyle.groundBottom,location:1)],
                           startPoint:UnitPoint(x:0.5-dx,y:0),endPoint:UnitPoint(x:0.5+dx,y:1))
        }
        .ignoresSafeArea().allowsHitTesting(false).accessibilityHidden(true)
    }
}
struct AppArtwork: View {
    var size:CGFloat=128
    var badge:String?
    private var artwork:NSImage {
        Bundle.main.url(forResource:"AppIcon",withExtension:"icns").flatMap { NSImage(contentsOf:$0) }
            ?? NSImage(named:NSImage.applicationIconName) ?? NSImage()
    }
    var body:some View {
        Image(nsImage:artwork).resizable().interpolation(.high).frame(width:size,height:size)
            .overlay(alignment:.bottomTrailing) {
                if let badge {
                    Image(systemName:badge).font(.system(size:22,weight:.semibold))
                        .foregroundStyle(LauncherStyle.ink).frame(width:44,height:44)
                        .background(LauncherStyle.accent,in:Circle()).offset(x:8,y:4)
                }
            }.accessibilityHidden(true)
    }
}
struct LauncherHeading: View {
    let title:String
    let subtitle:String
    var compact=false
    var body:some View {
        VStack(spacing:12) {
            Text(title).font(LauncherStyle.font(compact ? 28 : 36,.bold))
                .tracking(-0.8).foregroundStyle(.primary).accessibilityAddTraits(.isHeader)
            if !subtitle.isEmpty {
                Text(subtitle).font(LauncherStyle.font(16)).lineSpacing(3).foregroundStyle(.secondary)
            }
        }.multilineTextAlignment(.center).fixedSize(horizontal:false,vertical:true)
    }
}
struct LauncherPanel<Content:View>: View {
    @Environment(\.colorScheme) private var scheme
    var inset:CGFloat=20
    @ViewBuilder let content:Content
    var body:some View {
        content.padding(inset).frame(maxWidth:.infinity,alignment:.leading)
            .background(LauncherStyle.surface(scheme),in:RoundedRectangle(cornerRadius:LauncherStyle.radius,style:.continuous))
            .overlay { RoundedRectangle(cornerRadius:LauncherStyle.radius,style:.continuous).strokeBorder(.primary.opacity(0.05),lineWidth:1) }
    }
}
struct LauncherPrimaryAction: View {
    let title:String
    let action:()->Void
    var body:some View {
        Button(action:action) { Text(title).font(LauncherStyle.font(15,.semibold)) }
            .buttonStyle(LauncherActionStyle()).hoverGrow()
    }
}
/// "Open Steam": the hero action with a neutral game-controller symbol, so it reads as
/// the way to the games without borrowing Valve's artwork.
struct ClientAction: View {
    let title:String
    var secondary=false
    let action:()->Void
    var body:some View {
        Button(action:action) {
            HStack(spacing:9) {
                Image(systemName:"gamecontroller.fill").font(.system(size:15,weight:.semibold)).accessibilityHidden(true)
                Text(title).font(LauncherStyle.font(15,.semibold))
            }
        }.buttonStyle(secondary ? LauncherActionStyle(fill:Color.primary.opacity(0.1),text:.primary) : LauncherActionStyle()).hoverGrow()
    }
}
/// Stops what the launcher is doing (the launching screen's Cancel).
struct LauncherCancelAction: View {
    let title:String
    let action:()->Void
    var body:some View {
        Button(action:action) { Text(title).font(LauncherStyle.font(15,.semibold)) }
            .buttonStyle(LauncherActionStyle()).hoverGrow()
    }
}
private struct LauncherActionStyle: ButtonStyle {
    var fill=LauncherStyle.accent
    var text=LauncherStyle.ink
    @Environment(\.isEnabled) private var enabled
    @Environment(\.isFocused) private var focused
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    func makeBody(configuration:Configuration)->some View {
        configuration.label.frame(minWidth:200,minHeight:46).padding(.horizontal,16)
            .foregroundStyle(enabled ? text : Color.secondary)
            .background(enabled ? fill : Color.secondary.opacity(0.12),in:Capsule())
            .contentShape(Capsule())
            .overlay { Capsule().strokeBorder(focused ? Color.primary.opacity(0.8) : .clear,lineWidth:2).padding(3) }
            .opacity(configuration.isPressed ? 0.82 : 1)
            .scaleEffect(configuration.isPressed && !reduceMotion ? 0.98 : 1)
            .animation(reduceMotion ? nil : .easeOut(duration:0.16),value:configuration.isPressed)
    }
}
struct LauncherNotice: View {
    let text:String
    var symbol="info.circle"
    /// A button under the text that does what it asks.
    var action:(title:String, run:()->Void)?=nil
    var body:some View {
        Label {
            VStack(alignment:.leading,spacing:12) {
                Text(text).lineSpacing(3).fixedSize(horizontal:false,vertical:true)
                if let action {
                    Button(action.title,action:action.run).buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).foregroundStyle(.primary)
                }
            }
        } icon: { Image(systemName:symbol) }
            .font(LauncherStyle.font(13)).foregroundStyle(.secondary)
            .padding(16).frame(maxWidth:.infinity,alignment:.leading)
            .background(LauncherStyle.accent.opacity(0.09),in:RoundedRectangle(cornerRadius:16,style:.continuous))
    }
}
struct SheetHero: View {
    let symbol:String
    let title:String
    let subtitle:String
    var body:some View {
        VStack(spacing:20) {
            Image(systemName:symbol).font(.system(size:28,weight:.medium))
                .foregroundStyle(.primary).frame(width:64,height:64)
                .background(LauncherStyle.accent.opacity(0.14),in:RoundedRectangle(cornerRadius:20,style:.continuous))
                .accessibilityHidden(true)
            LauncherHeading(title:title,subtitle:subtitle,compact:true)
        }
    }
}

/// Buttons in the app grow 3 % while the pointer is over them (added after every .buttonStyle).
/// Disabled ones stay as they are; with Reduce Motion the size changes without animating.
struct HoverGrow:ViewModifier {
    @State private var hovering=false
    @Environment(\.isEnabled) private var enabled
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    func body(content:Content)->some View {
        content.scaleEffect(hovering && enabled ? 1.03 : 1)
            .animation(reduceMotion ? nil : .easeOut(duration:0.12),value:hovering && enabled)
            .onHover { hovering=$0 }
    }
}
extension View {
    func hoverGrow()->some View { modifier(HoverGrow()) }
}
