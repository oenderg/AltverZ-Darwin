// The Mac app's icon from the game's own (the APK's square, full-bleed pvz2_icon.png):
// the picture cut to the shape of a Mac icon -- a rounded square of 824 in a 1024 canvas,
// with a soft shadow under it -- at every size an .iconset holds. A square that fills
// the whole canvas is not a Mac icon's shape, and recent macOS shows such an icon inside
// a grey frame.
//
//   swift tools/macos-icon.swift <icon.png> <out.iconset>      then: iconutil -c icns <out.iconset>
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count == 3,
      let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil),
      let picture = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    FileHandle.standardError.write("usage: macos-icon.swift <icon.png> <out.iconset>\n".data(using: .utf8)!)
    exit(1)
}
let out = URL(fileURLWithPath: args[2])
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

// Apple's grid for a 1024 icon: the shape is 824 square at 100 from each edge, its
// corners 185.4 round; the shadow falls 10 down, blurred over 20.
func icon(_ size: Int) -> CGImage {
    let s = CGFloat(size) / 1024
    let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .high
    let body = CGRect(x: 100 * s, y: 100 * s, width: 824 * s, height: 824 * s)
    let shape = CGPath(roundedRect: body, cornerWidth: 185.4 * s, cornerHeight: 185.4 * s, transform: nil)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10 * s), blur: 20 * s,
                  color: CGColor(gray: 0, alpha: 0.3))
    ctx.addPath(shape)
    ctx.setFillColor(CGColor(gray: 0, alpha: 1))
    ctx.fillPath()
    ctx.restoreGState()
    ctx.addPath(shape)
    ctx.clip()
    ctx.draw(picture, in: body)
    return ctx.makeImage()!
}

for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let name = scale == 1 ? "icon_\(points)x\(points).png" : "icon_\(points)x\(points)@2x.png"
        let dest = CGImageDestinationCreateWithURL(out.appendingPathComponent(name) as CFURL,
                                                   UTType.png.identifier as CFString, 1, nil)!
        CGImageDestinationAddImage(dest, icon(points * scale), nil)
        CGImageDestinationFinalize(dest)
    }
}
