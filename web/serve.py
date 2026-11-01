"""Serves the built demo page (build/web/site) on localhost: wasm and the worklet can't load from file://.

Usage: python web/serve.py <site-dir> [port]
"""

import functools
import http.server
import sys

site = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080


class Handler(http.server.SimpleHTTPRequestHandler):
    # Windows' registry can map .js to text/plain, which browsers refuse for modules.
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".js": "text/javascript",
        ".wasm": "application/wasm",
    }

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")  # a rebuild shows on the next reload
        super().end_headers()


server = http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(Handler, directory=site))
print(f"Serving {site} on http://localhost:{port}/ (Ctrl+C stops it)")
try:
    server.serve_forever()
except KeyboardInterrupt:
    pass
