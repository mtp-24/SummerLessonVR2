// tools/font/mkfont.swift: the agent's 8x12 bitmap font (agent/font8x12.h) from Menlo, ASCII 32-126:
//   swift tools/font/mkfont.swift > agent/font8x12.h
// SPDX-License-Identifier: GPL-3.0-or-later
import CoreGraphics
import CoreText
import Foundation

let W = 8, H = 12
let font = CTFontCreateWithName("Menlo-Bold" as CFString, 11.0, nil)
let space = CGColorSpaceCreateDeviceGray()
print("/* The agent's 8x12 font (ASCII 32-126), Menlo Bold 11 rendered by tools/font/mkfont.swift: a")
print(" * row a byte, the leftmost pixel in bit 7. Generated: do not edit. */")
print("static const unsigned char k_font8x12[95][12] = {")
for c in 32...126 {
  let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W,
                      space: space, bitmapInfo: CGImageAlphaInfo.none.rawValue)!
  ctx.setFillColor(gray: 0, alpha: 1)
  ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
  ctx.setShouldAntialias(true)
  let s = String(UnicodeScalar(c)!)
  let attr = NSAttributedString(string: s, attributes: [
    NSAttributedString.Key(kCTFontAttributeName as String): font,
    NSAttributedString.Key(kCTForegroundColorFromContextAttributeName as String): true])
  ctx.setFillColor(gray: 1, alpha: 1)
  let line = CTLineCreateWithAttributedString(attr)
  ctx.textPosition = CGPoint(x: 1, y: 3) // baseline 3 px up from the bottom
  CTLineDraw(line, ctx)
  let p = ctx.data!.bindMemory(to: UInt8.self, capacity: W * H)
  var rows: [String] = []
  for y in 0..<H { // CG's rows bottom-up in memory? the context's first row is the top
    var b = 0
    for x in 0..<W where p[y * W + x] > 80 { b |= 0x80 >> x }
    rows.append(String(format: "0x%02x", b))
  }
  let shown = c == 92 ? "backslash" : (c == 39 ? "quote" : s)
  print("  {\(rows.joined(separator: ", "))}, /* \(shown) */")
}
print("};")
