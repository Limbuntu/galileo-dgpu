#!/bin/sh
# gitea-ci-watch.sh — live status of the Gitea image builds.
#
# Live state comes from the act_runner containers (Gitea only writes a task
# log once the job ends), so running builds are visible immediately.
set -u

HOST=${GITEA_HOST:-mediaserver}
CONTAINER=${GITEA_CONTAINER:-mysqlgitea-gitea-1}
REPO=${GITEA_REPO:-limbuntu/galileo-dgpu}
LOGDIR=/data/gitea/actions_log/$REPO
SINCE=${1:-0}          # ignore tasks with a smaller id

containers=$(ssh -o ConnectTimeout=10 "$HOST" \
	'docker ps -a --format "{{.Names}}|{{.Status}}" | grep GITEA-ACTIONS' 2>/dev/null)
[ -n "$containers" ] || { echo "gitea-ci-watch: no act_runner containers on $HOST"; exit 0; }

printf '%-6s %-20s %s\n' TASK IMAGE STATUS
failed=0
for line in $containers; do
	name=${line%%|*}; status=${line#*|}
	task=$(echo "$name" | sed -n 's/.*TASK-\([0-9]*\)_.*/\1/p')
	image=$(echo "$name" | sed -n 's/.*JOB-build-\(.*\)$/\1/p')
	[ -n "$task" ] || continue
	[ "$task" -ge "$SINCE" ] || continue
	case "$status" in
	*Exited*)
		log=$(ssh -o ConnectTimeout=10 "$HOST" \
			"docker exec $CONTAINER sh -c 'find $LOGDIR -name $task.log'" 2>/dev/null | head -1)
		if [ -n "$log" ]; then
			tailtxt=$(ssh -o ConnectTimeout=10 "$HOST" "docker exec $CONTAINER tail -60 $log" 2>/dev/null)
			[ -z "$image" ] && image=$(echo "$tailtxt" | grep -o 'galileo_[a-z]*_defconfig' | tail -1 | sed 's/galileo_//; s/_defconfig//')
			if echo "$tailtxt" | grep -q "Job failed"; then
				st=failed; failed=1
				err=$(echo "$tailtxt" | grep -Ei "error|not found|cannot copy" | tail -1 | sed 's/^[0-9T:.Z-]* //' | cut -c1-120)
			else
				st=ok; err=""
			fi
		else
			st=finished-no-log; err=""
		fi
		;;
	*) st=running; err="" ;;
	esac
	printf '%-6s %-20s %-10s %s\n' "$task" "${image:-?}" "$st" "$err"
done

exit $failed
