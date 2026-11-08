#!/bin/sh
# push-github.sh — publish this repo's main to GitHub through the export filter.
#
# The GitHub history is a rewrite of the Gitea one: never in it are the
# third-party media directories, the old 1.4 GB images/, the vendor CI or the
# Windows-platform overlay; board addresses and the SSH password appear only as
# placeholders. The Gitea remote keeps the unfiltered history — that is where
# the private build lives.
#
#   ./tools/push-github.sh                    # update GitHub (fast-forward)
#   PUSH_ARGS=--force ./tools/push-github.sh  # history change / repair
#
# Title-level scrubbing (third-party program and audio names) is deliberately
# NOT in this file: it lives in tools/export-extra-filters.local, which is
# git-ignored. Format, one directive per line:
#     regex <extended-regexp>      drop every matching path from history
#     replace <old==>new>          rewrite every matching byte in every blob
#
# The rewrite is deterministic: identical input commits give identical output
# hashes, so ordinary pushes work as soon as GitHub has seen the filtered
# history once.
set -eu

GH_URL=${GH_URL:-https://github.com/Limbuntu/galileo-dgpu.git}
BRANCH=${BRANCH:-main}

command -v git >/dev/null || { echo "push-github: git not found" >&2; exit 1; }
git rev-parse --git-dir >/dev/null || { echo "push-github: not a git repository" >&2; exit 1; }
command -v git-filter-repo >/dev/null || {
	echo "push-github: git-filter-repo missing (brew install git-filter-repo)" >&2; exit 1; }
if command -v gh >/dev/null; then
	gh auth status >/dev/null 2>&1 || {
		echo "push-github: gh is not logged in (gh auth login)" >&2; exit 1; }
fi

SRC=$(git rev-parse --show-toplevel)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/galileo-gh.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "== cloning $SRC"
git clone --no-hardlinks -q "$SRC" "$WORK/repo"
cd "$WORK/repo"
[ "$(git rev-parse "refs/heads/$BRANCH")" = "$(git rev-parse "HEAD")" ] || \
	git checkout -q "$BRANCH"

# Placeholders, applied to every blob in history. Longest match first: the
# "root@<ip>" form would otherwise be half-replaced by the bare address.
cat > "$WORK/replace.txt" <<'EOF'
root@<board-ip>==>root@<board-ip>
sshpass -p <board-password>==>sshpass -p <board-password>
password `<board-password>`==>password `<board-password>`
密码 `<board-password>`==>密码 `<board-password>`
password <board-password>==>password <board-password>
GITEA-PRIVATE==>GITEA-PRIVATE
BOARD-IP==>BOARD-IP
EOF

EXTRA="$SRC/tools/export-extra-filters.local"

echo "== filtering history"
set -- --force --invert-paths \
	--path board/intel/galileo/rootfs_overlay_dosbox \
	--path board/intel/galileo/dosgames-assets \
	--path board/intel/galileo/post-build-dosbox.sh \
	--path configs/galileo_dosbox_defconfig \
	--path package/dosbox \
	--path tools/push-dosgames.sh \
	--path tools/smoke-dosgame.sh \
	--path tools/verify-dosbox-image.sh \
	--path board/intel/galileo/rootfs_overlay_v4 \
	--path board/intel/galileo/rootfs_overlay_workstation/root/mc \
	--path board/intel/galileo/rootfs_overlay_workstation/root/Desktop \
	--path board/intel/galileo/rootfs_overlay_workstation/usr/share/applications \
	--path board/intel/galileo/rootfs_overlay_workstation/usr/share/icons \
	--path images \
	--path .gitea \
	--path board/intel/galileo/rootfs_overlay_desktop/root/doom/doom.log

if [ -f "$EXTRA" ]; then
	while IFS=' ' read -r _kind _value; do
		case "$_kind" in
		regex)   set -- "$@" --path-regex "$_value" ;;
		replace) printf '%s\n' "$_value" >> "$WORK/replace.txt" ;;
		esac
	done < "$EXTRA"
else
	echo "push-github: warning: $EXTRA absent — only the structural paths are filtered" >&2
fi

git filter-repo "$@" \
	--replace-text "$WORK/replace.txt" \
	--message-callback '
for _old, _new in ((b"root@<board-ip>", b"root@<board-ip>"),
                   (b"GITEA-PRIVATE", b"GITEA-PRIVATE"),
                   (b"BOARD-IP", b"BOARD-IP")):
    message = message.replace(_old, _new)
return message' >/dev/null

echo "== checking the result"
bad=""
for _p in images .gitea \
	  board/intel/galileo/dosgames-assets \
	  board/intel/galileo/rootfs_overlay_dosbox \
	  board/intel/galileo/post-build-dosbox.sh \
	  configs/galileo_dosbox_defconfig \
	  package/dosbox \
	  tools/push-dosgames.sh \
	  tools/smoke-dosgame.sh \
	  tools/verify-dosbox-image.sh \
	  board/intel/galileo/rootfs_overlay_v4 \
	  board/intel/galileo/rootfs_overlay_workstation/root/mc \
	  board/intel/galileo/rootfs_overlay_workstation/root/Desktop \
	  board/intel/galileo/rootfs_overlay_workstation/usr/share/applications \
	  board/intel/galileo/rootfs_overlay_workstation/usr/share/icons; do
	_hit=$(git ls-tree -r --name-only HEAD -- "$_p")
	[ -z "$_hit" ] || bad="$bad$_hit
"
done
if [ -f "$EXTRA" ]; then
	_hist=$(git log --all --format= --name-only | sort -u)
	_leakpat=""
	while IFS=' ' read -r _kind _value; do
		case "$_kind" in
		regex)
			_hit=$(printf '%s\n' "$_hist" | grep -E "$_value" || true)
			[ -z "$_hit" ] || bad="$bad$_hit
" ;;
		replace)
			_old=${_value%%==>*}
			_leakpat="$_leakpat${_leakpat:+|}$_old" ;;
		esac
	done < "$EXTRA"
	if [ -n "$_leakpat" ]; then
		_hit=$(git grep -I -n -E "$_leakpat" $(git rev-list --all) -- 2>/dev/null | head -3 || true)
		[ -z "$_hit" ] || bad="$bad$_hit
"
	fi
fi
[ -z "$bad" ] || { echo "push-github: filtered tree still has:" >&2; printf '%s' "$bad" >&2; exit 1; }

big=$(git rev-list --objects --all | git cat-file --batch-check='%(objecttype) %(objectsize)' |
	awk '$1=="blob" && $2 > 50000000' | head -1)
[ -z "$big" ] || { echo "push-github: a blob over 50 MB survived the filter" >&2; exit 1; }

leak=$(git grep -I -n -E '192\.168\.12[36]\.|sshpass -p <board-password>|password `<board-password>`' $(git rev-list --all) -- 2>/dev/null | head -5 || true)
[ -z "$leak" ] || { echo "push-github: address or password still in history:" >&2; echo "$leak" >&2; exit 1; }

leak=$(git log --all --format='%B' | grep -nE '192\.168\.12[36]\.' | head -5 || true)
[ -z "$leak" ] || { echo "push-github: address still in a commit message:" >&2; echo "$leak" >&2; exit 1; }

echo "== pushing $BRANCH to $GH_URL"
git remote add github "$GH_URL"
# shellcheck disable=SC2086
git push ${PUSH_ARGS:-} github "refs/heads/$BRANCH:refs/heads/$BRANCH"

# Every tag the remote already carries is rewritten as well: a tag left on the
# old history keeps the removed content reachable (and clonable) even after the
# branch no longer references it.
remote_tags=$(git ls-remote --refs --tags "$GH_URL" 2>/dev/null |
	sed -n 's|.*refs/tags/||p' | sort -u)
if [ -n "$remote_tags" ]; then
	for _t in $remote_tags; do
		if git rev-parse -q --verify "refs/tags/$_t" >/dev/null; then
			echo "refs/tags/$_t:refs/tags/$_t"
		else
			echo "push-github: remote tag $_t has no filtered counterpart" >&2
		fi
	done > "$WORK/tagrefs"
	if [ -s "$WORK/tagrefs" ]; then
		echo "== pushing $(wc -l < "$WORK/tagrefs" | tr -d ' ') tag(s)"
		# shellcheck disable=SC2086
		git push ${PUSH_ARGS:-} github $(cat "$WORK/tagrefs")
	fi
fi

echo "== done: $(git log --oneline -1)"
