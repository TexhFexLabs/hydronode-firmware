# OTA dev key

Local signing key for testing updates over the air. Never used for a release.

```bash
bash tools/dev-keys/make-dev-key.sh        # once; --force replaces an existing key
pio run -e esp32c3-dev                     # firmware that trusts the dev key
bash tools/sign-release.sh --key tools/dev-keys/ota-dev.pem
```

Creates `ota-dev.pem` (private), `ota-dev.pub.pem` and `ota-dev.pub.h` (public key compiled into
`-DHN_OTA_DEV_KEY` builds, key ID `dev-<year>-<month>`). Everything here except this README, the
script and `.gitignore` is ignored by git. The backend trusts the same key through
`hydronode.ota.public-keys` in its dev profile.
