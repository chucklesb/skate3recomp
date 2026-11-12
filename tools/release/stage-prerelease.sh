#!/usr/bin/env bash
# Build a linux-aarch64 *testing* pre-release locally and hand it to CI.
#
#   tools/release/stage-prerelease.sh <tag>     e.g. v2.1.0-testing.1
#
# Same flow as stage-release.sh, for test builds:
#   - the tag is v<upstream version>-testing.<n>;
#   - the AppImage is the testing variant (SKATE3_APPIMAGE_VARIANT=testing):
#     app name "Skate3Recomp Testing", red icon, file
#     skate3recomp-testing-<version>-aarch64.AppImage, so a hand import sits
#     next to the main install in DroidDeck instead of replacing it;
#   - only the AppImage and its .sha256 are uploaded, to a draft
#     *pre-release*, and .github/workflows/prerelease-aarch64.yml validates
#     and publishes it without marking it latest. DroidDeck's GitHub install
#     ignores pre-releases, so players on the normal channel never get it.
#
# Environment: SKATE3_RELEASE_REMOTE (default: fork), SKATE3_SKIP_BUILD=1
# (reuse out/; the version check still has to pass).

set -euo pipefail

tag=${1:?usage: stage-prerelease.sh <tag>  (e.g. v2.1.0-testing.1)}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
cd "$repo"
remote=${SKATE3_RELEASE_REMOTE:-fork}

die() { echo "stage-prerelease: $*" >&2; exit 1; }

[[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+-testing\.[0-9]+$ ]] \
    || die "tag '$tag' must look like v<upstream version>-testing.<n>, e.g. v2.1.0-testing.1"
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

# RelWithDebInfo (optimized, debug info kept in the AppImage) so crash
# addresses from testers' devices map to source lines; the notes the
# workflow writes say so. Real releases (stage-release.sh) stay `release`.
notes=docs/release-notes/$tag.md
[ -s "$notes" ] && grep -q '^## Notable Changes' "$notes" \
    || die "$notes with a '## Notable Changes' section must be committed at the tag (docs/linux-aarch64.md, Release notes)"

export SKATE3_APPIMAGE_VARIANT=testing
export SKATE3_APPIMAGE_KEEP_DEBUG=1
if [ -z "${SKATE3_SKIP_BUILD:-}" ]; then
    # No `package` step: pre-releases ship only the AppImage (and the zip
    # packer expects the release library names).
    tools/cross-aarch64/build.sh relwithdebinfo sysroot codegen build appimage
else
    tools/cross-aarch64/build.sh relwithdebinfo appimage
fi

build=out/build/linux-aarch64-relwithdebinfo
built=$(sed -n 's/^#define SKATE3_VERSION_STRING "\(.*\)"$/\1/p' "$build/include/skate3_version.h")
[ "$built" = "$version" ] || die "the build reports version '$built', expected '$version' (rebuild from the tag)"

image=out/release/skate3recomp-testing-$version-aarch64.AppImage
[ -f "$image" ] && [ -f "$image.sha256" ] || die "$image or its .sha256 is missing"

gh_() { nix develop "$repo#cross" --command gh "$@"; }

if gh_ release view "$tag" --repo "$slug" >/dev/null 2>&1; then
    die "a release for $tag already exists on $slug; delete the draft or use a new tag"
fi
gh_ release create "$tag" --repo "$slug" --draft --prerelease --verify-tag \
    --title "Skate3Recomp $version (linux-aarch64, testing)" \
    --notes "Draft: awaiting validation by the prerelease-aarch64 workflow." \
    "$image" "$image.sha256"
echo "stage-prerelease: draft pre-release $tag created on $slug"

gh_ workflow run prerelease-aarch64.yml --repo "$slug" --ref "$tag" -f tag="$tag"
echo "stage-prerelease: dispatched prerelease-aarch64.yml; waiting for the run to appear"
sleep 5
run_id=$(gh_ run list --repo "$slug" --workflow prerelease-aarch64.yml --limit 1 --json databaseId --jq '.[0].databaseId')
gh_ run watch "$run_id" --repo "$slug" --exit-status
echo "stage-prerelease: published https://github.com/$slug/releases/tag/$tag"
