#!/bin/sh
# push-github.sh — publish this repo's main to GitHub through the export filter.
#
# The GitHub history is a rewrite of the Gitea one: never in it are the DOS game
# data, the old 1.4 GB images/, the vendor CI workflows or Minecraft; board
# addresses and the SSH password appear only as placeholders. The Gitea remote
# keeps the unfiltered history — that is where the game data and private build
# live.
#
#   ./tools/push-github.sh                # update GitHub (fast-forward)
#   PUSH_ARGS=--force ./tools/push-github.sh   # first migration / history repair
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

echo "== filtering history"
git filter-repo --force --invert-paths \
	--path board/intel/galileo/rootfs_overlay_dosbox/usr/share/dosgames \
	--path board/intel/galileo/dosgames-assets \
	--path images \
	--path .gitea \
	--path board/intel/galileo/rootfs_overlay_desktop/root/doom/doom.log \
	--path board/intel/galileo/rootfs_overlay_workstation/root/mc \
	--path board/intel/galileo/rootfs_overlay_v4/root/mc \
	--path board/intel/galileo/rootfs_overlay_v4/root/Desktop/minecraft.desktop \
	--path-regex 'minecraft' \
	--replace-text "$WORK/replace.txt" \
	--message-callback '
for _old, _new in ((b"root@<board-ip>", b"root@<board-ip>"),
                   (b"GITEA-PRIVATE", b"GITEA-PRIVATE"),
                   (b"BOARD-IP", b"BOARD-IP")):
    message = message.replace(_old, _new)
return message' >/dev/null

echo "== checking the result"
bad_paths=$(git ls-tree -r --name-only HEAD |
	grep -E 'usr/share/dosgames/|dosgames-assets/|^images/|^\.gitea/|minecraft|/root/mc/' || true)
[ -z "$bad_paths" ] || { echo "push-github: filtered tree still has:" >&2; echo "$bad_paths" >&2; exit 1; }

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

echo "== done: $(git log --oneline -1)"
