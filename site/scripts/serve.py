#!/usr/bin/env python3
"""验收用的最小静态服务：SPA 深链接回落到 index.html。用法：serve.py <端口> <dist 目录>。只在测试服务器上跑。"""
import http.server
import os
import sys

PORT = int(sys.argv[1])
ROOT = os.path.abspath(sys.argv[2])


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def send_head(self):
        path = self.translate_path(self.path)
        if not os.path.exists(path) or (os.path.isdir(path) and not os.path.exists(os.path.join(path, 'index.html'))):
            self.path = '/index.html'
        return super().send_head()

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def log_message(self, *a):
        pass


http.server.ThreadingHTTPServer(('0.0.0.0', PORT), Handler).serve_forever()
