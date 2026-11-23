// screentext — read the text out of a simulator screenshot with the Vision
// framework, so the iOS regression suites can assert on what a guest screen
// actually says instead of on how many pixels changed.
//
// Build: swiftc -O -o screentext ScreenText.swift   (see scripts/ocr/build.sh)
//
// Usage:
//   screentext <image.png> [options]
//
// Options:
//   --crop x,y,w,h     Restrict recognition to a normalized (0..1) rectangle,
//                      origin top-left. Use it to cut the host chrome away and
//                      leave only the guest display band.
//   --scale <n>        Upscale the (cropped) image before recognition. Guest
//                      text is tiny; 2-3 is usually the difference between
//                      nothing and a clean read. Default 2.
//   --fast             Trade accuracy for speed (Vision's .fast level).
//   --languages a,b    Recognition languages. Default en-US.
//   --min-confidence f Drop observations below this confidence. Default 0.3.
//   --json             Emit {"lines":[{"text","confidence","rect"}]} instead of
//                      one line of text per observation.
//   --contains <text>  Exit 0 if the recognized text contains it (case- and
//                      whitespace-insensitive), 1 if not. Repeatable: any match
//                      wins. Still prints the recognized text to stdout.

import Foundation
import CoreGraphics
import ImageIO
import Vision
import UniformTypeIdentifiers

// MARK: - Arguments

struct Options {
    var imagePath: String = ""
    var crop: CGRect? = nil
    var scale: CGFloat = 2
    var fast = false
    var languages: [String] = ["en-US"]
    var minConfidence: Float = 0.3
    var json = false
    var wanted: [String] = []
}

func die(_ message: String) -> Never {
    FileHandle.standardError.write(Data("screentext: \(message)\n".utf8))
    exit(2)
}

func parseArguments() -> Options {
    var opts = Options()
    var args = Array(CommandLine.arguments.dropFirst())

    func next(_ flag: String) -> String {
        guard !args.isEmpty else { die("\(flag) needs a value") }
        return args.removeFirst()
    }

    while !args.isEmpty {
        let arg = args.removeFirst()
        switch arg {
        case "--crop":
            let parts = next(arg).split(separator: ",").compactMap { Double($0) }
            guard parts.count == 4 else { die("--crop wants x,y,w,h") }
            opts.crop = CGRect(x: parts[0], y: parts[1], width: parts[2], height: parts[3])
        case "--scale":
            guard let value = Double(next(arg)), value > 0 else { die("--scale wants a positive number") }
            opts.scale = CGFloat(value)
        case "--fast":
            opts.fast = true
        case "--languages":
            opts.languages = next(arg).split(separator: ",").map(String.init)
        case "--min-confidence":
            guard let value = Float(next(arg)) else { die("--min-confidence wants a number") }
            opts.minConfidence = value
        case "--json":
            opts.json = true
        case "--contains":
            opts.wanted.append(next(arg))
        case "-h", "--help":
            print("usage: screentext <image.png> [--crop x,y,w,h] [--scale n] [--fast]",
                  "[--languages a,b] [--min-confidence f] [--json] [--contains text ...]",
                  separator: " ")
            exit(0)
        default:
            guard opts.imagePath.isEmpty else { die("unexpected argument: \(arg)") }
            opts.imagePath = arg
        }
    }

    guard !opts.imagePath.isEmpty else { die("no image given") }
    return opts
}

// MARK: - Image loading

func loadImage(_ path: String, crop: CGRect?, scale: CGFloat) -> CGImage {
    let url = URL(fileURLWithPath: path)
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
        die("cannot read image: \(path)")
    }

    var result = image
    if let crop {
        let rect = CGRect(x: crop.minX * CGFloat(image.width),
                          y: crop.minY * CGFloat(image.height),
                          width: crop.width * CGFloat(image.width),
                          height: crop.height * CGFloat(image.height)).integral
        guard let cropped = image.cropping(to: rect), cropped.width > 0, cropped.height > 0 else {
            die("crop \(crop) falls outside the image")
        }
        result = cropped
    }

    guard scale != 1 else { return result }

    let width = Int((CGFloat(result.width) * scale).rounded())
    let height = Int((CGFloat(result.height) * scale).rounded())
    guard let context = CGContext(data: nil,
                                  width: width,
                                  height: height,
                                  bitsPerComponent: 8,
                                  bytesPerRow: 0,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else {
        die("cannot allocate a \(width)x\(height) scaling context")
    }
    context.interpolationQuality = .high
    context.draw(result, in: CGRect(x: 0, y: 0, width: width, height: height))
    guard let scaled = context.makeImage() else { die("scaling failed") }
    return scaled
}

// MARK: - Recognition

struct Line {
    let text: String
    let confidence: Float
    let rect: CGRect
}

func recognize(_ image: CGImage, _ opts: Options) -> [Line] {
    let request = VNRecognizeTextRequest()
    request.recognitionLevel = opts.fast ? .fast : .accurate
    // Guest UIs are full of product names and truncated labels; language
    // correction rewrites those into unrelated dictionary words.
    request.usesLanguageCorrection = false
    request.recognitionLanguages = opts.languages

    let handler = VNImageRequestHandler(cgImage: image, options: [:])
    do {
        try handler.perform([request])
    } catch {
        die("recognition failed: \(error.localizedDescription)")
    }

    let observations = request.results ?? []
    return observations.compactMap { observation in
        guard let candidate = observation.topCandidates(1).first,
              candidate.confidence >= opts.minConfidence else { return nil }
        return Line(text: candidate.string, confidence: candidate.confidence, rect: observation.boundingBox)
    }
}

// MARK: - Matching

// Compare on letters and digits only: OCR drifts on punctuation, spacing and
// case far more often than it drifts on the characters that carry the meaning.
func normalized(_ text: String) -> String {
    text.lowercased().unicodeScalars.reduce(into: "") { result, scalar in
        if CharacterSet.alphanumerics.contains(scalar) {
            result.unicodeScalars.append(scalar)
        }
    }
}

// MARK: - Main

let opts = parseArguments()
let image = loadImage(opts.imagePath, crop: opts.crop, scale: opts.scale)
let lines = recognize(image, opts)

if opts.json {
    let payload = lines.map { line -> [String: Any] in
        [
            "text": line.text,
            "confidence": line.confidence,
            "rect": ["x": line.rect.minX, "y": line.rect.minY,
                     "w": line.rect.width, "h": line.rect.height],
        ]
    }
    let data = try JSONSerialization.data(withJSONObject: ["lines": payload],
                                          options: [.prettyPrinted, .sortedKeys])
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data("\n".utf8))
} else {
    for line in lines {
        print(line.text)
    }
}

if !opts.wanted.isEmpty {
    let haystack = normalized(lines.map(\.text).joined(separator: " "))
    let matched = opts.wanted.contains { !normalized($0).isEmpty && haystack.contains(normalized($0)) }
    exit(matched ? 0 : 1)
}
