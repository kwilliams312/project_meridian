#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# In-cluster concurrency harness: provision N accounts + N characters, then run
# N meridian-bot sessions against a realm (default dev). Usage:
#   scripts/dev/loadtest.sh [--realm dev] [--count 5] [--duration 60] [--tag dev]
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
export REALM=dev COUNT=5 DURATION=60 PREFIX=loadtest PASSWORD=loadtestpass TAG=dev
while [ $# -gt 0 ]; do
  case "$1" in
    --realm) REALM=$2; shift 2 ;;
    --count) COUNT=$2; shift 2 ;;
    --duration) DURATION=$2; shift 2 ;;
    --tag) TAG=$2; shift 2 ;;
    --prefix) PREFIX=$2; shift 2 ;;
    --password) PASSWORD=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
export REALM COUNT DURATION PREFIX PASSWORD TAG
# Restrict envsubst to ONLY the template placeholders below. Unrestricted
# envsubst matches every $IDENTIFIER-shaped token in the input, including the
# pod-local shell vars the rendered Job scripts use at runtime ($i, $u, $U) —
# those must survive rendering untouched and be interpreted by the container's
# shell, not the host's empty/unset environment.
VARS='${REALM} ${COUNT} ${DURATION} ${PREFIX} ${PASSWORD} ${TAG}'
echo "== 1/3 provisioning ${COUNT} accounts in meridian-${REALM} =="
envsubst "$VARS" < deploy/gitops/loadtest/add-users.job.yaml | kubectl apply -f -
kubectl -n "meridian-${REALM}" wait --for=condition=complete "job/meridian-${REALM}-add-users" --timeout=300s
echo "== 2/3 creating a character per account =="
envsubst "$VARS" < deploy/gitops/loadtest/add-characters.job.yaml | kubectl apply -f -
kubectl -n "meridian-${REALM}" wait --for=condition=complete "job/meridian-${REALM}-add-characters" --timeout=300s
echo "== 3/3 launching ${COUNT} bots for ${DURATION}s =="
envsubst "$VARS" < deploy/gitops/loadtest/loadtest.job.yaml | kubectl apply -f -
sleep 5
kubectl -n "meridian-${REALM}" get pods -l app=meridian-loadtest -o wide
echo "watch: kubectl -n meridian-${REALM} get job/meridian-${REALM}-loadtest -w"
