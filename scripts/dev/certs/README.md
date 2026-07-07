# Local dev TLS certs (`scripts/dev/run-local.sh`)

`authd` (IF-1) and `worldd` (IF-2 at M0) are TLS 1.3 listeners — each needs a
server cert + key. `scripts/dev/run-local.sh` generates a throwaway self-signed
`server.crt` / `server.key` here on first run (and reuses them afterwards).

**These `.crt` / `.key` files are gitignored** — they are per-machine dev secrets,
never committed. Only this README is tracked.

To regenerate manually (the one-liner `run-local.sh` uses):

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key -out server.crt \
  -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
```

This is a *local* analogue of `deploy/docker/certs/` — the same self-signed
approach, kept out of the Docker tree so the native path is self-contained. Never
use a self-signed dev pair on a public realm (server SAD §5.1).
