#!/bin/sh
# 把 main 的最新改动同步到私有版(workstation-private)
set -e
cd "$(dirname "$0")"
git checkout workstation-private
git merge main -m "Sync from main: $(git log main -1 --format='%s')"
git push buildserver workstation-private
git checkout main
echo "✓ 私有版已同步并推送"
