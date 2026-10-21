#!/usr/bin/env python3
"""One-time helper: obtain a Spotify refresh token for the Kallsup alarm clock.

Usage:
    python3 tools/get_refresh_token.py [CLIENT_ID]

The client ID can also come from $SPOTIFY_CLIENT_ID, the secret from
$SPOTIFY_CLIENT_SECRET. Anything missing is prompted for (the secret without echo).

The script starts a local server on http://127.0.0.1:8888/callback, opens the
Spotify authorize page in your browser, exchanges the returned code and prints
the refresh token. Nothing is written to disk. Python standard library only.
"""

import base64
import getpass
import http.server
import json
import os
import secrets
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

HOST = "127.0.0.1"
PORT = 8888
REDIRECT_URI = f"http://{HOST}:{PORT}/callback"
SCOPES = "user-read-playback-state user-modify-playback-state"
AUTHORIZE_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"


class CallbackHandler(http.server.BaseHTTPRequestHandler):
    expected_state = ""
    code = None
    error = None

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path != "/callback":
            self.send_error(404)
            return
        params = urllib.parse.parse_qs(url.query)
        state = params.get("state", [""])[0]
        if state != CallbackHandler.expected_state:
            CallbackHandler.error = "state mismatch (possible CSRF), try again"
        elif "error" in params:
            CallbackHandler.error = params["error"][0]
        else:
            CallbackHandler.code = params.get("code", [None])[0]

        ok = CallbackHandler.code is not None
        body = (
            "Done. You can close this tab and return to the terminal."
            if ok
            else f"Authorization failed: {CallbackHandler.error}"
        )
        self.send_response(200 if ok else 400)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.end_headers()
        self.wfile.write(body.encode())

    def log_message(self, *args):
        pass


def exchange_code(client_id, client_secret, code):
    basic = base64.b64encode(f"{client_id}:{client_secret}".encode()).decode()
    data = urllib.parse.urlencode(
        {"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT_URI}
    ).encode()
    req = urllib.request.Request(
        TOKEN_URL,
        data=data,
        headers={
            "Authorization": f"Basic {basic}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as e:
        sys.exit(f"Token exchange failed: HTTP {e.code}: {e.read().decode(errors='replace')}")


def main():
    client_id = (sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SPOTIFY_CLIENT_ID")) or input(
        "Spotify client ID: "
    ).strip()
    client_secret = os.environ.get("SPOTIFY_CLIENT_SECRET") or getpass.getpass("Spotify client secret: ").strip()
    if not client_id or not client_secret:
        sys.exit("Client ID and secret are required.")

    CallbackHandler.expected_state = secrets.token_urlsafe(16)
    query = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT_URI,
            "scope": SCOPES,
            "state": CallbackHandler.expected_state,
        }
    )
    auth_url = f"{AUTHORIZE_URL}?{query}"

    server = http.server.HTTPServer((HOST, PORT), CallbackHandler)
    print(f"Opening browser. If nothing happens, open this URL manually:\n\n  {auth_url}\n")
    webbrowser.open(auth_url)

    while CallbackHandler.code is None and CallbackHandler.error is None:
        server.handle_request()
    server.server_close()

    if CallbackHandler.error:
        sys.exit(f"Authorization failed: {CallbackHandler.error}")

    tokens = exchange_code(client_id, client_secret, CallbackHandler.code)
    refresh = tokens.get("refresh_token")
    if not refresh:
        sys.exit(f"No refresh_token in response: {tokens}")

    print("Refresh token (paste it into the Kallsup web UI, keep it secret):\n")
    print(refresh)


if __name__ == "__main__":
    main()
