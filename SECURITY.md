<!-- SPDX-License-Identifier: MIT -->

# Security

This is an SSH client that runs on a conference badge. It talks to hostile
networks by definition, so the transport and the terminal are treated as
security boundaries; the badge in your hand is not.

## Threat model

**In scope** — the client defends against these:

- A hostile or compromised SSH **server** (crafted key exchanges, signature
  blobs, terminal escape sequences, and command output).
- A **man in the middle** on the network, both before and after a host key is
  pinned.
- The **first-connection** case, as far as trust-on-first-use allows: a changed
  key later is caught loudly.

**Out of scope** — accepted, and documented in the README's
[What this does not protect against](README.md#what-this-does-not-protect-against):

- Physical possession of the badge. Anyone holding it has the SSH key.
- Flash dumps and other apps on the badge. NVS is unencrypted and not access
  controlled; the private key and any saved password are readable, and
  overwriting them does not scrub the old bytes from flash.
- No Secure Boot, so nothing binds the running firmware.

The mitigations for the out-of-scope items are on the server side: the badge key
is its own key, revocable in one line of `authorized_keys`, and can be fenced in
with `restrict,from=...`.

## Where the security-relevant code lives

| Area | File |
|------|------|
| Host key pinning and the changed-key prompt | `main/ssh_client.c`, `main/ui.c` |
| Known-host store (hashed NVS keys, record parsing) | `main/hosts.c` |
| The badge's Ed25519 identity | `main/keystore.c` |
| Terminal escape / UTF-8 parser (all server input) | `main/terminal.c` |
| libssh2 crypto backend on PSA + TweetNaCl | `components/libssh2/port/` |

`main/hosts.c` and the generated font are covered by host tests
(`make test`) under the address and undefined-behaviour sanitisers.

## Reviewed

The code in this tree was reviewed across those surfaces. No remotely reachable
memory-safety defect or host-key-verification bypass was found; the terminal
parser was fuzzed under ASan/UBSan without a fault. The review hardened a number
of smaller points (a changed host key now needs a deliberate second, red-flagged
confirmation before it can be re-pinned; the pin lookup is case-normalised so a
differently-cased host name cannot dodge the warning; the RNG glue fails closed
so a failed draw cannot yield a predictable identity key; a copy-id install is
only believed when the server confirms it; scrollback is dropped at each session
boundary). Residual risks are the out-of-scope items above, plus the
trust-on-first-use window and the `BOOTLOADER_REGION_PROTECTION` default noted in
the README's Crypto section.

## Reporting a vulnerability

Please report security issues privately rather than in a public issue: open a
[GitHub security advisory](https://github.com/annejan/ssh-konsool/security/advisories/new)
for the repository, or contact the maintainer directly. A fingerprint or a
minimal reproduction helps. There is no bounty; this is a hobby project, and
fixes are best-effort.
