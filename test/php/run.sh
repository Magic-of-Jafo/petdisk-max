#!/bin/sh
# Run the petdisk.php tests against several PHP versions in Docker.
#   test/php/run.sh [versions...]      default: 7.4 8.1 8.3
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
versions=${*:-7.4 8.1 8.3}
status=0
for v in $versions; do
    echo "=== PHP $v ==="
    docker run --rm -v "$repo:/repo:ro" "php:$v-cli" sh -c '
        set -e
        mkdir -p /srv/root
        cp /repo/www/petdisk.php /srv/root/
        php -S 127.0.0.1:8080 -t /srv/root >/tmp/server.log 2>&1 &
        php /repo/test/php/test_petdisk.php http://127.0.0.1:8080/petdisk.php /srv/root
    ' || status=1
done
exit $status
