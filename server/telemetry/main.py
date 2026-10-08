"""JeffPub's anonymous usage statistics: the collector and its stats page.

The app sends at most one ping a day (see src/app/telemetry.cpp), unless the
person using it turned this off:

    POST /v1/ping  {"install": "<random UUID>", "version": "0.1.37",
                    "os": "windows", "osVersion": "10.0.22631", "lang": "en-US",
                    "launches": 2, "counts": {"file.open.pub": 3, ...}}

Nothing else is kept: no IP address (none is logged either), no file names,
no text. The install id is made up by the app and says nothing about the
person or the computer.

    GET /          what's collected (public)
    GET /stats     the numbers (HTTP basic auth, password STATS_PASSWORD)
    GET /health    "ok"

Settings come from the environment: PORT, DATA_DIR (where the SQLite file
goes; a Railway volume), STATS_PASSWORD. Standard library only.
"""
import base64
import datetime as dt
import hmac
import html
import json
import os
import re
import sqlite3
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DATA_DIR = os.environ.get("DATA_DIR", os.path.dirname(os.path.abspath(__file__)))
DB_PATH = os.path.join(DATA_DIR, "telemetry.db")
PASSWORD = os.environ.get("STATS_PASSWORD", "")
MAX_BODY = 16 * 1024

UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
VERSION = re.compile(r"^[0-9A-Za-z.+-]{1,20}$")
OS = re.compile(r"^[a-z]{1,16}$")
OS_VERSION = re.compile(r"^[0-9A-Za-z .()_+-]{0,40}$")
LANG = re.compile(r"^[A-Za-z_-]{0,20}$")
FEATURE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,47}$")

_lock = threading.Lock()


def db():
    con = sqlite3.connect(DB_PATH, timeout=10)
    con.execute("PRAGMA journal_mode=WAL")
    return con


def init():
    os.makedirs(DATA_DIR, exist_ok=True)
    with db() as con:
        con.executescript(
            """
            CREATE TABLE IF NOT EXISTS pings (
                day TEXT NOT NULL, install TEXT NOT NULL, version TEXT, os TEXT,
                os_version TEXT, lang TEXT, launches INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (day, install));
            CREATE TABLE IF NOT EXISTS counts (
                day TEXT NOT NULL, install TEXT NOT NULL, feature TEXT NOT NULL,
                n INTEGER NOT NULL, PRIMARY KEY (day, install, feature));
            CREATE INDEX IF NOT EXISTS counts_day ON counts (day);
            """
        )


def parse_ping(body):
    """The ping's fields, checked; None when anything is off."""
    try:
        p = json.loads(body)
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(p, dict):
        return None
    install, version = p.get("install"), p.get("version")
    os_name, os_version, lang = p.get("os", ""), p.get("osVersion", ""), p.get("lang", "")
    launches, counts = p.get("launches", 0), p.get("counts", {})
    if not (isinstance(install, str) and UUID.match(install)):
        return None
    if not (isinstance(version, str) and VERSION.match(version)):
        return None
    if not (isinstance(os_name, str) and OS.match(os_name)):
        return None
    if not (isinstance(os_version, str) and OS_VERSION.match(os_version)):
        return None
    if not (isinstance(lang, str) and LANG.match(lang)):
        return None
    if not (isinstance(launches, int) and 0 <= launches <= 1000):
        return None
    if not (isinstance(counts, dict) and len(counts) <= 400):
        return None
    for k, v in counts.items():
        if not (FEATURE.match(k) and isinstance(v, int) and 0 <= v <= 100000):
            return None
    return install, version, os_name, os_version, lang, launches, counts


def record(ping):
    install, version, os_name, os_version, lang, launches, counts = ping
    day = dt.datetime.now(dt.timezone.utc).date().isoformat()
    with _lock, db() as con:
        con.execute(
            """INSERT INTO pings (day, install, version, os, os_version, lang, launches)
               VALUES (?, ?, ?, ?, ?, ?, ?)
               ON CONFLICT (day, install) DO UPDATE SET version = excluded.version,
                 os = excluded.os, os_version = excluded.os_version, lang = excluded.lang,
                 launches = launches + excluded.launches""",
            (day, install, version, os_name, os_version, lang, launches),
        )
        con.executemany(
            """INSERT INTO counts (day, install, feature, n) VALUES (?, ?, ?, ?)
               ON CONFLICT (day, install, feature) DO UPDATE SET n = n + excluded.n""",
            [(day, install, k, v) for k, v in counts.items() if v > 0],
        )


ABOUT = """<!doctype html><meta charset="utf-8"><title>JeffPub usage statistics</title>
<style>body{font:16px/1.5 system-ui,sans-serif;max-width:40em;margin:2em auto;padding:0 1em}</style>
<h1>JeffPub usage statistics</h1>
<p>Unless you turn it off (in setup, when JeffPub first starts, or under File &gt; Options),
JeffPub sends this once a day while it's in use, and once more the day it's updated:</p>
<ul><li>a random number made up when it was installed, which says nothing about you or your computer;</li>
<li>JeffPub's version, your operating system and its version, and the language it's set to;</li>
<li>how many times it was started, and how many times each of its commands was used
(for example "opened a .pub file" or "exported a PDF").</li></ul>
<p>Never your files, their names, or anything in them. Your IP address isn't kept.
The numbers show how many people use JeffPub and which parts matter most to them.</p>
<p>The program that collects them is part of JeffPub's source:
<a href="https://github.com/JeffOffice/jeffpub/tree/main/server/telemetry">server/telemetry</a>.</p>
"""


def stats_page():
    today = dt.datetime.now(dt.timezone.utc).date()
    d30 = (today - dt.timedelta(days=29)).isoformat()
    d60 = (today - dt.timedelta(days=59)).isoformat()
    d7 = (today - dt.timedelta(days=6)).isoformat()
    with db() as con:
        q = lambda sql, *a: con.execute(sql, a).fetchall()
        one = lambda sql, *a: con.execute(sql, a).fetchone()[0]
        totals = [
            ("Today", one("SELECT COUNT(*) FROM pings WHERE day = ?", today.isoformat())),
            ("Last 7 days", one("SELECT COUNT(DISTINCT install) FROM pings WHERE day >= ?", d7)),
            ("Last 30 days", one("SELECT COUNT(DISTINCT install) FROM pings WHERE day >= ?", d30)),
            ("Ever", one("SELECT COUNT(DISTINCT install) FROM pings")),
        ]
        daily = dict(q("SELECT day, COUNT(*) FROM pings WHERE day >= ? GROUP BY day", d60))
        firsts = dict(q("SELECT first, COUNT(*) FROM (SELECT install, MIN(day) AS first FROM pings GROUP BY install) "
                        "WHERE first >= ? GROUP BY first", d60))
        latest = "SELECT p.{0}, COUNT(*) FROM pings p JOIN (SELECT install, MAX(day) AS d FROM pings WHERE day >= ? " \
                 "GROUP BY install) l ON p.install = l.install AND p.day = l.d GROUP BY p.{0} ORDER BY 2 DESC"
        versions = q(latest.format("version"), d30)
        systems = q(latest.format("os"), d30)
        os_versions = q("SELECT p.os || ' ' || p.os_version, COUNT(*) FROM pings p JOIN (SELECT install, MAX(day) AS d FROM pings "
                        "WHERE day >= ? GROUP BY install) l ON p.install = l.install AND p.day = l.d GROUP BY 1 ORDER BY 2 DESC LIMIT 15", d30)
        langs = q(latest.format("lang"), d30)
        features = q("SELECT feature, SUM(n), COUNT(DISTINCT install) FROM counts WHERE day >= ? GROUP BY feature "
                     "ORDER BY 3 DESC, 2 DESC LIMIT 100", d30)
    e = html.escape

    def table(title, head, rows):
        body = "".join("<tr>" + "".join(f"<td>{e(str(c))}</td>" for c in r) + "</tr>" for r in rows)
        return f"<h2>{e(title)}</h2><table><tr>{''.join(f'<th>{e(h)}</th>' for h in head)}</tr>{body or '<tr><td>none yet</td></tr>'}</table>"

    days = [(today - dt.timedelta(days=i)).isoformat() for i in range(59, -1, -1)]
    peak = max([1] + list(daily.values()))
    w = 10
    bars = "".join(
        f'<rect x="{i * w}" y="{120 - 110 * daily.get(d, 0) / peak:.1f}" width="{w - 2}" height="{110 * daily.get(d, 0) / peak:.1f}" fill="#1F5FAD">'
        f"<title>{d}: {daily.get(d, 0)} active, {firsts.get(d, 0)} new</title></rect>"
        for i, d in enumerate(days))
    chart = f'<svg viewBox="0 0 {w * 60} 125" width="100%" role="img" aria-label="Active installs per day">{bars}</svg>'
    return f"""<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>JeffPub usage</title>
<style>body{{font:15px/1.45 system-ui,sans-serif;max-width:56em;margin:1.5em auto;padding:0 1em;color:#1E2430}}
table{{border-collapse:collapse;margin-bottom:1em}}td,th{{padding:3px 10px;border-bottom:1px solid #dde;text-align:left}}
td:nth-child(n+2){{text-align:right}}.cards{{display:flex;gap:1em;flex-wrap:wrap}}
.card{{background:#F2F5F9;border-radius:8px;padding:.6em 1em}}.card b{{display:block;font-size:1.6em}}</style>
<h1>JeffPub usage</h1>
<p>Installs that sent statistics (each counted once per period). Times are UTC.</p>
<div class="cards">{''.join(f'<div class="card"><b>{n}</b>{e(label)}</div>' for label, n in totals)}</div>
<h2>Active installs, last 60 days</h2>{chart}
{table("Version (last 30 days)", ["Version", "Installs"], versions)}
{table("Operating system (last 30 days)", ["System", "Installs"], systems)}
{table("System versions (last 30 days)", ["System", "Installs"], os_versions)}
{table("Language (last 30 days)", ["Language", "Installs"], langs)}
{table("Commands used (last 30 days)", ["Command", "Times", "Installs"], features)}
"""


class Handler(BaseHTTPRequestHandler):
    server_version = "jeffpub-telemetry"
    sys_version = ""

    def log_message(self, fmt, *args):
        # No client address: only the request line and status.
        print(f"{self.command} {self.path.split('?')[0]} {args[1] if len(args) > 1 else ''}", flush=True)

    def send(self, code, body=b"", ctype="text/plain; charset=utf-8", extra=()):
        self.send_response(code)
        for k, v in extra:
            self.send_header(k, v)
        if body:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def authorized(self):
        if not PASSWORD:
            return False
        h = self.headers.get("Authorization", "")
        if not h.startswith("Basic "):
            return False
        try:
            user_pass = base64.b64decode(h[6:]).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            return False
        return hmac.compare_digest(user_pass.partition(":")[2].encode(), PASSWORD.encode())

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/health":
            self.send(200, b"ok")
        elif path == "/":
            self.send(200, ABOUT.encode(), "text/html; charset=utf-8")
        elif path == "/stats":
            if not self.authorized():
                self.send(401, b"Password needed", extra=[("WWW-Authenticate", 'Basic realm="JeffPub usage"')])
                return
            self.send(200, stats_page().encode(), "text/html; charset=utf-8", extra=[("Cache-Control", "no-store")])
        else:
            self.send(404, b"Not found")

    do_HEAD = do_GET

    def do_POST(self):
        if self.path.split("?")[0] != "/v1/ping":
            self.send(404, b"Not found")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length <= 0 or length > MAX_BODY:
            self.send(413 if length > MAX_BODY else 400, b"Bad request")
            return
        ping = parse_ping(self.rfile.read(length))
        if ping is None:
            self.send(400, b"Bad request")
            return
        record(ping)
        self.send(204)


def main():
    init()
    port = int(os.environ.get("PORT", "8080"))
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    server.daemon_threads = True
    print(f"listening on {port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
