# TLS certs for the reference compose

`authd` (IF-1) and `worldd` (IF-2 at M0) are TLS 1.3 listeners — each needs a
server certificate + private key. The compose mounts this directory read-only at
`/certs` and points both daemons at `server.crt` / `server.key`.

**These files are not committed** (see `.gitignore`) — certs are per-deployment.

## Local dev: a self-signed pair

For a throwaway local realm, generate a self-signed cert/key here:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key -out server.crt \
  -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
```

That produces `deploy/docker/certs/server.crt` and `server.key`, which the
compose picks up automatically.

## Real deployments

Use a cert from your realm operator PKI (server SAD §5.1: "server cert from realm
operator PKI; client does standard verification"). Never ship the self-signed
dev pair to a public realm.
