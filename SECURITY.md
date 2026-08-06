# Security Policy

This project follows the Eclipse Foundation Vulnerability Reporting Policy:

* https://www.eclipse.org/security/

## Reporting a Vulnerability

Please report vulnerabilities that are not yet publicly disclosed by email to
the Eclipse Foundation Security Team at security@eclipse-foundation.org. Do
NOT open a public GitHub issue for an undisclosed vulnerability.

## Scope

This repository contains the SoC-level adaptations (e.g. the display and GPU HAL glue for the hybris_generic and x86_general SoC targets). Please report here only vulnerabilities in
content authored by this project:

* Vulnerabilities in upstream OpenHarmony components (the
  `eclipse-oniro-mirrors` repositories) should be reported to the upstream
  OpenAtom OpenHarmony project.
* Vulnerabilities in the Linux kernel should be reported upstream
  (see https://docs.kernel.org/process/security-bugs.html).
* Vulnerabilities in third-party vendor binaries fetched at build time (e.g.
  Halium / vendor blobs) should be reported to their respective vendors.

## Supported Versions

Only the most recent release branch (`OpenHarmony-6.1-LTS`) is supported with
security updates. Older release branches, including `OpenHarmony-6.1-Release`,
are not maintained.
