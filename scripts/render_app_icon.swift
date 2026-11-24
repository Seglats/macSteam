// Renders app/Resources/AppIcon.svg (the owner's mark) onto the window's ground as the 1024-pixel
// app/Resources/AppIcon.png that build_native_app.py packages (app_icon.swift). Run it after
// changing the SVG, then update APP_ICON_SHA256 in repo_guard.py:
//   xcrun swift scripts/render_app_icon.swift app/Resources/AppIcon.svg app/Resources/AppIcon.png
import AppKit
let svg=URL(fileURLWithPath:CommandLine.arguments[1]), out=URL(fileURLWithPath:CommandLine.arguments[2])
guard let mark=NSImage(contentsOf:svg) else { fatalError("SVG not readable") }
let side=1024
let rep=NSBitmapImageRep(bitmapDataPlanes:nil,pixelsWide:side,pixelsHigh:side,bitsPerSample:8,samplesPerPixel:4,hasAlpha:true,isPlanar:false,colorSpaceName:.deviceRGB,bytesPerRow:0,bitsPerPixel:0)!
let ctx=NSGraphicsContext(bitmapImageRep:rep)!
NSGraphicsContext.saveGraphicsState();NSGraphicsContext.current=ctx;ctx.imageInterpolation = .high
func c(_ h:Int)->NSColor { NSColor(srgbRed:CGFloat(h>>16&255)/255,green:CGFloat(h>>8&255)/255,blue:CGFloat(h&255)/255,alpha:1) }
// The window's ground (LauncherStyle): dark top, lighter bottom, sloped pi/20.
let g=NSGradient(colors:[c(0x103862),c(0x091936),c(0x05091a)],atLocations:[0,0.45,1],colorSpace:.sRGB)!
g.draw(in:NSRect(x:0,y:0,width:side,height:side),angle:90 + 9)
let s=CGFloat(side)*0.74/0.762   // the mark fills ~76% of its own viewBox
let r=NSRect(x:(CGFloat(side)-s)/2,y:(CGFloat(side)-s)/2,width:s,height:s)
mark.draw(in:r,from:.zero,operation:.sourceOver,fraction:1)
ctx.flushGraphics();NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using:.png,properties:[:])!.write(to:out)
