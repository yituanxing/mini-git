"""临时本地 Git Smart HTTP 服务器（git http-backend CGI 封装）

用于在没有外网 Git 服务器时验证 mgit clone 的协议实现。
启动后把 GIT_PROJECT_ROOT 下的仓库以 http://host:port/<repo> 提供。

用法: python git_http_server.py <repos_root> [port]
"""
import os
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

GIT = sys.argv[3] if len(sys.argv) > 3 else "git"
ROOT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8000


class Handler(BaseHTTPRequestHandler):
    def _handle(self):
        path = self.path.split("?", 1)[0].lstrip("/")
        query = self.path.split("?", 1)[1] if "?" in self.path else ""

        env = dict(os.environ)
        env["GIT_PROJECT_ROOT"] = ROOT
        env["GIT_HTTP_EXPORT_ALL"] = "1"
        env["PATH_INFO"] = "/" + path
        env["QUERY_STRING"] = query
        env["REQUEST_METHOD"] = self.command
        env["GATEWAY_INTERFACE"] = "CGI/1.1"
        ct = self.headers.get("Content-Type")
        if ct:
            env["CONTENT_TYPE"] = ct
        cl = self.headers.get("Content-Length")
        body = b""
        if cl:
            env["CONTENT_LENGTH"] = cl
            body = self.rfile.read(int(cl))
        elif self.command == "POST":
            te = self.headers.get("Transfer-Encoding", "")
            if "chunked" in te.lower():
                # 读 chunked 体
                data = b""
                while True:
                    line = self.rfile.readline().strip()
                    size = int(line, 16)
                    if size == 0:
                        self.rfile.readline()
                        break
                    data += self.rfile.read(size)
                    self.rfile.readline()
                body = data
                env["CONTENT_LENGTH"] = str(len(body))

        proc = subprocess.run(
            [GIT, "http-backend"],
            input=body, env=env, cwd=ROOT, capture_output=True,
        )
        out = proc.stdout

        # 解析 CGI 输出：头部 + 空行 + 内容
        sep = out.find(b"\r\n\r\n")
        sep_len = 4
        if sep < 0:
            sep = out.find(b"\n\n")
            sep_len = 2
        headers_raw = out[:sep].decode("latin1") if sep >= 0 else ""
        content = out[sep + sep_len:] if sep >= 0 else out

        status = 200
        headers = []
        for line in headers_raw.splitlines():
            if line.lower().startswith("status:"):
                status = int(line.split()[1])
            elif ":" in line:
                k, v = line.split(":", 1)
                headers.append((k.strip(), v.strip()))

        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def do_GET(self):
        self._handle()

    def do_POST(self):
        self._handle()

    def log_message(self, fmt, *args):
        sys.stderr.write("[server] " + fmt % args + "\n")


if __name__ == "__main__":
    print("serving %s on http://127.0.0.1:%d" % (ROOT, PORT))
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
