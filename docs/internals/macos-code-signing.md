# macOS code signing and notarization

Everything here is about the release `.pkg` and `.dmg`. A local build needs
none of it: the signing knobs are empty by default, and an unsigned package
still builds and installs.

## Why it matters

macOS 15 removed the Control-click "Open anyway" shortcut. An unsigned,
un-notarized package downloaded from a browser cannot be opened by any
ordinary gesture — the user has to dismiss a malware warning, go to
**System Settings → Privacy & Security → Open Anyway**, and re-authenticate,
all within about an hour of the block. Inside a `.dmg` it is worse, because
the package then sits on a read-only volume where that recovery path often
does not stick.

So signing is not polish. An unsigned `.pkg` is a worse experience than the
plain tarball it replaces.

## What you need

Two **different** certificates. Mixing them up is the usual failure:

| Certificate | Signs |
|---|---|
| Developer ID **Application** | the Mach-O binaries, and the `.dmg` |
| Developer ID **Installer** | the `.pkg` |

Plus an App Store Connect API key for notarization. An API key rather than
an Apple ID and app-specific password: it carries no personal account, can
be scoped to the Developer role, and is revocable on its own.

### One-time setup

1. In Keychain Access, **Certificate Assistant → Request a Certificate From
   a Certificate Authority**, saved to disk.
2. At [developer.apple.com](https://developer.apple.com/account/resources/certificates/list),
   create a *Developer ID Application* and a *Developer ID Installer*
   certificate from that request. Download and double-click both.
3. Export each from Keychain Access as a `.p12` with a password.
4. Create an API key under **App Store Connect → Users and Access →
   Integrations → Keys**. Note the Key ID and the Issuer ID (a UUID shown
   above the key table) and download the `.p8` — it is downloadable once.

Check what you have:

```sh
security find-identity -v
```

`-p codesigning` is the more common invocation but it *omits* Installer
certificates, which is a good way to conclude you are missing one you
actually have.

### CI secrets

Set at organization level with visibility restricted to this repository:

| Secret | Value |
|---|---|
| `BUILD_CERTIFICATE_BASE64` | `base64 -i application.p12` |
| `INSTALLER_CERTIFICATE_BASE64` | `base64 -i installer.p12` |
| `P12_PASSWORD` | the export password |
| `KEYCHAIN_PASSWORD` | any random string; protects the runner's throwaway keychain |
| `APPLE_API_KEY_P8` | `base64 -i AuthKey_XXXXXXXXXX.p8` |
| `APPLE_API_KEY_ID` | the 10-character Key ID |
| `APPLE_API_ISSUER_ID` | the Issuer UUID |

The `package-macos` job decides whether to sign by testing whether
`BUILD_CERTIFICATE_BASE64` is non-empty, **not** by branch name. GitHub
withholds secrets from fork pull requests, which is the real precondition; a
branch test both breaks every fork PR and leaves the signing path
unexercised until after a merge.

## How the build wires it up

```sh
cmake -S . -B build \
  -DFASTCACHED_PACKAGE_ROOT_PREFIX=ON \
  -DFASTCACHED_MACOS_SIGN_IDENTITY_APP="Developer ID Application: … (TEAMID)" \
  -DFASTCACHED_MACOS_SIGN_IDENTITY_PKG="Developer ID Installer: … (TEAMID)" \
  -DFASTCACHED_MACOS_NOTARIZE=ON
cd build && cpack -G productbuild
```

Notarization needs a stored credential profile, once per machine:

```sh
xcrun notarytool store-credentials fastcached-notary \
      --key AuthKey_XXXXXXXXXX.p8 --key-id <KEY-ID> --issuer <ISSUER-UUID>
```

The order is fixed by two constraints, and neither step can move:

1. **`cmake/MacOSSignBinaries.cmake`** runs as `CPACK_PRE_BUILD_SCRIPTS`, in
   the only window that works: after CPack has staged the payload and applied
   `CPACK_STRIP_FILES`, and before `pkgbuild` seals it. Signing any earlier is
   pointless — `strip` rewrites the binary and invalidates the signature, and
   on arm64 an invalid signature is not a warning but a refusal to execute,
   so the package would install a daemon that dies with `Killed: 9`.
2. **`cmake/MacOSNotarizePkg.cmake`** runs as `CPACK_POST_BUILD_SCRIPTS`: it
   notarizes and staples the `.pkg`, *then* wraps it in the `.dmg`, then signs
   and notarizes that. Stapling the package before wrapping is deliberate — a
   ticket on the outer image does not travel with a `.pkg` the user drags out
   of it, so an offline install of the extracted package would fall back to an
   online Gatekeeper check.

The `.pkg` itself is signed by CPack via `CPACK_PKGBUILD_IDENTITY_NAME` and
`CPACK_PRODUCTBUILD_IDENTITY_NAME`.

Binaries are signed with `--options=runtime --timestamp`: notarization
rejects a submission missing either. No entitlements file is used — unlike a
GUI app, fastcached needs neither JIT nor relaxed library validation, and
entitlements it does not need only widen its attack surface.

The `.dmg` is signed **without** `--options=runtime`: a disk image holds no
executable code of its own, and the hardened runtime is a property of a
running process.

## Verifying

```sh
pkgutil --check-signature fastcached-*.pkg
spctl --assess --verbose=4 --type install fastcached-*.pkg   # what Gatekeeper decides
xcrun stapler validate fastcached-*.pkg
xcrun stapler validate fastcached-*.dmg
```

## Troubleshooting

| Symptom | Cause |
|---|---|
| `codesign` hangs in CI | The key's ACL does not permit the tool. Needs both `security import -T /usr/bin/codesign` and `security set-key-partition-list`. |
| `resource fork, Finder information, or similar detritus not allowed` | Stray extended attributes; `xattr -c` the file first. |
| Notarization rejected, no reason given | Fetch it: `xcrun notarytool log <submission-id> --keychain-profile fastcached-notary`. |
| `The signature does not include a secure timestamp` | `--timestamp` was omitted. |
| `Killed: 9` after install | The binary was stripped after signing. |
| No Installer identity found | `security find-identity -v -p codesigning` does not list Installer certificates; drop `-p codesigning`. |

## Related

Configure-time guard: `FASTCACHED_MACOS_NOTARIZE` requires both identities.
Apple rejects an unsigned or ad-hoc submission, but only after a multi-minute
round trip, so the mistake is caught at configure time instead.
