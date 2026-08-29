# SSH for Tanmatsu and Konsool

An SSH client with a VT100/xterm terminal, for the
[Tanmatsu](https://nicolaielectronics.nl/docs/tanmatsu/) and Konsool badges.

The badge keyboard is the terminal keyboard. The screen is 100x29 characters at
8x16, or 50x14 at double size. Text is UTF-8 end to end: Latin, Greek, Cyrillic,
punctuation, arrows, maths, box drawing and block elements all render, so `htop`,
`vim` and `mc` look like themselves.

## What it does

- Saved connections in NVS: host, port, user, optional password.
- Public key login with a key the badge generates and keeps to itself. The
  private half never leaves NVS; libssh2 asks for a signature and gets one.
- Its own `ssh-copy-id`: log in with a password once, and the badge installs its
  key on the server and uses it from then on.
- Host key checking against a remembered fingerprint, with a prompt the first
  time and a warning when a known key changes.
- A terminal with colours (16, 256 and 24 bit), the alternate screen, scroll
  regions, insert and delete, and 512 lines of scrollback.

## Getting the public key onto a server

The easy way, from the badge: give a connection a host, a user and a password,
then press **F4** on it, in the connection list or in the editor. It logs in
with the password once, appends the badge's key to that account's
`authorized_keys`, and switches the connection over to key authentication. After
that the password is not needed, and you can clear it. Installing the same key
twice is harmless.

If you would rather carry the key across yourself, open **SSH key** from the
main menu. That page shows the fingerprint, the `authorized_keys` line and a QR
code of the same line, and offers:

| Key | What it does                                     |
|-----|--------------------------------------------------|
| F1  | Write `ssh_konsool.pub` to the SD card           |
| F2  | Write `/int/ssh/id_ed25519.pub`, for BadgeLink   |
| F3  | Print the line on the serial console             |
| F4  | Throw the key away and make a new one            |

Over BadgeLink that becomes:

    badgelink.sh fs download /int/ssh/id_ed25519.pub id_badge.pub
    ssh-copy-id -f -i id_badge.pub user@host

## Keys in the terminal

| Key            | Sends                                     |
|----------------|-------------------------------------------|
| Fn + Esc       | Back to the menu (local)                  |
| Fn + F1        | Toggle double size (local)                |
| Shift + Up/Dn  | Scrollback (local)                        |
| Fn + Left/Rght | Home / End                                |
| Fn + Up/Down   | Page Up / Page Down                       |
| Fn + Backspace | Delete                                    |
| Fn + F1..F6    | F7..F12                                   |
| Ctrl + letter  | The usual control codes                   |
| Alt + key      | Escape prefix (meta)                      |

## Building

Needs ESP-IDF v6.0.2.

    make prepare          # fetch the libssh2 submodule
    make sdk              # only if you have no ESP-IDF yet
    make build            # DEVICE=tanmatsu (default) or DEVICE=konsool

Point the build at an existing SDK by writing its path to `.IDF_PATH`, or set
`IDF_PATH` in the environment.

Install over BadgeLink, which also uploads the icon and metadata:

    make badgelink        # once, to fetch the tool
    make install
    make run

Or flash over USB with `make flashmonitor PORT=/dev/ttyACM0`.

## Crypto

ESP-IDF v6 ships Mbed TLS 4, which has dropped the legacy `mbedtls_rsa`,
`mbedtls_ecp` and `mbedtls_pk` interfaces that libssh2's own mbedTLS backend is
written against. So this project brings its own libssh2 crypto backend, written
against the PSA Crypto API: `components/libssh2/port/psa_backend.c`. Ed25519 and
X25519 come from TweetNaCl, because PSA has no Edwards curve support.

What that buys and costs:

- Key exchange: `curve25519-sha256`, `ecdh-sha2-nistp256/384/521`.
  The finite field Diffie-Hellman exchanges need big integer arithmetic that PSA
  does not expose, so they are not offered. Every current OpenSSH prefers ECDH
  anyway.
- Host keys: `ssh-ed25519`, `ecdsa-sha2-nistp256/384/521`, `rsa-sha2-512/256`.
- Ciphers: `chacha20-poly1305@openssh.com`, `aes256/192/128-ctr`.
- MACs: `hmac-sha2-256/512`, including the ETM variants.
- Deliberately absent: SHA-1 signatures, 3DES, RC4, Blowfish, MD5.

Private keys from elsewhere cannot be imported; the badge signs with the key it
generated itself.

## Layout

    main/terminal.c        Screen model and the escape sequence parser
    main/term_render.c     Drawing the grid with PAX
    main/terminal_font.c   Generated 8x16 font (tools/make_font.py)
    main/keymap.c          Board input events to terminal bytes
    main/ssh_client.c      libssh2 session, on a task of its own
    main/keystore.c        The badge's Ed25519 identity
    main/hosts.c           Saved connections and known host keys
    main/ui.c              Menu, editor, key page, terminal screen
    components/libssh2/    libssh2 plus the PSA crypto backend

## Licence

MIT, see [LICENSE](LICENSE) for this project and the third party components.
