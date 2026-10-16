#!/usr/bin/env python3
"""Render the SVG app icon with macOS AppKit; JUCE generates ICNS at build time."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SWIFT = r'''
import AppKit
let input = URL(fileURLWithPath: CommandLine.arguments[1])
let output = URL(fileURLWithPath: CommandLine.arguments[2])
let size = Int(CommandLine.arguments[3])!
guard let source = NSImage(contentsOf: input),
      let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
          bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
          colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
      let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
    fatalError("Cannot read SVG or create the icon bitmap")
}
bitmap.size = NSSize(width: size, height: size)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = context
context.imageInterpolation = .high
source.draw(in: NSRect(x: 0, y: 0, width: size, height: size),
            from: .zero, operation: .copy, fraction: 1)
NSGraphicsContext.restoreGraphicsState()
guard let png = bitmap.representation(using: .png, properties: [:]) else {
    fatalError("Cannot encode icon PNG")
}
try png.write(to: output)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/icon")
    parser.add_argument("--output", type=Path, default=ROOT / "assets/flows-icon.png")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("AppKit requires macOS")
    source = ROOT / "assets/flows-icon.svg"
    master = args.output.resolve()
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    master.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="flows-icon-") as temporary:
        swift = Path(temporary) / "render.swift"
        executable = Path(temporary) / "render"
        swift.write_text(SWIFT, encoding="utf-8")
        subprocess.run(["xcrun", "swiftc", "-module-cache-path", str(output / "swift-cache"),
                        str(swift), "-o", str(executable)], check=True)
        subprocess.run([str(executable), str(source), str(master), "1024"], check=True)
    print(master)


if __name__ == "__main__":
    main()
