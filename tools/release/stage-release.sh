#!/usr/bin/env bash
# Build a linux-aarch64 release locally and hand it to CI for publishing.
#
#   tools/release/stage-release.sh <tag>        e.g. v2.1.0-aarch64.1
#
# The game can only be built where the Skate 3 dump and Title Update 3 are
# (codegen reads them), so release binaries are built here. This script:
#   1. checks the tag (v<upstream version>-aarch64.<n>), that HEAD is that tag,
#      that the tree is clean and that the tag is pushed to the release remote;
#   2. runs tools/cross-aarch64/build.sh release (sysroot, codegen, build,
#      package, appimage; the AppImage is validated as it is built);
#   3. creates a *draft* GitHub release on the fork with the AppImage, the zip
#      and their .sha256 files;
#   4. dispatches .github/workflows/release-aarch64.yml, which re-validates the
#      uploaded assets on an arm64 runner and publishes the release.
#
# Environment:
#   SKATE3_RELEASE_REMOTE   git remote of the fork (default: fork)
#   SKATE3_SKIP_BUILD=1     reuse the existing out/ build (the version check
#                           still has to pass)
# Needs `gh auth login` for the fork. Runs the build through nix itself.

set -euo pipefail

tag=${1:?usage: stage-release.sh <tag>  (e.g. v2.1.0-aarch64.1)}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
cd "$repo"
remote=${SKATE3_RELEASE_REMOTE:-fork}

die() { echo "stage-release: $*" >&2; exit 1; }

[[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+-aarch64\.[0-9]+$ ]] \
    || die "tag '$tag' must look like v<upstream version>-aarch64.<n>, e.g. v2.1.0-aarch64.1"
version=${tag#v}

url=$(git remote get-url "$remote" 2>/dev/null) \
    || die "no git remote '$remote'; add your fork with: git remote add $remote <url>"
slug=$(sed -E 's#^(git@github\.com:|https://github\.com/)##; s#\.git$##' <<<"$url")
[[ $slug =~ ^[^/]+/[^/]+$ ]] || die "cannot read a GitHub owner/repo from '$url'"
[ "$slug" != "rubensteezy/skate3recomp" ] || die "remote '$remote' points at upstream; releases go to your fork"

[ "$(git rev-parse HEAD)" = "$(git rev-parse "$tag^{commit}" 2>/dev/null)" ] \
    || die "HEAD is not at $tag (git checkout $tag)"
[ -z "$(git status --porcelain --untracked-files=normal)" ] || die "working tree is not clean"
[ -z "$(git -C third_party/rexglue-sdk status --porcelain)" ] || die "rexglue-sdk submodule is not clean"
remote_tag=$(git ls-remote --tags "$remote" "refs/tags/$tag" | cut -f1)
[ -n "$remote_tag" ] || die "$tag is not on $remote (git push $remote $tag)"
[ "$remote_tag" = "$(git rev-parse "$tag")" ] || die "$tag on $remote differs from the local tag"

if [ -z "${SKATE3_SKIP_BUILD:-}" ]; then
    tools/cross-aarch64/build.sh release
fi

build=out/build/linux-aarch64-release
built=$(sed -n 's/^#define SKATE3_VERSION_STRING "\(.*\)"$/\1/p' "$build/include/skate3_version.h")
[ "$built" = "$version" ] || die "the build reports version '$built', expected '$version' (rebuild from the tag)"

image=out/release/Skate3Recomp-$version-aarch64.AppImage
zip=out/release/Skate3Recomp-Linux-aarch64-$version.zip
[ -f "$image" ] && [ -f "$image.sha256" ] || die "$image or its .sha256 is missing"
cp out/release/Skate3Recomp-Linux-aarch64.zip "$zip"
(cd out/release && sha256sum "$(basename "$zip")" > "$(basename "$zip").sha256")

gh_() { nix develop "$repo#cross" --command gh "$@"; }

if gh_ release view "$tag" --repo "$slug" >/dev/null 2>&1; then
    die "a release for $tag already exists on $slug; delete the draft or use a new tag"
fi
gh_ release create "$tag" --repo "$slug" --draft --verify-tag \
    --title "Skate3Recomp $version (linux-aarch64)" \
    --notes "Draft: awaiting validation by the release-aarch64 workflow." \
    "$image" "$image.sha256" "$zip" "$zip.sha256"
echo "stage-release: draft release $tag created on $slug"

gh_ workflow run release-aarch64.yml --repo "$slug" --ref "$tag" -f tag="$tag"
echo "stage-release: dispatched release-aarch64.yml; waiting for the run to appear"
sleep 5
run_id=$(gh_ run list --repo "$slug" --workflow release-aarch64.yml --limit 1 --json databaseId --jq '.[0].databaseId')
gh_ run watch "$run_id" --repo "$slug" --exit-status
echo "stage-release: published https://github.com/$slug/releases/tag/$tag"
