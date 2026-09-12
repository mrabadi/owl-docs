#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s OUTPUT_DIRECTORY [FIXTURE_COUNT]\n' "$0" >&2
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage
    exit 64
fi

output_directory=$1
fixture_count=${2:-100}
if [[ ! $fixture_count =~ ^[1-9][0-9]*$ ]]; then
    printf 'FIXTURE_COUNT must be a positive integer.\n' >&2
    exit 64
fi
if [[ -e $output_directory ]]; then
    printf 'Refusing to replace existing corpus path: %s\n' "$output_directory" >&2
    exit 73
fi

for command in curl jq git sha256sum base64; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'Required command is unavailable: %s\n' "$command" >&2
        exit 69
    fi
done

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
source_catalog=$script_directory/../tests/corpus/public_sources.json
source_id=$(jq -er '.sources[0].id' "$source_catalog")
source_revision=$(jq -er '.sources[0].sourceRevision' "$source_catalog")
source_license=$(jq -er '.sources[0].license' "$source_catalog")
index_url=$(jq -er '.sources[0].indexUrl' "$source_catalog")
license_url=$(jq -er '.sources[0].licenseUrl' "$source_catalog")

output_parent=$(dirname -- "$output_directory")
mkdir -p -- "$output_parent"
output_parent=$(CDPATH= cd -- "$output_parent" && pwd)
output_directory=$output_parent/$(basename -- "$output_directory")
case "$output_directory/" in
    "$repository_root/"*)
        printf 'Corpus caches must remain outside the source repository.\n' >&2
        exit 73
        ;;
esac
staging_directory=$(mktemp -d "$output_parent/.owl-docs-corpus.XXXXXX")
cleanup() {
    rm -rf -- "$staging_directory"
}
trap cleanup EXIT
mkdir -p -- "$staging_directory/files"

curl --fail --location --silent --show-error --retry 3 \
    "$index_url" --output "$staging_directory/upstream-index.json"
curl --fail --location --silent --show-error --retry 3 \
    "$license_url" --output "$staging_directory/SOURCE-LICENSE-MPL-2.0.txt"

available_count=$(jq '[.[] | select(.type == "file") |
    select(.name | ascii_downcase | endswith(".docx")) |
    select(.name | ascii_downcase | startswith("encrypted_") | not)] | length' \
    "$staging_directory/upstream-index.json")
if (( available_count < fixture_count )); then
    printf 'Pinned source exposes %s DOCX files; %s were requested.\n' \
        "$available_count" "$fixture_count" >&2
    exit 65
fi

fixture_records=$staging_directory/fixtures.jsonl
: > "$fixture_records"
fixture_index=0
while IFS= read -r encoded_record; do
    record=$(printf '%s' "$encoded_record" | base64 --decode)
    upstream_name=$(jq -er '.name' <<<"$record")
    upstream_path=$(jq -er '.path' <<<"$record")
    git_blob_sha1=$(jq -er '.sha' <<<"$record")
    download_url=$(jq -er '.download_url' <<<"$record")
    provenance_url=$(jq -er '.html_url' <<<"$record")
    identifier=$(printf '%s-%03d' "$source_id" "$fixture_index")
    local_name=$(printf '%03d.docx' "$fixture_index")
    destination=$staging_directory/files/$local_name

    curl --fail --location --silent --show-error --retry 3 \
        "$download_url" --output "$destination"
    actual_blob_sha1=$(git hash-object "$destination")
    if [[ $actual_blob_sha1 != "$git_blob_sha1" ]]; then
        printf 'Git blob identity mismatch for %s.\n' "$upstream_path" >&2
        exit 65
    fi
    digest=$(sha256sum "$destination")
    digest=${digest%% *}

    jq -cn \
        --arg id "$identifier" \
        --arg path "files/$local_name" \
        --arg license "$source_license" \
        --arg provenanceUrl "$provenance_url" \
        --arg sourceRevision "$source_revision" \
        --arg sha256 "$digest" \
        --arg gitBlobSha1 "$git_blob_sha1" \
        --arg upstreamPath "$upstream_path" \
        --arg upstreamName "$upstream_name" \
        '{id: $id, path: $path, license: $license,
          provenanceUrl: $provenanceUrl, sourceRevision: $sourceRevision,
          sha256: $sha256, gitBlobSha1: $gitBlobSha1,
          upstreamPath: $upstreamPath, upstreamName: $upstreamName,
          checks: ["exact-copy"]}' >> "$fixture_records"
    fixture_index=$((fixture_index + 1))
done < <(jq -r --argjson count "$fixture_count" '
    [.[] | select(.type == "file") |
     select(.name | ascii_downcase | endswith(".docx")) |
     select(.name | ascii_downcase | startswith("encrypted_") | not)] |
    sort_by(.path) | .[0:$count][] | @base64' \
    "$staging_directory/upstream-index.json")

jq -s --argjson count "$fixture_count" \
    '{schema: "owl-docs-corpus-v1", minimumFixtureCount: $count,
      fixtures: .}' "$fixture_records" > "$staging_directory/manifest.json"
rm -f -- "$fixture_records"
mv -- "$staging_directory" "$output_directory"
trap - EXIT

printf 'Prepared %s pinned, hash-verified public DOCX fixtures in %s\n' \
    "$fixture_count" "$output_directory"
printf 'Manifest: %s/manifest.json\n' "$output_directory"
