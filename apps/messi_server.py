# a minimal HTTP server that reads PORT from environment

import os
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

PORT = int(os.environ.get('PORT', 5001))

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = f"""<html>
<body>
  <h1>Messi is the goat</h1>
  <p>Path requested: {self.path}</p>
  <p>Running on port: {PORT}</p>
  <p>Time: {datetime.now()}</p>
  <p>PID: {os.getpid()}</p>
</body>
</html>""".encode()

        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass

print(f"[messi_server.py] starting on port {PORT}", flush=True)
httpserver = HTTPServer(('localhost', PORT), Handler)
httpserver.serve_forever()