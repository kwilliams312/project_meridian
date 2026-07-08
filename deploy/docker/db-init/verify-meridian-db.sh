#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build meridian-db and assert it self-seeds the 3-DB split on first boot.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

docker build --platform linux/amd64 -f deploy/docker/Dockerfile.db -t meridian-db:test .

cid=$(docker run -d --rm \
  -e MARIADB_ROOT_PASSWORD=meridian-root \
  -e MARIADB_USER=meridian -e MARIADB_PASSWORD=meridian \
  meridian-db:test)
trap 'docker stop "$cid" >/dev/null 2>&1 || true' EXIT

echo "waiting for mariadb to become healthy + seed..."
for i in $(seq 1 60); do
  if docker exec "$cid" healthcheck.sh --connect --innodb_initialized >/dev/null 2>&1; then break; fi
  sleep 2
done

# The three schemas must exist, and the app user must reach all three.
got=$(docker exec "$cid" mariadb -umeridian -pmeridian -N -e \
  "SELECT GROUP_CONCAT(schema_name ORDER BY schema_name) FROM information_schema.schemata \
   WHERE schema_name LIKE 'meridian\\_%';")
echo "schemas: $got"
[ "$got" = "meridian_auth,meridian_characters,meridian_world" ] || { echo "FAIL: schemas"; exit 1; }

# Auth tables must be present (proves the SOURCEd migrations ran).
tbls=$(docker exec "$cid" mariadb -umeridian -pmeridian -N meridian_auth -e "SHOW TABLES;" | wc -l | tr -d ' ')
echo "auth tables: $tbls"
[ "$tbls" -ge 1 ] || { echo "FAIL: no auth tables"; exit 1; }
echo "PASS"
