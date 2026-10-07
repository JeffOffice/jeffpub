"""Tests for the usage statistics collector: python3 -m unittest test_main"""
import base64
import json
import os
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

os.environ["DATA_DIR"] = tempfile.mkdtemp()
os.environ["STATS_PASSWORD"] = "secret"
import main  # noqa: E402  (reads the environment above)

INSTALL = "0f8fad5b-d9cb-469f-a165-70867728950e"


class Collector(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        main.init()
        cls.server = main.ThreadingHTTPServer(("127.0.0.1", 0), main.Handler)
        cls.url = "http://127.0.0.1:%d" % cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def post(self, body):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        req = urllib.request.Request(self.url + "/v1/ping", data=data, headers={"Content-Type": "application/json"})
        try:
            return urllib.request.urlopen(req).status
        except urllib.error.HTTPError as e:
            return e.code

    def get(self, path, password=None):
        req = urllib.request.Request(self.url + path)
        if password is not None:
            req.add_header("Authorization", "Basic " + base64.b64encode(b"jeff:" + password.encode()).decode())
        try:
            r = urllib.request.urlopen(req)
            return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, ""

    def ping(self, **over):
        p = {"install": INSTALL, "version": "0.1.37", "os": "windows", "osVersion": "10.0.22631", "lang": "en-US",
             "launches": 1, "counts": {"file.open.pub": 2, "export.pdf": 1}}
        p.update(over)
        return p

    def test_pings_add_up_by_day(self):
        self.assertEqual(self.post(self.ping()), 204)
        self.assertEqual(self.post(self.ping(counts={"file.open.pub": 3})), 204)
        with main.db() as con:
            launches = con.execute("SELECT launches FROM pings WHERE install = ?", (INSTALL,)).fetchone()[0]
            n = con.execute("SELECT SUM(n) FROM counts WHERE install = ? AND feature = 'file.open.pub'", (INSTALL,)).fetchone()[0]
        self.assertEqual(launches, 2)
        self.assertEqual(n, 5)

    def test_bad_pings_are_refused(self):
        self.assertEqual(self.post(self.ping(install="not-a-uuid")), 400)
        self.assertEqual(self.post(self.ping(counts={"C:\\Users\\jeff\\letter.pub": 1})), 400)
        self.assertEqual(self.post(self.ping(counts={"x": -1})), 400)
        self.assertEqual(self.post(self.ping(lang="<script>")), 400)
        self.assertEqual(self.post(b"{not json"), 400)
        self.assertEqual(self.post(b"x" * (main.MAX_BODY + 1)), 413)

    def test_stats_need_the_password(self):
        self.post(self.ping())
        self.assertEqual(self.get("/stats")[0], 401)
        self.assertEqual(self.get("/stats", "wrong")[0], 401)
        code, page = self.get("/stats", "secret")
        self.assertEqual(code, 200)
        self.assertIn("0.1.37", page)
        self.assertIn("export.pdf", page)

    def test_public_pages(self):
        self.assertEqual(self.get("/health"), (200, "ok"))
        code, page = self.get("/")
        self.assertEqual(code, 200)
        self.assertIn("Never your files", page)


if __name__ == "__main__":
    unittest.main()
