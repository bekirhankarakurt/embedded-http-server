# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 1.0.x   | Yes       |
| < 1.0   | No        |

## Reporting a vulnerability

Please report vulnerabilities privately through
[GitHub Security Advisories](https://github.com/bekirhankarakurt/embedded-http-server/security/advisories/new)
instead of opening a public issue. Include:

- affected version or commit,
- a minimal request sequence or program that reproduces the problem,
- the observed and expected behaviour, and your assessment of the impact.

You can expect an acknowledgement within 5 working days and a fix or mitigation plan within
30 days for confirmed issues. Credit is given in the release notes unless you prefer otherwise.

## Scope and hardening notes

The server is designed to sit on a device's management network. It does not implement TLS;
terminate TLS in a reverse proxy (or a TLS-capable gateway) when traffic leaves a trusted
segment, because HTTP Basic credentials are otherwise sent in clear text.

Built-in defences: bounded request line, header and body sizes; request, idle and write
timeouts against slow clients; connection cap; rejection of ambiguous message framing;
path-traversal and symlink-escape checks for static files; constant-time credential comparison.
