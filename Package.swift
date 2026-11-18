// swift-tools-version:5.5

import Foundation
import PackageDescription

let optimizesDebug = !#filePath.contains("/checkouts/") && ProcessInfo.processInfo.environment["TWIDGET_DEBUG_UNOPTIMIZED"] == nil


// The card renderer and the scripted targets (TwidgetRenderer, HTMLScript, HTML3D, Twidget3DSandbox) were removed with
// the old card runtime; cards on Apple platforms come back later, on twcard.
let package = Package(
    name: "twidget",
    platforms: [.macOS(.v10_13)],
    products: [
        .library(name: "LiteHTML", targets: ["LiteHTML"]),
        .library(name: "TwidgetAssets", targets: ["TwidgetAssets"]),
        .library(name: "TwidgetIcons", targets: ["TwidgetIcons"]),
        .library(name: "HTMLSandbox", targets: ["HTMLSandbox"]),
        .library(name: "Twidget3D", targets: ["Twidget3D"]),
    ],
    targets: [
        .target(
            name: "TwidgetAssets",
            path: "apple/Sources/TwidgetAssets",
            exclude: ["icons", "icons.json", "twa_icon_data.inc"],
            sources: ["twa_icons.cpp"],
            publicHeadersPath: "include"),
        .target(
            name: "TwidgetIcons",
            dependencies: ["TwidgetAssets"],
            path: "apple/Sources/TwidgetIcons"),
        // The litehtml fork both renderers share. Its headers are C++ and reach LiteHTML through header search
        // paths: a public headers folder would make SwiftPM wrap them in a Clang module, so it names none.
        .target(
            name: "TwidgetLitehtml",
            path: "litehtml",
            exclude: ["LICENSE", "src/gumbo/LICENSE", "src/cjk_rules_table.inc", "src/cjk_scripts_table.inc", "src/cjk_tate_chu_yoko_table.inc"],
            sources: ["src"],
            publicHeadersPath: "no-public-headers",
            cSettings: [
                .headerSearchPath("include"),
                .headerSearchPath("include/litehtml"),
                .headerSearchPath("src"),
                .headerSearchPath("gumbo-include"),
            ],
            cxxSettings: [
                .headerSearchPath("include"),
                .headerSearchPath("include/litehtml"),
                .headerSearchPath("src"),
                .headerSearchPath("gumbo-include"),
            ],
            linkerSettings: [.linkedFramework("CoreFoundation")]),
        .target(
            name: "LiteHTML",
            dependencies: ["TwidgetAssets", "TwidgetLitehtml"],
            path: "apple/Sources/LiteHTML",
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("../../../litehtml/include"),
                .headerSearchPath("../../../litehtml/include/litehtml"),
                .headerSearchPath("../../../litehtml/src"),
                .headerSearchPath("../../../litehtml/gumbo-include"),
                .headerSearchPath("core"),
            ],
            cxxSettings: [
                .headerSearchPath("../../../litehtml/include"),
                .headerSearchPath("../../../litehtml/include/litehtml"),
                .headerSearchPath("../../../litehtml/src"),
                .headerSearchPath("../../../litehtml/gumbo-include"),
                .headerSearchPath("core"),
            ],
            linkerSettings: [
                .linkedFramework("Cocoa"),
                .linkedFramework("CoreText"),
                .linkedFramework("ImageIO"),
                .linkedFramework("Metal"),
                .linkedFramework("IOSurface"),
                .linkedFramework("CoreImage"),
                .linkedLibrary("z"),
            ]),
        .target(
            name: "HTMLSandbox",
            path: "apple/Sources/HTMLSandbox"),
        .target(
            name: "Twidget3D",
            path: "apple/Sources/Twidget3D",
            resources: [.copy("Resources/three.js"), .copy("Resources/twidget3d.js")],
            linkerSettings: [.linkedFramework("Metal"), .linkedFramework("QuartzCore")]),
        .testTarget(
            name: "TwidgetIconsTests",
            dependencies: ["TwidgetIcons"],
            path: "apple/Tests/TwidgetIconsTests"),
        .testTarget(
            name: "Twidget3DTests",
            dependencies: ["Twidget3D"],
            path: "apple/Tests/Twidget3DTests"),
        .testTarget(
            name: "LiteHTMLTests",
            dependencies: ["LiteHTML", "TwidgetAssets"],
            path: "apple/Tests/LiteHTMLTests",
            cxxSettings: [
                .headerSearchPath("../../../litehtml/include"),
                .headerSearchPath("../../../litehtml/include/litehtml"),
                .headerSearchPath("../../Sources/LiteHTML/core"),
                .headerSearchPath("../../Sources/LiteHTML/include"),
            ],
            linkerSettings: [.linkedFramework("XCTest")]),
    ],
    cxxLanguageStandard: .gnucxx17
)

if optimizesDebug {
    for target in package.targets where target.type == .regular {
        target.cSettings = (target.cSettings ?? []) + [.unsafeFlags(["-O3"])]
        target.cxxSettings = (target.cxxSettings ?? []) + [.unsafeFlags(["-O3"]), .define("_LIBCPP_HARDENING_MODE", to: "_LIBCPP_HARDENING_MODE_FAST", .when(configuration: .debug))]
        target.swiftSettings = (target.swiftSettings ?? []) + [.unsafeFlags(["-O"], .when(configuration: .debug))]
    }
}
