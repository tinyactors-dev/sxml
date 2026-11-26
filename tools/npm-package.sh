#!/bin/sh
# Builds, checks and publishes the JavaScript package @tinyactors/sxml
# (bindings/javascript) from a release tag. Run it through mise, which pins
# the tools (mise.toml):
#
#   mise run npm:pack [-- TAG]      build and check the tarball of TAG
#                                   (default: the tag of include/sxml/version.h)
#   mise run npm:publish [-- TAG]   the same, then `npm stage publish`;
#                                   only in GitHub Actions (npm.yml)
#
# or directly: tools/npm-package.sh pack [TAG] | publish [TAG]
#
# The package is built from the tagged commit, in a git worktree under
# build/npm/TAG/, whatever is checked out: the script on main can build an
# older tag. Steps:
#   1. the tag's version.h, package.json and package-lock.json name one version
#   2. the wasm preset builds sxml.wasm (sxml_javascript_module) and the
#      tests' language modules
#   3. npm ci, the package's tests (ctest javascript/*: the API, the size
#      budget, the types), npm run build
#   4. npm pack into build/npm/; the tarball holds package.json, README.md,
#      dist/index.js, dist/index.d.ts, dist/sxml.wasm and dist/allkeys.txt
#   5. the tarball is installed into a fresh project, which imports the
#      package by its name and evaluates an XPath expression
#   6. (publish) unless npm has the version already: npm stage publish with
#      npm trusted publishing (OIDC, no token; provenance attached). A
#      maintainer approves the staged version with 2FA on npmjs.com, or with
#      `npm stage list @tinyactors/sxml` and `npm stage approve ID`.
#
# A version with a prerelease part (0.3.0-rc.1) goes to the dist-tag named by
# its first identifier (rc), never to latest.
#
# npm 11.15.0 or later stages packages; the script runs the npm it pins below
# through npx, since the Node.js of mise.toml ships an older one.

set -eu

repository_root=$(cd "$(dirname "$0")/.." && pwd)
npm_directory=$repository_root/build/npm
staging_npm=npm@12.2.3

fail() {
    echo "npm-package.sh: $*" >&2
    exit 1
}

version_of_header() {
    header=$1
    major=$(sed -n 's/^#define SXML_VERSION_MAJOR \([0-9]*\)$/\1/p' "$header")
    minor=$(sed -n 's/^#define SXML_VERSION_MINOR \([0-9]*\)$/\1/p' "$header")
    patch=$(sed -n 's/^#define SXML_VERSION_PATCH \([0-9]*\)$/\1/p' "$header")
    [ -n "$major" ] && [ -n "$minor" ] && [ -n "$patch" ] || fail "no version in $header"
    echo "$major.$minor.$patch"
}

json_field() {
    node -e 'const value = JSON.parse(require("node:fs").readFileSync(process.argv[1], "utf8"))[process.argv[2]]; if (value === undefined) process.exit(1); console.log(value)' "$1" "$2"
}

# The worktree of TAG, made (or refreshed) under build/npm/TAG/source.
source_of_tag() {
    tag=$1
    git -C "$repository_root" rev-parse -q --verify "refs/tags/$tag^{commit}" >/dev/null ||
        git -C "$repository_root" fetch -q origin "refs/tags/$tag:refs/tags/$tag" ||
        fail "no tag $tag here or at origin"
    source=$npm_directory/$tag/source
    if [ -d "$source" ]; then
        git -C "$source" checkout -q --detach "$tag"
    else
        mkdir -p "$npm_directory/$tag"
        git -C "$repository_root" worktree add -q --detach "$source" "$tag"
    fi
    echo "$source"
}

check_versions() {
    source=$1
    tag=$2
    package=$source/bindings/javascript
    header_version=$(version_of_header "$source/include/sxml/version.h")
    package_version=$(json_field "$package/package.json" version)
    lock_version=$(json_field "$package/package-lock.json" version)
    [ "$tag" = "v$package_version" ] ||
        fail "the tag $tag does not name package.json's version $package_version"
    [ "$header_version" = "${package_version%%-*}" ] ||
        fail "version.h says $header_version, package.json $package_version"
    [ "$lock_version" = "$package_version" ] ||
        fail "package-lock.json says $lock_version, package.json $package_version"
}

build_module() {
    source=$1
    if [ -z "${WASI_SDK_PREFIX:-}" ] && command -v mise >/dev/null 2>&1; then
        mise_wasi_sdk=$(mise where github:WebAssembly/wasi-sdk 2>/dev/null || true)
        [ -z "$mise_wasi_sdk" ] || export WASI_SDK_PREFIX="$mise_wasi_sdk"
    fi
    (cd "$source" &&
        cmake --preset wasm >/dev/null &&
        cmake --build --preset wasm)
}

build_package() {
    source=$1
    wasm=$source/build/wasm/bindings/javascript/sxml.wasm
    [ -f "$wasm" ] || fail "no module at $wasm"
    (cd "$source/bindings/javascript" &&
        rm -rf dist &&
        npm ci --no-audit --no-fund)
    (cd "$source" && ctest --preset wasm -R '^javascript/' --output-on-failure)
    (cd "$source/bindings/javascript" &&
        SXML_WASM_PATH=$wasm npm run build)
}

pack_package() {
    source=$1
    tag=$2
    tarball_directory=$npm_directory/$tag
    rm -f "$tarball_directory"/*.tgz
    (cd "$source/bindings/javascript" &&
        npm pack --pack-destination "$tarball_directory" >/dev/null)
    set -- "$tarball_directory"/*.tgz
    [ -f "$1" ] || fail "npm pack wrote no tarball"
    tarball=$1
    contents=$(tar -tzf "$tarball")
    for expected in package/package.json package/README.md package/dist/index.js \
        package/dist/index.d.ts package/dist/sxml.wasm package/dist/allkeys.txt; do
        echo "$contents" | grep -qx "$expected" || fail "$tarball lacks $expected"
    done
    echo "$tarball"
}

# Installs the tarball into an empty project and uses it by its name, as a
# user of the published package would.
smoke_test() {
    tarball=$1
    tag=$2
    project=$npm_directory/$tag/smoke
    rm -rf "$project"
    mkdir -p "$project"
    (cd "$project" &&
        echo '{ "name": "smoke", "private": true, "type": "module" }' >package.json &&
        npm install --no-audit --no-fund "$tarball" >/dev/null &&
        cat >smoke.mjs <<'SMOKE' &&
import { load } from '@tinyactors/sxml';

const sxml = await load();
const processor = sxml.createProcessor();
const books = processor.parse('<books><book year="1979">GEB</book><book year="1968">TAOCP</book></books>');
const text = processor.evaluate('//book[@year < 1975] ! string()', { context: books }).toString();
if (!text.includes('TAOCP') || text.includes('GEB')) {
  throw new Error(`expected TAOCP, got ${text}`);
}
processor.dispose();
console.log('smoke test: @tinyactors/sxml loads and evaluates');
SMOKE
        node smoke.mjs)
}

pack() {
    tag=${1:-v$(version_of_header "$repository_root/include/sxml/version.h")}
    source=$(source_of_tag "$tag")
    check_versions "$source" "$tag"
    build_module "$source"
    build_package "$source"
    tarball=$(pack_package "$source" "$tag")
    smoke_test "$tarball" "$tag"
    echo "$tarball"
}

publish() {
    if [ "${GITHUB_ACTIONS:-}" != true ] || [ -z "${ACTIONS_ID_TOKEN_REQUEST_URL:-}" ]; then
        fail "publish runs only in GitHub Actions with id-token: write (.github/workflows/npm.yml); use pack here"
    fi
    tag=${1:-v$(version_of_header "$repository_root/include/sxml/version.h")}
    version=${tag#v}
    name=@tinyactors/sxml
    if npm view "$name@$version" version >/dev/null 2>&1; then
        echo "$name@$version is on npm already: nothing to do."
        return 0
    fi
    tarball=$(pack "$tag" | tail -n 1)
    dist_tag=latest
    case $version in
        *-*)
            dist_tag=${version#*-}
            dist_tag=${dist_tag%%.*}
            ;;
    esac
    # Publish the built directory, whose package.json names the repository
    # that npm's trusted publisher checks; it packs the same files as $tarball.
    (cd "$npm_directory/$tag/source/bindings/javascript" &&
        npx --yes "$staging_npm" stage publish --access public --tag "$dist_tag")
    cat <<MESSAGE

Staged $name@$version (dist-tag $dist_tag) from $(basename "$tarball").
It is not public yet: approve it with 2FA on npmjs.com (the package,
Staged Packages, Approve), or from a terminal:
  npx $staging_npm stage list $name
  npx $staging_npm stage approve STAGE-ID
MESSAGE
}

[ $# -ge 1 ] || fail "usage: npm-package.sh pack [TAG] | publish [TAG]"
command=$1
shift
case $command in
    pack) pack "$@" ;;
    publish) publish "$@" ;;
    *) fail "unknown command '$command' (pack, publish)" ;;
esac
