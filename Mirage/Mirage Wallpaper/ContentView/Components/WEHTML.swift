//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import WebKit

// WE labels are HTML. Rich content (images / links / tables) is rendered
// faithfully with WKWebView; everything else is flattened to plain text so most
// rows stay native and cheap.
enum WEHTML {
    static func isRich(_ raw: String) -> Bool {
        let s = raw.replacingOccurrences(of: "＜", with: "<").replacingOccurrences(of: "＞", with: ">")
        return s.range(of: "<\\s*(img|a|table|center|iframe|video)\\b",
                       options: [.regularExpression, .caseInsensitive]) != nil
    }

    /// Rich labels that still need an out-of-process web view.
    ///
    /// `NSAttributedString`'s HTML importer resolves referenced resources
    /// synchronously on the calling (main) thread, so a label pointing at a
    /// remote image would beachball the settings panel. Those keep the web
    /// view; everything else — formatting, links, tables, local images —
    /// renders natively and costs no WebKit content process at all.
    static func needsWebView(_ raw: String) -> Bool {
        let s = normalizeAngles(raw)
        return s.range(of: "<\\s*(img|iframe|video)\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*(https?:|//)",
                       options: [.regularExpression, .caseInsensitive]) != nil
    }

    @MainActor private static let imports = ImportQueue()

    @MainActor
    static func attributed(_ raw: String) async -> AttributedString? {
        await imports.value(for: raw)
    }

    @MainActor
    final class ImportQueue {
        private final class Request {
            let html: String
            var consumers: [UUID: CheckedContinuation<AttributedString?, Never>] = [:]
            var active = false

            init(_ html: String) { self.html = html }
        }

        private let importer: (String) async -> NSAttributedString?
        private let cache = NSCache<NSString, Box>()
        private var requests: [String: Request] = [:]
        private var waiting: [Request] = []
        private var activeCount = 0

        init(importer: @escaping (String) async -> NSAttributedString? = { raw in
            await withCheckedContinuation { continuation in
                NSAttributedString.loadFromHTML(string: normalizeAngles(raw), options: [.timeout: 2.0]) {
                    value, _, _ in continuation.resume(returning: value)
                }
            }
        }) {
            self.importer = importer
            cache.countLimit = 256
        }

        func value(for raw: String) async -> AttributedString? {
            guard !Task.isCancelled else { return nil }
            if let cached = cache.object(forKey: raw as NSString) { return cached.value }
            let token = UUID()
            return await withTaskCancellationHandler {
                await withCheckedContinuation { continuation in
                    guard !Task.isCancelled else { continuation.resume(returning: nil); return }
                    let request: Request
                    if let existing = requests[raw] {
                        request = existing
                    } else {
                        request = Request(raw)
                        requests[raw] = request
                        waiting.append(request)
                    }
                    request.consumers[token] = continuation
                    startWaiting()
                }
            } onCancel: {
                Task { @MainActor [weak self] in self?.cancel(raw, token: token) }
            }
        }

        private func cancel(_ raw: String, token: UUID) {
            guard let request = requests[raw] else { return }
            request.consumers.removeValue(forKey: token)?.resume(returning: nil)
            if request.consumers.isEmpty && !request.active {
                requests[raw] = nil
                waiting.removeAll { $0 === request }
            }
        }

        private func startWaiting() {
            while activeCount < 2, !waiting.isEmpty {
                let request = waiting.removeFirst()
                request.active = true
                activeCount += 1
                Task { @MainActor in
                    let parsed = await importer(request.html)
                    var result: AttributedString?
                    if !request.consumers.isEmpty, let parsed {
                        var value = AttributedString(parsed)
                        for run in value.runs {
                            value[run.range].foregroundColor = nil
                            value[run.range].backgroundColor = nil
                            value[run.range].font = nil
                        }
                        while let last = value.characters.last, last.isNewline || last == " " {
                            value.removeSubrange(value.index(beforeCharacter: value.endIndex)..<value.endIndex)
                        }
                        cache.setObject(Box(value), forKey: request.html as NSString)
                        result = value
                    }
                    requests[request.html] = nil
                    activeCount -= 1
                    let consumers = request.consumers.values
                    request.consumers.removeAll()
                    consumers.forEach { $0.resume(returning: result) }
                    startWaiting()
                }
            }
        }
    }

    private final class Box {
        let value: AttributedString
        init(_ value: AttributedString) { self.value = value }
    }

    private static func normalizeAngles(_ raw: String) -> String {
        raw.replacingOccurrences(of: "＜", with: "<")
            .replacingOccurrences(of: "＞", with: ">")
    }

    private static let plainCache: NSCache<NSString, NSString> = {
        let cache = NSCache<NSString, NSString>()
        cache.countLimit = 1024
        return cache
    }()

    static func plain(_ raw: String) -> String {
        if let cached = plainCache.object(forKey: raw as NSString) { return cached as String }
        var s = raw
            .replacingOccurrences(of: "＜", with: "<")
            .replacingOccurrences(of: "＞", with: ">")
        func rx(_ pattern: String, _ rep: String) {
            s = s.replacingOccurrences(of: pattern, with: rep,
                                       options: [.regularExpression, .caseInsensitive])
        }
        rx("<\\s*br\\s*/?>", "\n")
        rx("<\\s*/?\\s*(p|div|center)\\s*>", "\n")
        rx("<[^>]*>", "")
        rx("<\\s*/?\\s*[a-zA-Z][^<]*$", "")            // truncated trailing tag
        rx("(?m)^[ \\t]*/?(?:center|big|small|strong|font|span|div|sub|sup|b|i|u|p|a)[ \\t]*>[ \\t]*$", "")
        s = decodeEntities(s)
        rx("[ \\t]+", " ")
        rx("\\n{3,}", "\n\n")
        let result = s.trimmingCharacters(in: .whitespacesAndNewlines)
        plainCache.setObject(result as NSString, forKey: raw as NSString)
        return result
    }

    static func decodeEntities(_ s: String) -> String {
        guard s.contains("&") else { return s }
        var out = ""
        out.reserveCapacity(s.count)
        var i = s.startIndex
        while i < s.endIndex {
            let c = s[i]
            if c == "&", let semi = s[i...].firstIndex(of: ";") {
                let entity = String(s[s.index(after: i)..<semi])
                if let decoded = decodeEntity(entity) {
                    out.append(decoded)
                    i = s.index(after: semi)
                    continue
                }
            }
            out.append(c)
            i = s.index(after: i)
        }
        return out
    }

    private static func decodeEntity(_ e: String) -> Character? {
        switch e.lowercased() {
        case "amp": return "&"
        case "lt": return "<"
        case "gt": return ">"
        case "quot": return "\""
        case "apos", "#39": return "'"
        case "nbsp": return "\u{00A0}"
        default: break
        }
        if e.hasPrefix("#x") || e.hasPrefix("#X") {
            if let v = UInt32(e.dropFirst(2), radix: 16), let scalar = Unicode.Scalar(v) {
                return Character(scalar)
            }
        } else if e.hasPrefix("#") {
            if let v = UInt32(e.dropFirst()), let scalar = Unicode.Scalar(v) {
                return Character(scalar)
            }
        }
        return nil
    }
}

// Renders rich WE HTML with a WKWebView: transparent, follows system
// colors/fonts, images fit width and load async, links open in the browser.
// Self-scrolling is disabled; height is measured via JS and the wheel is
// forwarded to the enclosing ScrollView.
struct RichHTMLText: View {
    let html: String
    @Environment(\.mirageContentActive) private var isActive
    @State private var attributed: AttributedString?
    @State private var loadedHTML: String?

    var body: some View {
        Group {
            if isActive && WEHTML.needsWebView(html) {
                RichHTMLWebViewHost(html: html)
            } else {
                Text(loadedHTML == html ? (attributed ?? AttributedString(WEHTML.plain(html)))
                     : AttributedString(WEHTML.plain(html)))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
        }
        .task(id: isActive ? html : nil) {
            guard isActive, !WEHTML.needsWebView(html) else { return }
            let result = await WEHTML.attributed(html)
            guard !Task.isCancelled else { return }
            attributed = result
            loadedHTML = html
        }
    }
}

private struct RichHTMLWebViewHost: View {
    let html: String
    @State private var height: CGFloat = 24

    var body: some View {
        RichHTMLWebView(html: html, height: $height)
            .frame(height: height)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private final class PassThroughWebView: WKWebView {
    override func scrollWheel(with event: NSEvent) {
        nextResponder?.scrollWheel(with: event)
    }
}

private struct RichHTMLWebView: NSViewRepresentable {
    let html: String
    @Binding var height: CGFloat

    func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.userContentController.add(context.coordinator,
            contentWorld: .defaultClient, name: "mirageLabelSize")
        let webView = PassThroughWebView(frame: .zero, configuration: config)
        webView.navigationDelegate = context.coordinator
        webView.setValue(false, forKey: "drawsBackground")
        webView.enclosingScrollView?.hasVerticalScroller = false
        return webView
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        context.coordinator.parent = self
        guard context.coordinator.loadedHTML != html else { return }
        context.coordinator.loadedHTML = html
        let generation = UUID().uuidString
        context.coordinator.generation = generation
        context.coordinator.publish(height: 24, generation: generation)
        let controller = webView.configuration.userContentController
        controller.removeAllUserScripts()
        controller.addUserScript(WKUserScript(source: Self.measurementScript(generation: generation),
            injectionTime: .atDocumentEnd, forMainFrameOnly: true, in: .defaultClient))
        webView.loadHTMLString(Self.wrap(html), baseURL: nil)
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    static func dismantleNSView(_ webView: WKWebView, coordinator: Coordinator) {
        coordinator.generation = nil
        coordinator.publication?.cancel()
        coordinator.publication = nil
        webView.evaluateJavaScript("window.__mirageLabelCleanup?.()", in: nil,
                                  in: .defaultClient, completionHandler: nil)
        webView.stopLoading()
        webView.navigationDelegate = nil
        webView.configuration.userContentController.removeScriptMessageHandler(
            forName: "mirageLabelSize", contentWorld: .defaultClient)
        webView.configuration.userContentController.removeAllUserScripts()
    }

    private static func measurementScript(generation: String) -> String {
        """
        (() => {
            const content = document.getElementById('mirage-label-content');
            if (!content) return;
            let scheduled = false;
            let disposed = false;
            let previous = -1;
            const measure = () => {
                if (disposed || scheduled) return;
                scheduled = true;
                Promise.resolve().then(() => {
                    scheduled = false;
                    if (disposed) return;
                    const height = Math.max(1, Math.ceil(Math.max(
                        content.getBoundingClientRect().height, content.scrollHeight)));
                    if (height === previous) return;
                    previous = height;
                    window.webkit.messageHandlers.mirageLabelSize.postMessage({
                        generation: '\(generation)', height
                    });
                });
            };
            const resize = new ResizeObserver(measure);
            const mutation = new MutationObserver(measure);
            resize.observe(content);
            mutation.observe(content, {
                subtree: true, childList: true, attributes: true, characterData: true
            });
            document.addEventListener('load', measure, true);
            window.addEventListener('resize', measure);
            window.__mirageLabelCleanup = () => {
                disposed = true;
                resize.disconnect();
                mutation.disconnect();
                document.removeEventListener('load', measure, true);
                window.removeEventListener('resize', measure);
            };
            if (document.fonts) document.fonts.ready.then(measure);
            measure();
        })();
        """
    }

    private static func wrap(_ body: String) -> String {
        let normalized = body
            .replacingOccurrences(of: "＜", with: "<")
            .replacingOccurrences(of: "＞", with: ">")
        return """
        <!DOCTYPE html><html><head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>
        :root { color-scheme: light dark; }
        html, body { margin:0; padding:0; background:transparent; }
        #mirage-label-content { display: flow-root; width: 100%; }
        /* Property labels are display-only: retain link clicks while disabling
           WebKit's default text selection and drag sources. */
        html, body, body * {
            -webkit-user-select: none;
            user-select: none;
            -webkit-user-drag: none;
        }
        body {
            font: -apple-system-body, system-ui;
            font-size: 13px; line-height: 1.45;
            color: -apple-system-label;
            word-break: break-word; overflow-wrap: anywhere;
            overflow: hidden;
        }
        a { color: -apple-system-blue; text-decoration: none; }
        a:hover { text-decoration: underline; }
        img {
            max-width: 100%; height: auto; border-radius: 6px; display: block; margin: 4px 0;
            -webkit-user-drag: none;
        }
        big { font-size: 1.2em; }
        center { text-align: center; }
        p { margin: 4px 0; }
        table { max-width: 100%; }
        </style></head><body><div id="mirage-label-content">\(normalized)</div></body></html>
        """
    }

    final class Coordinator: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
        var parent: RichHTMLWebView
        var loadedHTML: String?
        var generation: String?
        var publication: DispatchWorkItem?
        init(_ parent: RichHTMLWebView) { self.parent = parent }

        func userContentController(_ userContentController: WKUserContentController,
                                   didReceive message: WKScriptMessage) {
            guard message.frameInfo.isMainFrame,
                  let payload = message.body as? [String: Any],
                  let generation = payload["generation"] as? String,
                  let height = payload["height"] as? Double,
                  height.isFinite, height > 0 else { return }
            publish(height: CGFloat(height), generation: generation)
        }

        func publish(height: CGFloat, generation: String) {
            guard self.generation == generation else { return }
            publication?.cancel()
            let work = DispatchWorkItem { [weak self] in
                guard let self, self.generation == generation else { return }
                self.publication = nil
                if abs(height - self.parent.height) > 0.5 { self.parent.height = height }
            }
            publication = work
            DispatchQueue.main.async(execute: work)
        }

        func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
                     decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            if navigationAction.navigationType == .linkActivated, let url = navigationAction.request.url {
                NSWorkspace.shared.open(url)
                decisionHandler(.cancel)
                return
            }
            decisionHandler(.allow)
        }
    }
}
