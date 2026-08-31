# SSH for Tanmatsu and Konsool

An SSH client with a VT100/xterm terminal, for the
[Tanmatsu](https://nicolaielectronics.nl/docs/tanmatsu/) and Konsool badges.

The badge keyboard is the terminal keyboard. The screen is 100x29 characters at
8x16, or 50x14 at double size. Text is UTF-8 end to end: Latin, Greek, Cyrillic,
punctuation, arrows, maths, box drawing, block elements and Braille all render,
so `htop`, `vim` and `mc` look like themselves. East Asian characters take two
columns and are drawn from 16x16 glyphs, so kana and fullwidth text line up with
what the host thinks it sent.

## What it does

- Saved connections in NVS: host, port, user, and optionally the password,
  which the badge drops as soon as the key can do the job.
- Public key login with a key the badge generates itself. The private half stays
  in NVS; libssh2 asks for a signature and gets one, never the key. See
  [What this does not protect against](#what-this-does-not-protect-against).
- Its own `ssh-copy-id`: log in with a password once, and the badge installs its
  key on the server and uses it from then on.
- Host key checking against a remembered fingerprint, with a prompt the first
  time and a red, two-press warning when a known key changes.
- A terminal with colours (16, 256 and 24 bit), the alternate screen, scroll
  regions, insert and delete, and 512 lines of scrollback.

## Getting the public key onto a server

The easy way, from the badge: give a connection a host, a user and a password,
then press the **green ●** key on it, in the connection list or in the
editor. It logs in with the password once, appends the badge's key to that
account's `authorized_keys`, switches the connection over to key
authentication, and forgets the password — keeping both on the badge would
defeat the point. Installing the same key twice is harmless.

If you would rather carry the key across yourself, open **SSH key** from the
main menu. That page shows the fingerprint, the `authorized_keys` line and a QR
code of the same line, and offers:

| Key                 | What it does                                   |
|---------------------|------------------------------------------------|
| Red ✗ (F1)          | Write `ssh_konsool.pub` to the SD card         |
| Orange ▲ (F2)       | Write `/int/ssh/id_ed25519.pub`, for BadgeLink |
| Yellow ■ (F3)       | Print the line on the serial console           |
| Green ● (F4)        | Throw the key away and make a new one          |

The six function keys have no numbers printed on them; the badge calls them
F1..F6 internally, and the screen names them by the shape and colour on the
keycap: red ✗, orange ▲, yellow ■, green ●, blue ☁, purple ◆.

Over BadgeLink that becomes:

    badgelink.sh fs download /int/ssh/id_ed25519.pub id_badge.pub
    ssh-copy-id -f -i id_badge.pub user@host

## Keys in the terminal

| Key            | Sends                                     |
|----------------|-------------------------------------------|
| Fn + Esc       | Back to the menu (local)                  |
| Fn + red ✗     | Toggle double size (local)                |
| Shift + Up/Dn  | Scrollback (local)                        |
| Fn + Left/Rght | Home / End                                |
| Fn + Up/Down   | Page Up / Page Down                       |
| Fn + Backspace | Delete                                    |
| Fn + ✗..◆      | F7..F12                                   |
| Ctrl + letter  | The usual control codes                   |
| Alt + key      | Escape prefix (meta)                      |

## The terminal font

`main/terminal_font.c` is generated from [GNU Unifont](https://unifoundry.com/unifont/)
and the Unicode East Asian Width data, and is checked in — a plain clone needs
neither. Regenerate it after changing which blocks are covered:

    make font                # the default set, about 80 KB of glyphs
    make font FONT_CJK=1     # adds CJK unified ideographs, about 740 KB

The default covers Latin, Greek, Cyrillic, punctuation, arrows, maths, box
drawing, blocks, Braille, kana, Bopomofo, Hangul jamo and the fullwidth forms.
`FONT_CJK=1` adds U+4E00..U+9FFF, which takes the binary from about 1.29 MB to
1.96 MB — inside the 2 MB app partition, but only just, which is why it is not
the default.

Two things are kept deliberately apart in there. Glyph width is a property of
the font: Unifont draws most characters 8x16 and East Asian ones 16x16. Column
width is a property of Unicode, and is taken from the East Asian Width data,
because the host advances its own cursor using its own `wcwidth` — if we
disagreed, every line after a wide character would slide. A character with no
glyph still takes the right number of columns and draws as U+FFFD.

Hangul syllables (U+AC00..U+D7A3) would add another 350 KB and overflow the
partition, so they are not offered; the jamo are there and compose visually
badly, which is honest about the limit.

## Building

Needs ESP-IDF v6.0.2.

    make prepare          # fetch the libssh2 submodule
    make sdk              # only if you have no ESP-IDF yet
    make build            # DEVICE=tanmatsu (default) or DEVICE=konsool

Point the build at an existing SDK by writing its path to `.IDF_PATH`, or set
`IDF_PATH` in the environment.

The parts that can be tested without hardware are:

    make test

That covers the known-host store and the generated font, under the address and
undefined behaviour sanitisers.

Install over BadgeLink, which also uploads the icon and metadata:

    make badgelink        # once, to fetch the tool
    make install
    make run

Or flash over USB with `make flashmonitor PORT=/dev/ttyACM0`.

## What this does not protect against

Anyone holding the badge has the SSH key. Treat it the way you would treat a key
file on a laptop with no disk encryption.

NVS is not encrypted, so the private key and any password you asked the badge to
remember are readable by anyone who can dump the flash, and by any other app on
the badge — NVS namespaces are not access controlled. ESP-IDF can encrypt NVS,
but the key has to be protected by either flash encryption or an eFuse-backed
HMAC key, and neither stops someone who can simply run code on the badge. That
would need Secure Boot as well, which means the badge only runs firmware you
signed, and all of it is a one-way eFuse burn. For a badge you want to keep
hacking on, that is the wrong trade.

Because of that, "forget the password" (F4, or clearing the checkbox) and
"throw the key away" (F4 on the SSH key page) overwrite the NVS entry but cannot
scrub the old bytes from flash: NVS wear-levels, so a superseded password or key
can linger on unused pages until they are reused. Treat a badge that ever held a
secret as still holding it until the flash is fully erased. Revoking on the
server side, below, is what actually protects you.

So the protection is on the other side of the connection:

- The badge key is its own key. It is not a copy of the one on your laptop, and
  revoking it costs you one line in one `authorized_keys`.
- Restrict what it can do, in `authorized_keys`:

      restrict,from="10.0.0.0/8" ssh-ed25519 AAAA... ssh@tanmatsu

- Keeping a password on the badge is off by default, and the editor says what it
  costs when you turn it on. Leave it off, and let F4 do the work after the
  first login.
- Lost the badge? Delete that one line, or press F4 twice on the SSH key page to
  make a new key, which makes every copy of the old one useless.

Host keys are pinned on first sight, and a later change is reported loudly: the
prompt turns red, says the key CHANGED, and takes a deliberate second press on
either accept key — so a swapped server key cannot be accepted, or re-pinned, by
one reflexive keypress. Accepting a changed key *without* re-pinning it (`y`)
also drops the saved password for that session: the badge asks for it by hand
instead, so the stored secret never reaches a key you declined to trust.

The pin is trust-on-first-use, though. If someone is already in the middle the
very first time you connect to a host, their key is what gets pinned, silently,
because there is nothing yet to compare it against. On that first connection,
check the fingerprint the badge shows against one you got another way (the
server's `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub`).

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

The ChaCha20-Poly1305 and ETM MAC modes offered are the ones the Terrapin attack
(CVE-2023-48795) targets, so the transcript-integrity defence matters: the pinned
libssh2 (1.11.1) negotiates the `strict-kex` extension, which closes it whenever
the server also supports it, as every current OpenSSH does.

Private keys from elsewhere cannot be imported; the badge signs with the key it
generated itself.

The firmware is built with stack canaries on (`CONFIG_COMPILER_STACK_CHECK_MODE_STRONG`),
a cheap guard for the two places that parse hostile input: the SSH transport and
the terminal escape parser. Two platform defaults are worth knowing about:
`CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED` is on because the app runs execute-in-place
from the badge's AppFS partition, which the write guard would otherwise abort; and
`CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE` is off (inherited from the badge
template), which leaves the PMP W^X and invalid-region traps unprogrammed. Turning
it on is worthwhile but needs testing on real hardware first.

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
    test/host/             tests that run on a development machine

## Licence

MIT, see [LICENSE](LICENSE) for this project and the third party components.
