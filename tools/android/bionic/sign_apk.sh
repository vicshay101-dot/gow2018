#!/usr/bin/env bash
set -Eeuo pipefail
cd -- "$(dirname -- "$0")/../../.."
root=$PWD
sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
: "${sdk:?Set ANDROID_HOME or ANDROID_SDK_ROOT}"
bt="$sdk/build-tools/${GOW3_BUILD_TOOLS:-36.0.0}"
unsigned="$root/android-launcher/app/build/outputs/apk/release/app-arm64-v8a-release-unsigned.apk"
apk="$root/out/release/God-of-War-III-Daedalus-Fork-v2.1.6-daedalus.4-bionic-arm64.apk"
aligned="$root/out/release/bionic-aligned.apk"
key="$root/.local-deps/android/bionic-development.keystore"
mkdir -p "$(dirname "$key")" "$(dirname "$apk")"
# Experimental testing only. A release must use the project's controlled signing key.
if [[ ! -f $key ]]; then
    keytool -genkeypair -keystore "$key" -storepass android -keypass android -alias bionic \
        -dname 'CN=Daedalus Bionic experimental development' -keyalg RSA -keysize 2048 -validity 10000
fi

# Keep 16 KiB native-library page alignment, then verify the aligned input.
"$bt/zipalign" -P 16 -f 4 "$unsigned" "$aligned"
"$bt/zipalign" -c -P 16 -v 4 "$aligned"

# Emit all Android-supported APK signature schemes for broad OEM installer compatibility.
"$bt/apksigner" sign \
    --ks "$key" --ks-key-alias bionic \
    --ks-pass pass:android --key-pass pass:android \
    --v1-signing-enabled true \
    --v2-signing-enabled true \
    --v3-signing-enabled true \
    --out "$apk" "$aligned"

# Fail the workflow unless every requested signature scheme verifies.
verify_output=$("$bt/apksigner" verify --verbose --print-certs "$apk")
printf '%s\n' "$verify_output"
for scheme in v1 v2 v3; do
    if ! grep -Fq "Verified using $scheme scheme: true" <<<"$verify_output"; then
        echo "ERROR: APK signature scheme $scheme was not verified" >&2
        exit 1
    fi
done

python3 tools/android/bionic/validate_apk.py "$apk" --aapt "$bt/aapt"
(cd out/release; sha256sum "$(basename "$apk")" > "$(basename "$apk").sha256")
echo "Daedalus Fork Bionic APK: $apk"
