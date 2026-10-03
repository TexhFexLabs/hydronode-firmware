#!/usr/bin/env bash
# Signs the OTA images of a packaged release (tools/package-release.py) and writes the
# signatures into its manifest:
#   images.<family>.ota = {file, sha256, size, sig, keyId, downgrade}
# Signed text per image (one line, UTF-8, no newline):
#   hydronode-ota-v1|<version>|<family>|<sha256 hex>|<size>|<downgrade 0|1>
# ECDSA P-256 over SHA-256, DER, Base64. Afterwards SHA256SUMS and the release zip are rebuilt.
#
#   tools/sign-release.sh --key tools/dev-keys/ota-dev.pem            # local dev key
#   tools/sign-release.sh --key yubikey --pub prod.pub.pem [--slot 9c] [--key-id prod-2026-10]
#   options: --manifest <path>  (default dist/release/<version>/manifest.json)
#            --downgrade        (allow devices on a newer version to take this one)
#
# With --key yubikey the private key never leaves the YubiKey: yubico-piv-tool signs on the
# device and asks for the PIN. --pub is the matching public key; every signature is verified
# with it before the manifest is written.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
key="" pub="" slot="9c" key_id="" manifest="" downgrade=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --key) key="$2"; shift 2 ;;
    --pub) pub="$2"; shift 2 ;;
    --slot) slot="$2"; shift 2 ;;
    --key-id) key_id="$2"; shift 2 ;;
    --manifest) manifest="$2"; shift 2 ;;
    --downgrade) downgrade=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[[ -n "$key" ]] || { echo "--key <pem>|yubikey is required" >&2; exit 2; }

version="$(sed -n 's/^version = //p' "$root/platformio.ini")"
manifest="${manifest:-$root/dist/release/$version/manifest.json}"
[[ -f "$manifest" ]] || { echo "no manifest at $manifest, run tools/package-release.py first" >&2; exit 1; }
# The chosen release may differ from the current checkout. Sign its version.
version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$manifest")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [[ "$key" == "yubikey" ]]; then
  command -v yubico-piv-tool >/dev/null || { echo "yubico-piv-tool not found" >&2; exit 1; }
  [[ -n "$pub" ]] || { echo "--pub <public key pem> is required with --key yubikey" >&2; exit 1; }
  [[ -n "$key_id" ]] || { echo "--key-id is required with --key yubikey" >&2; exit 1; }
else
  [[ -f "$key" ]] || { echo "key $key not found" >&2; exit 1; }
  pub="$work/pub.pem"
  openssl ec -in "$key" -pubout -out "$pub" 2>/dev/null
  if [[ -z "$key_id" ]]; then
    if [[ -f "$(dirname "$key")/ota-dev.keyid" ]]; then key_id="$(cat "$(dirname "$key")/ota-dev.keyid")"
    else echo "--key-id is required" >&2; exit 1; fi
  fi
fi

sign() {  # $1 text file, $2 signature output (DER)
  if [[ "$key" == "yubikey" ]]; then
    yubico-piv-tool -a verify-pin --sign -s "$slot" -A ECCP256 -H SHA256 -i "$1" -o "$2"
  else
    openssl dgst -sha256 -sign "$key" -out "$2" "$1"
  fi
  openssl dgst -sha256 -verify "$pub" -signature "$2" "$1" >/dev/null || { echo "signature check failed" >&2; exit 1; }
}

dir="$(dirname "$manifest")"
families="$(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(" ".join(f for f,i in m["images"].items() if "ota" in i))' "$manifest")"
[[ -n "$families" ]] || { echo "no OTA images in $manifest" >&2; exit 1; }

for family in $families; do
  read -r file size sha < <(python3 -c 'import json,sys; o=json.load(open(sys.argv[1]))["images"][sys.argv[2]]["ota"]; print(o["file"], o["size"], o["sha256"])' "$manifest" "$family")
  actual="$(shasum -a 256 "$dir/$file" | cut -d' ' -f1)"
  [[ "$actual" == "$sha" ]] || { echo "$file: sha256 does not match the manifest" >&2; exit 1; }
  printf 'hydronode-ota-v1|%s|%s|%s|%s|%s' "$version" "$family" "$sha" "$size" "$downgrade" > "$work/$family.txt"
  sign "$work/$family.txt" "$work/$family.sig"
  base64 < "$work/$family.sig" | tr -d '\n' > "$work/$family.b64"
done

python3 - "$manifest" "$work" "$key_id" "$downgrade" $families <<'PY'
import hashlib, json, sys, zipfile
from pathlib import Path
manifest, work, key_id, downgrade, *families = sys.argv[1:]
path = Path(manifest)
m = json.loads(path.read_text())
for f in families:
    ota = m["images"][f]["ota"]
    ota["sig"] = (Path(work) / f"{f}.b64").read_text().strip()
    ota["keyId"] = key_id
    ota["downgrade"] = downgrade == "1"
    print(f"{f:10} {ota['file']:22} {ota['size']:>8} bytes  signed with {key_id}{'  (downgrade)' if downgrade == '1' else ''}")
path.write_text(json.dumps(m, indent=2) + "\n")
out = path.parent
sums = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}" for p in sorted(out.iterdir()) if p.name != "SHA256SUMS"]
(out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
archive = out.parent.parent / f"hydronode-firmware-{m['version']}.zip"
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
    for p in sorted(out.iterdir()):
        z.write(p, p.name)
print(f"manifest signed, {archive.name} rebuilt (sha256 {hashlib.sha256(archive.read_bytes()).hexdigest()})")
PY
