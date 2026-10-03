#!/usr/bin/env python3
"""Sign a release older than the checkout and verify its manifest's exact wire text."""
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class SigningTest(unittest.TestCase):
    def test_explicit_manifest_version_is_signed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release = root / "release" / "0.0.1"
            release.mkdir(parents=True)
            binary = release / "esp32c3-app.bin"
            binary.write_bytes(b"test image")
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            manifest = release / "manifest.json"
            manifest.write_text(json.dumps({"version": "0.0.1", "images": {"esp32c3": {"ota": {
                "file": binary.name, "sha256": digest, "size": binary.stat().st_size
            }}}}))
            key = root / "test.pem"
            pub = root / "test.pub.pem"
            subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key)], check=True)
            subprocess.run(["openssl", "ec", "-in", str(key), "-pubout", "-out", str(pub)], check=True, capture_output=True)
            subprocess.run(["bash", str(ROOT / "tools/sign-release.sh"), "--key", str(key), "--key-id", "test", "--manifest", str(manifest)], check=True, capture_output=True)
            signed = json.loads(manifest.read_text())["images"]["esp32c3"]["ota"]
            sig = root / "sig.der"
            sig.write_bytes(base64.b64decode(signed["sig"]))
            text = root / "signed.txt"
            text.write_text(f"hydronode-ota-v1|0.0.1|esp32c3|{digest}|{binary.stat().st_size}|0")
            subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(pub), "-signature", str(sig), str(text)], check=True, capture_output=True)
            self.assertEqual(signed["keyId"], "test")
            self.assertTrue((root / "hydronode-firmware-0.0.1.zip").exists())

if __name__ == "__main__":
    unittest.main()
