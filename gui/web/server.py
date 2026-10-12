#!/usr/bin/env python3
import http.server, json, subprocess, urllib.parse, os, sys, pathlib, shlex, tempfile, mimetypes

HAZF_BIN = os.environ.get("HAZF_BIN", "")
# Try to locate hazf binary
candidates = [
    HAZF_BIN,
    pathlib.Path(__file__).parent.parent.parent / "build" / "hazf",
    pathlib.Path("/home/user/HAZF/build/hazf"),
    pathlib.Path("/usr/local/bin/hazf"),
]
HAZF = None
for c in candidates:
    if c and pathlib.Path(c).exists():
        HAZF = str(c)
        break
if not HAZF:
    # fallback to hazf in PATH
    HAZF = "hazf"

WEB_ROOT = pathlib.Path(__file__).parent

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(WEB_ROOT), **kw)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/list":
            qs = urllib.parse.parse_qs(parsed.query)
            arc = qs.get("archive", [""])[0]
            if not arc or not os.path.exists(arc):
                self.send_error(404, "Archive not found")
                return
            try:
                out = subprocess.check_output([HAZF, "list", arc], text=True, stderr=subprocess.STDOUT)
                # Parse entries from list output: lines like " 1. path  123 bytes (compressed ... CRC32 0x...)"
                entries = []
                archive_size = os.path.getsize(arc)
                for line in out.splitlines():
                    line=line.strip()
                    if line.startswith(tuple(str(i)+"." for i in range(1,100))):
                        # Example: 1. README.md  1024 bytes (compressed 512 bytes, CRC32 0x12345678)
                        try:
                            # extract path and numbers
                            # format: "1. path  1024 bytes (compressed 512 bytes, CRC32 0x...)"
                            parts = line.split()
                            # find path: after "1." until number
                            # Simplify: use parsing
                            # path is second token
                            path = parts[1]
                            orig = int(parts[2])
                            # compressed is parts[5]
                            comp = int(parts[5])
                            crc_str = parts[8].strip('),')
                            crc = int(crc_str, 16)
                            entries.append([path, orig, comp, crc])
                        except: pass
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"entries": entries, "archiveSize": archive_size, "raw": out}).encode())
                return
            except subprocess.CalledProcessError as e:
                self.send_error(500, e.output)
                return
        elif parsed.path == "/api/test":
            qs = urllib.parse.parse_qs(parsed.query)
            arc = qs.get("archive", [""])[0]
            if not arc:
                self.send_error(400, "missing archive")
                return
            try:
                out = subprocess.check_output([HAZF, "test", arc], text=True, stderr=subprocess.STDOUT)
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.end_headers()
                self.wfile.write(out.encode())
                return
            except subprocess.CalledProcessError as e:
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write((e.output or "Failed").encode())
                return
        elif parsed.path == "/api/info":
            qs = urllib.parse.parse_qs(parsed.query)
            arc = qs.get("archive", [""])[0]
            try:
                out = subprocess.check_output([HAZF, "info", arc], text=True, stderr=subprocess.STDOUT)
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(out.encode())
                return
            except Exception as e:
                self.send_error(500, str(e))
                return
        else:
            return super().do_GET()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length) if length else b""
        try:
            data = json.loads(body.decode()) if body else {}
        except:
            data = {}
        if parsed.path == "/api/create":
            archive = data.get("archive", "/tmp/web_archive.hazf")
            files = data.get("files", [])
            # files are just names from browser, not real paths; mock
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"message": f"Archive {archive} would be created with {len(files)} files (mock, use native GUI for real files)"}).encode())
            return
        elif parsed.path == "/api/extract":
            archive = data.get("archive","")
            dest = data.get("dest","/tmp")
            if not archive or not os.path.exists(archive):
                self.send_error(404, "Archive not found")
                return
            try:
                out = subprocess.check_output([HAZF, "extract", archive, dest], text=True, stderr=subprocess.STDOUT)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"message": out.strip()}).encode())
                return
            except subprocess.CalledProcessError as e:
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": e.output}).encode())
                return
        else:
            self.send_error(404, "Not found")

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - - [%s] %s\n" % (self.client_address[0], self.log_date_time_string(), fmt%args))

def run():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--host", default="0.0.0.0")
    args = p.parse_args()
    port = int(os.environ.get("PORT", args.port))
    host = os.environ.get("HOST", args.host)
    print(f"HAZF GUI web preview serving at http://{host}:{port}  (HAZF_BIN={HAZF}, root={WEB_ROOT})")
    server = http.server.ThreadingHTTPServer((host, port), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    run()
