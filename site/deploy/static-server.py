#!/usr/bin/env python3
"""Celadon 站点的静态服务（只用 Python 标准库；服务器上没有 nginx、没有 sudo）。

- SPA：只有「看起来像页面路由」的路径（最后一段没有扩展名，如 /docs/install）不存在时才回落到 index.html；
  /zh 与 /zh/… 回落到 index.zh.html（中文标题与分享卡片，构建时由 scripts/gen-zh-html.mjs 生成；ADR-024），没有这份时退回 index.html。
  带扩展名的文件缺了一律 404，/assets/ 下的更是如此——发版后旧标签页再请求已删掉的旧 JS，
  拿到的是 404，而不是一页 HTML（那样浏览器会报 MIME 类型错误）。
- 下载：给了 --downloads DIR 时，/downloads/<文件名> 从 DIR 提供安装包：附件下载（Content-Disposition）、
  正确的 Content-Type / Content-Length、Range 断点续传（单段）、HEAD、ETag / Last-Modified、If-Range 与条件请求。
  文件名只认一段、只认字母数字与 ._+-，再核对真实路径仍在 DIR 里，防目录穿越；缺失 404。/downloads 本身跳到 /download 页。
- 缓存：/assets/ 下的文件名带内容哈希 → 一年 immutable；/downloads/ → 一小时；其它（index.html 等）与跳转 no-cache；
  4xx / 5xx 一律 no-store（别让 CDN 把 404 当成文件缓存起来）；--no-cache 全部 no-store（验收用）。
- HTTP/1.1 keep-alive；线程池。生产绑 127.0.0.1，只经 Cloudflare Tunnel 出去。

用法：static-server.py --root DIST [--downloads DIR] [--bind 127.0.0.1] [--port 8090] [--no-cache]
"""
import argparse
import email.utils
import http.server
import os
import posixpath
import re
import sys
import urllib.parse

parser = argparse.ArgumentParser()
parser.add_argument('--root', required=True)
parser.add_argument('--downloads', default=None, help='安装包目录：/downloads/<文件名> 从这里提供')
parser.add_argument('--bind', default='127.0.0.1')
parser.add_argument('--port', type=int, default=8090)
parser.add_argument('--no-cache', action='store_true')
args = parser.parse_args()
ROOT = os.path.abspath(args.root)
DOWNLOADS = os.path.realpath(args.downloads) if args.downloads else None

DOWNLOAD_PREFIX = '/downloads/'
# 只认一段、字母数字开头的文件名：没有 /、\、..、隐藏文件、空格与非 ASCII，Content-Disposition 也就不用转义。
SAFE_NAME = re.compile(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,199}')
RANGE = re.compile(r'bytes=([0-9]*)-([0-9]*)', re.IGNORECASE)
DOWNLOAD_TYPES = {
    '.exe': 'application/vnd.microsoft.portable-executable',
    '.msi': 'application/x-msi',
    '.pkg': 'application/octet-stream',
    '.dmg': 'application/x-apple-diskimage',
    '.zip': 'application/zip',
    '.txt': 'text/plain; charset=utf-8',
}
# 没有扩展名的纯文本（校验和清单）
TEXT_NAMES = {'SHA256SUMS'}


ZH_ROUTE = re.compile(r'/zh(/|$)')


def fallback_page(clean):
    """页面路由回落到哪份 HTML：中文路由用 index.zh.html（存在时），其余 index.html。"""
    if ZH_ROUTE.match(clean) and os.path.isfile(os.path.join(ROOT, 'index.zh.html')):
        return '/index.zh.html'
    return '/index.html'


def looks_like_route(clean):
    """页面路由（可回落到 index.html）：不在 /assets/ 下，且最后一段没有扩展名。"""
    if clean == '/assets' or clean.startswith('/assets/'):
        return False
    last = posixpath.basename(urllib.parse.unquote(clean))
    return '.' not in last


def parse_range(header, size):
    """Range 头 → (起, 止) 闭区间；None = 忽略 Range、整份发送；'unsatisfiable' = 416。

    只支持单段（断点续传只用单段）；多段或写法不对按 RFC 9110 忽略 Range。
    """
    m = RANGE.fullmatch(header.strip())
    if not m:
        return None
    first, last = m.groups()
    if not first and not last:
        return None
    if not first:  # 后缀：最后 N 个字节
        n = int(last)
        if n == 0 or size == 0:
            return 'unsatisfiable'
        return (max(0, size - n), size - 1)
    start = int(first)
    if last and int(last) < start:
        return None
    if start >= size:
        return 'unsatisfiable'
    end = min(int(last), size - 1) if last else size - 1
    return (start, end)


def http_date_to_ts(value):
    try:
        dt = email.utils.parsedate_to_datetime(value)
    except (TypeError, ValueError, IndexError):
        return None
    if dt is None:
        return None
    return int(dt.timestamp())


class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'celadon-site'
    sys_version = ''

    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    # ---- 路由 ----

    def do_GET(self):
        if not self.route_special(head=False):
            super().do_GET()

    def do_HEAD(self):
        if not self.route_special(head=True):
            super().do_HEAD()

    def route_special(self, head):
        """/downloads 下的请求在这里处理完并返回 True；其余交给父类（静态文件 + SPA 回落）。"""
        clean = urllib.parse.urlsplit(self.path).path
        if clean in ('/downloads', DOWNLOAD_PREFIX):
            self.send_response(302)
            self.send_header('Location', '/download')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return True
        if clean.startswith(DOWNLOAD_PREFIX):
            self.serve_download(clean, head)
            return True
        return False

    def send_head(self):
        # 把查询串去掉再判断文件是否存在；目录没有 index.html、或页面路由不存在 → SPA 回落；带扩展名的文件缺了 → 404
        clean = urllib.parse.urlsplit(self.path).path
        path = self.translate_path(clean)
        if os.path.isdir(path):
            if not looks_like_route(clean):
                self.send_error(404, 'Not Found')
                return None
            if not os.path.exists(os.path.join(path, 'index.html')):
                self.path = fallback_page(clean)
        elif not os.path.isfile(path):
            if not looks_like_route(clean):
                self.send_error(404, 'Not Found')
                return None
            self.path = fallback_page(clean)
        else:
            self.path = clean
        return super().send_head()

    # ---- 下载 ----

    def serve_download(self, clean, head):
        name = urllib.parse.unquote(clean[len(DOWNLOAD_PREFIX):])
        full = self.download_path(name)
        if full is None:
            self.send_error(404, 'Not Found')
            return
        try:
            f = open(full, 'rb')
        except OSError:
            self.send_error(404, 'Not Found')
            return
        with f:
            st = os.fstat(f.fileno())
            size = st.st_size
            etag = '"%x-%x"' % (size, st.st_mtime_ns)
            mtime = int(st.st_mtime)
            last_modified = email.utils.formatdate(st.st_mtime, usegmt=True)

            if self.not_modified(etag, mtime):
                self.send_response(304)
                self.send_header('ETag', etag)
                self.send_header('Last-Modified', last_modified)
                self.end_headers()
                return

            rng = None
            header = self.headers.get('Range')
            if header and self.if_range_ok(etag, mtime):
                rng = parse_range(header, size)
            if rng == 'unsatisfiable':
                self.send_response(416)
                self.send_header('Content-Range', 'bytes */%d' % size)
                self.send_header('Accept-Ranges', 'bytes')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return

            start, end = rng if rng else (0, size - 1)
            length = end - start + 1 if size else 0
            self.send_response(206 if rng else 200)
            if rng:
                self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
            self.send_header('Content-Type', self.download_type(name))
            self.send_header('Content-Length', str(length))
            self.send_header('Content-Disposition', 'attachment; filename="%s"' % name)
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('ETag', etag)
            self.send_header('Last-Modified', last_modified)
            self.end_headers()
            if head or length == 0:
                return
            try:
                self.connection.sendfile(f, start, length)
            except (BrokenPipeError, ConnectionResetError):
                # 用户取消下载或断网：连接作废，不打印堆栈
                self.close_connection = True

    def download_path(self, name):
        """合规且确实在下载目录里的普通文件 → 绝对路径；否则 None（一律按 404 处理，不区分原因）。"""
        if DOWNLOADS is None or not SAFE_NAME.fullmatch(name):
            return None
        full = os.path.realpath(os.path.join(DOWNLOADS, name))
        if os.path.dirname(full) != DOWNLOADS or not os.path.isfile(full):
            return None
        return full

    @staticmethod
    def download_type(name):
        if name in TEXT_NAMES:
            return 'text/plain; charset=utf-8'
        return DOWNLOAD_TYPES.get(posixpath.splitext(name)[1].lower(), 'application/octet-stream')

    def not_modified(self, etag, mtime):
        """条件请求：If-None-Match 优先（弱比较）；没有它才看 If-Modified-Since。带 Range 的请求不回 304。"""
        if self.headers.get('Range'):
            return False
        inm = self.headers.get('If-None-Match')
        if inm is not None:
            tags = [t.strip() for t in inm.split(',')]
            return '*' in tags or any(t.removeprefix('W/') == etag for t in tags)
        ims = self.headers.get('If-Modified-Since')
        if ims:
            ts = http_date_to_ts(ims)
            return ts is not None and mtime <= ts
        return False

    def if_range_ok(self, etag, mtime):
        """If-Range：续传前确认文件没换过；换过就忽略 Range 整份重发。ETag 用强比较，日期要与 Last-Modified 完全相同。"""
        value = self.headers.get('If-Range')
        if value is None:
            return True
        value = value.strip()
        if value.startswith('"') or value.startswith('W/'):
            return value == etag
        return http_date_to_ts(value) == mtime

    # ---- 响应头与日志 ----

    def send_response(self, code, message=None):
        self._status = code
        super().send_response(code, message)

    def end_headers(self):
        clean = urllib.parse.urlsplit(self.path).path
        status = getattr(self, '_status', 200)
        if args.no_cache or status >= 400:
            self.send_header('Cache-Control', 'no-store')
        elif 300 <= status < 400 and status != 304:
            # 跳转（/downloads → /download、目录补斜杠）不进缓存；304 沿用资源本身的策略
            self.send_header('Cache-Control', 'no-cache')
        elif clean.startswith('/assets/'):
            self.send_header('Cache-Control', 'public, max-age=31536000, immutable')
        elif clean.startswith(DOWNLOAD_PREFIX):
            self.send_header('Cache-Control', 'public, max-age=3600')
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

if DOWNLOADS is not None and not os.path.isdir(DOWNLOADS):
    # 目录还没建（第一次部署前）也照常起服务：站点照常，/downloads/ 下一律 404
    print(f'celadon-site: 下载目录 {DOWNLOADS} 不存在，/downloads/ 暂时全部 404', file=sys.stderr, flush=True)

server = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
server.daemon_threads = True
print(f'celadon-site serving {ROOT} on http://{args.bind}:{args.port}' + (f'，下载目录 {DOWNLOADS}' if DOWNLOADS else ''), flush=True)
server.serve_forever()
