#!/bin/sh
# 把 main 的最新改动同步到私有版(v3-browser-mod)
set -e
cd "$(dirname "$0")"
git checkout v3-browser-mod
git merge main -m "Sync from main: $(git log main -1 --format='%s')"
git push buildserver v3-browser-mod
git checkout main
echo "✓ 私有版已同步并推送"
