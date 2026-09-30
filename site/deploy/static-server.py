#!/usr/bin/env python3
"""Celadon 站点的静态服务（只用 Python 标准库；服务器上没有 nginx、没有 sudo）。

- SPA：路径不存在就回落到 index.html（/docs/xxx、/algorithms/xxx 深链接）。
- 缓存：/assets/ 下的文件名带内容哈希 → 一年 immutable；其它（index.html、_redirects）不缓存；--no-cache 全部 no-store（验收用）。
- HTTP/1.1 keep-alive；线程池。生产绑 127.0.0.1，只经 Cloudflare Tunnel 出去。

用法：static-server.py --root DIST [--bind 127.0.0.1] [--port 8090] [--no-cache]
"""
import argparse
import http.server
import os
import posixpath
import urllib.parse

parser = argparse.ArgumentParser()
parser.add_argument('--root', required=True)
parser.add_argument('--bind', default='127.0.0.1')
parser.add_argument('--port', type=int, default=8090)
parser.add_argument('--no-cache', action='store_true')
args = parser.parse_args()
ROOT = os.path.abspath(args.root)


class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'celadon-site'
    sys_version = ''

    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def send_head(self):
        # 把查询串去掉再判断文件是否存在；目录没有 index.html 或文件不存在 → SPA 回落
        clean = urllib.parse.urlsplit(self.path).path
        path = self.translate_path(clean)
        if os.path.isdir(path):
            if not os.path.exists(os.path.join(path, 'index.html')):
                self.path = '/index.html'
        elif not os.path.exists(path):
            self.path = '/index.html'
        else:
            self.path = clean
        return super().send_head()

    def end_headers(self):
        clean = urllib.parse.urlsplit(self.path).path
        if args.no_cache:
            self.send_header('Cache-Control', 'no-store')
        elif clean.startswith('/assets/'):
            self.send_header('Cache-Control', 'public, max-age=31536000, immutable')
        else:
            self.send_header('Cache-Control', 'no-cache')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'strict-origin-when-cross-origin')
        super().end_headers()

    def log_message(self, fmt, *a):
        # 只记非 2xx/3xx，别把日志刷满
        try:
            code = int(a[1])
        except (IndexError, ValueError):
            code = 0
        if code >= 400:
            super().log_message(fmt, *a)


# 让 .woff2 / .svg 等有正确的 Content-Type（旧 mimetypes 表可能缺）
Handler.extensions_map.update({'.woff2': 'font/woff2', '.woff': 'font/woff', '.svg': 'image/svg+xml', '.webmanifest': 'application/manifest+json'})

server = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
server.daemon_threads = True
print(f'celadon-site serving {ROOT} on http://{args.bind}:{args.port}', flush=True)
server.serve_forever()
